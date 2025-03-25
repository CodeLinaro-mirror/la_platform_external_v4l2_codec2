// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Changes from Qualcomm Technologies, Inc. are provided under the following license:
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//#define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_VIDEO
#define LOG_TAG "V4L2Decoder"

#include <v4l2_codec2/v4l2/V4L2Decoder.h>

#include <stdint.h>

#include <algorithm>
#include <vector>

#include <base/bind.h>
#include <base/files/scoped_file.h>
#include <base/memory/ptr_util.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <utils/Trace.h>

#include <v4l2_codec2/common/Common.h>
#include <v4l2_codec2/common/Fourcc.h>
#include <v4l2_codec2/common/H264NalParser.h>
#include <v4l2_codec2/common/HEVCNalParser.h>
#include <v4l2_codec2/plugin_store/DmabufHelpers.h>

namespace android {
namespace {

// Extra buffers for transmitting in the whole video pipeline.
constexpr size_t kNumExtraOutputBuffers = 4;

constexpr size_t kMinimumSupportedWidth = 96;
constexpr size_t kMinimumSupportedHeight = 96;

bool waitForDRC(const C2ConstLinearBlock& input, std::optional<VideoCodec> codec) {
    C2ReadView view = input.map().get();
    const uint8_t* pos = view.data();
    // frame type takes the (2) position in first byte of VP9  uncompressed header
    const uint8_t kVP9FrameTypeMask = 0x4;
    // frame type takes the (0) position in first byte of VP8 uncompressed header
    const uint8_t kVP8FrameTypeMask = 0x1;

    switch (*codec) {
    case VideoCodec::H264: {
        H264NalParser parser(view.data(), view.capacity());
        return parser.locateIDR();
    }
    case VideoCodec::HEVC: {
        HEVCNalParser parser(view.data(), view.capacity());
        return parser.locateIDR();
    }
    // For VP8 and VP9 it is assumed that the input buffer contains a single
    // frame that is not fragmented.
    case VideoCodec::VP9:
        // 0 - key frame; 1 - interframe
        return ((pos[0] & kVP9FrameTypeMask) == 0);
    case VideoCodec::VP8:
        // 0 - key frame; 1 - interframe;
        return ((pos[0] & kVP8FrameTypeMask) == 0);
    }

    return false;
}

uint32_t HalPixelFormatToV4L2PixFmt(HalPixelFormat pixelFormat) {
    switch (pixelFormat) {
    case HalPixelFormat::YCBCR_420_888:
    case HalPixelFormat::NV12:
        return V4L2_PIX_FMT_NV12;
    case HalPixelFormat::YV12:
        return V4L2_PIX_FMT_YVU420;
    case HalPixelFormat::RGBX_8888:
        return V4L2_PIX_FMT_RGB32;
    default:
        return 0;
    }
}

HalPixelFormat V4L2PixFmtToHalPixelFormat(uint32_t pixelFormat) {
    switch (pixelFormat) {
    case V4L2_PIX_FMT_NV12:
        return HalPixelFormat::YCBCR_420_888;
    case V4L2_PIX_FMT_YVU420:
        return HalPixelFormat::YV12;
    case V4L2_PIX_FMT_RGB32:
        return HalPixelFormat::RGBX_8888;
    default:
        return HalPixelFormat::UNKNOWN;
    }
}

}  // namespace

// static
std::unique_ptr<VideoDecoder> V4L2Decoder::Create(
        uint32_t debugStreamId, const VideoCodec& codec, const size_t inputBufferSize,
        const size_t minNumOutputBuffers, const HalPixelFormat outputPixelFormat,
         const std::shared_ptr<C2StreamColorAspectsInfo::output>& colorAspects,
        GetPoolCB getPoolCb, OutputCB outputCb, ErrorCB errorCb,
        scoped_refptr<::base::SequencedTaskRunner> taskRunner, bool isSecure) {
    std::unique_ptr<V4L2Decoder> decoder =
            ::base::WrapUnique<V4L2Decoder>(new V4L2Decoder(debugStreamId, taskRunner));
    if (!decoder->start(codec, inputBufferSize, minNumOutputBuffers, outputPixelFormat, colorAspects,
                        std::move(getPoolCb), std::move(outputCb), std::move(errorCb), isSecure)) {
        return nullptr;
    }
    return decoder;
}

V4L2Decoder::V4L2Decoder(uint32_t debugStreamId,
                         scoped_refptr<::base::SequencedTaskRunner> taskRunner)
      : mDebugStreamId(debugStreamId), mTaskRunner(std::move(taskRunner)) {
    ALOGV("%s()", __func__);

    mWeakThis = mWeakThisFactory.GetWeakPtr();
}

V4L2Decoder::~V4L2Decoder() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mWeakThisFactory.InvalidateWeakPtrs();

    // Streamoff input and output queue.
    if (mOutputQueue) {
        mOutputQueue->streamoff();
        mOutputQueue->deallocateBuffers();
        mOutputQueue = nullptr;
    }
    if (mInputQueue) {
        mInputQueue->streamoff();
        mInputQueue->deallocateBuffers();
        mInputQueue = nullptr;
    }
    mInputBuffer.reset();
    if (mDevice) {
        mDevice->stopPolling();
        mDevice = nullptr;
    }
    if (mInitialEosBuffer) {
        mInitialEosBuffer = nullptr;
    }
}

bool V4L2Decoder::start(const VideoCodec& codec, const size_t inputBufferSize,
                        const size_t minNumOutputBuffers, const HalPixelFormat outputPixelFormat,
                        const std::shared_ptr<C2StreamColorAspectsInfo::output>& outputColorAspects,
                        GetPoolCB getPoolCb, OutputCB outputCb,
                        ErrorCB errorCb, bool isSecure) {
    ATRACE_CALL();
    ALOGV("%s(codec=%s, inputBufferSize=%zu, minNumOutputBuffers=%zu)", __func__,
          VideoCodecToString(codec), inputBufferSize, minNumOutputBuffers);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    mMinNumOutputBuffers = minNumOutputBuffers;
    mOutputColorAspects = outputColorAspects;
    mOutputPixelFormat = outputPixelFormat;
    mGetPoolCb = std::move(getPoolCb);
    mOutputCb = std::move(outputCb);
    mErrorCb = std::move(errorCb);
    mCodec = codec;
    mIsSecure = isSecure;

    if (mState == State::Error) {
        ALOGE("Ignore due to error state.");
        return false;
    }

    mDevice = V4L2Device::create(mDebugStreamId);

    const uint32_t inputPixelFormat = V4L2Device::videoCodecToPixFmt(codec);
    if (!mDevice->open(V4L2Device::Type::kDecoder, inputPixelFormat)) {
        ALOGE("Failed to open device for %s", VideoCodecToString(codec));
        return false;
    }

    if (!mDevice->hasCapabilities(V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) {
        ALOGE("Device does not have VIDEO_M2M_MPLANE and STREAMING capabilities.");
        return false;
    }

    if (!sendV4L2DecoderCmd(false)) {
        ALOGE("Device does not support flushing (V4L2_DEC_CMD_STOP)");
        return false;
    }

    // Subscribe to the resolution change event.
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (mDevice->ioctl(VIDIOC_SUBSCRIBE_EVENT, &sub) != 0) {
        ALOGE("ioctl() failed: VIDIOC_SUBSCRIBE_EVENT: V4L2_EVENT_SOURCE_CHANGE");
        return false;
    }

    // Create Input/Output V4L2Queue, and setup input queue.
    mInputQueue = mDevice->getQueue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    mOutputQueue = mDevice->getQueue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    if (!mInputQueue || !mOutputQueue) {
        ALOGE("Failed to create V4L2 queue.");
        return false;
    }

    // Current implementation of AU-alignment for H264 and HEVC codecs assume possibility to 
    // combining several input works into single frame buffer. Appending data to input v4l2 buffer only works
    // for MMAP memory type where directly copying is used.  
    if (property_get_bool("ro.vendor.v4l2_codec2.decoder.use_dmabuf_inputqueue", true) &&
        (codec == VideoCodec::H264 || codec == VideoCodec::HEVC)) {
        ALOGE("Current implementation of AU-alignment for H264 and HEVC codecs required MMAP memory type for input queue.");
        return false;
    }

    mInputBuffer.reset();
    
    if (!setupInputFormat(inputPixelFormat, inputBufferSize, nullptr)) {
        ALOGE("Failed to setup input format.");
        return false;
    }
    if (!setupInitialOutput(outputColorAspects)) {
        ALOGE("Unable to setup initial output");
        return false;
    }

    if (!mDevice->startPolling(mTaskRunner,
                               ::base::BindRepeating(&V4L2Decoder::serviceDeviceTask, mWeakThis),
                               ::base::BindRepeating(&V4L2Decoder::onError, mWeakThis))) {
        ALOGE("Failed to start polling V4L2 device.");
        return false;
    }

    setState(State::Idle);
    return true;
}

bool V4L2Decoder::setupInputFormat(const uint32_t inputPixelFormat, const size_t inputBufferSize,
                                   const std::shared_ptr<C2StreamColorAspectsInfo::output>& colorAspects) {
    ALOGV("%s(inputPixelFormat=%u, inputBufferSize=%zu)", __func__, inputPixelFormat,
          inputBufferSize);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());
    const C2ColorAspectsStruct *c2ColorAspects =
        static_cast<const C2ColorAspectsStruct *>(colorAspects.get());
    // Check if the format is supported.
    std::vector<uint32_t> formats =
            mDevice->enumerateSupportedPixelformats(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    if (std::find(formats.begin(), formats.end(), inputPixelFormat) == formats.end()) {
        ALOGE("Input codec s not supported by device.");
        return false;
    }

    // Setup the input format.
    // Allocate of initial buffers with minimum supported dimension, to avoid the issue with buffer allocation by the kernel due to size constraints.
    auto format = mInputQueue->setFormat(inputPixelFormat, ui::Size(kMinimumSupportedWidth,
        kMinimumSupportedHeight), inputBufferSize, 0, c2ColorAspects);
    if (!format) {
        ALOGE("Failed to call IOCTL to set input format.");
        return false;
    }
    ALOG_ASSERT(format->fmt.pix_mp.pixelformat == inputPixelFormat);

    v4l2_memory memtype;
    if (property_get_bool("ro.vendor.v4l2_codec2.decoder.use_dmabuf_inputqueue", false)) {
        memtype = V4L2_MEMORY_DMABUF;
    } else {
        memtype = V4L2_MEMORY_MMAP;
    }

    if (mInputQueue->allocateBuffers(kNumInputBuffers, memtype) == 0) {
        ALOGE("Failed to allocate input buffer.");
        return false;
    }
    if (!mInputQueue->streamon()) {
        ALOGE("Failed to streamon input queue.");
        return false;
    }
    return true;
}

bool V4L2Decoder::setupInitialOutput(
    const std::shared_ptr<C2StreamColorAspectsInfo::output>& colorAspects) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (!setupMinimalOutputFormat(colorAspects)) {
        ALOGE("Failed to set minimal resolution for initial output buffers");
        return false;
    }

    v4l2_memory memtype;
    if (property_get_bool("ro.vendor.v4l2_codec2.decoder.use_dmabuf_outputqueue", true)) {
        memtype = V4L2_MEMORY_DMABUF;
    } else {
        memtype = V4L2_MEMORY_MMAP;
    }

    if (!startOutputQueue(1, memtype, colorAspects)) {
        ALOGE("Failed to start initialy output queue");
        return false;
    }

    std::optional<V4L2WritableBufferRef> eosBuffer = mOutputQueue->getFreeBuffer();
    if (!eosBuffer) {
        ALOGE("Failed to acquire initial EOS buffer");
        return false;
    }

    mInitialEosBuffer =
            new GraphicBuffer(mCodedSize.getWidth(), mCodedSize.getHeight(),
                              static_cast<PixelFormat>(mOutputPixelFormat),
                              GraphicBuffer::USAGE_HW_VIDEO_ENCODER, "V4L2DecodeComponent");

    if (mInitialEosBuffer->initCheck() != NO_ERROR) {
        return false;
    }

    std::vector<int> fds;
    for (size_t i = 0; i < mInitialEosBuffer->handle->numFds; i++) {
        fds.push_back(mInitialEosBuffer->handle->data[i]);
    }

    bool result;
    if (eosBuffer->memory() == V4L2_MEMORY_MMAP) {
        result = std::move(*eosBuffer).queueMMap();
    } else {
        result = std::move(*eosBuffer).queueDMABuf(fds);
    }
   
    if (!result) {
        ALOGE("Failed to queue initial EOS buffer");
        return false;
    }

    return true;
}

bool V4L2Decoder::setupMinimalOutputFormat(
    const std::shared_ptr<C2StreamColorAspectsInfo::output>& colorAspects) {
    ui::Size minResolution, maxResolution;

    const C2ColorAspectsStruct *c2ColorAspects =
        static_cast<const C2ColorAspectsStruct *>(colorAspects.get());

    for (const uint32_t& pixfmt :
         mDevice->enumerateSupportedPixelformats(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
        if (std::find(kSupportedOutputFourccs.begin(), kSupportedOutputFourccs.end(), pixfmt) ==
            kSupportedOutputFourccs.end()) {
            ALOGD("Pixel format %s is not supported, skipping...", fourccToString(pixfmt).c_str());
            continue;
        }

        mDevice->getSupportedResolution(pixfmt, &minResolution, &maxResolution);
        if (minResolution.isEmpty()) {
            minResolution.set(kMinimumSupportedWidth, kMinimumSupportedHeight);
        }

        if (mOutputQueue->setFormat(pixfmt, minResolution, 0, 0, c2ColorAspects) != std::nullopt) {
            return true;
        }
    }

    ALOGE("Failed to find supported pixel format");
    return false;
}

bool V4L2Decoder::startOutputQueue(size_t minOutputBuffersCount, enum v4l2_memory memory,
    const std::shared_ptr<C2StreamColorAspectsInfo::output>& colorAspects) {
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    const std::optional<struct v4l2_format> format = getFormatInfo();
    std::optional<size_t> numOutputBuffers = getNumOutputBuffers();
    if (!format || !numOutputBuffers) {
        return false;
    }
    *numOutputBuffers = std::max(*numOutputBuffers, minOutputBuffersCount);

    const ui::Size codedSize(format->fmt.pix_mp.width, format->fmt.pix_mp.height);
    if (!setupOutputFormat(codedSize, colorAspects)) {
        return false;
    }

    const std::optional<struct v4l2_format> adjustedFormat = getFormatInfo();
    if (!adjustedFormat) {
        return false;
    }
    mCodedSize.set(adjustedFormat->fmt.pix_mp.width, adjustedFormat->fmt.pix_mp.height);
    mVisibleRect = getVisibleRect(mCodedSize);

    ALOGI("Need %zu output buffers. coded size: %s, visible rect: %s", *numOutputBuffers,
          toString(mCodedSize).c_str(), toString(mVisibleRect).c_str());
    if (isEmpty(mCodedSize)) {
        ALOGE("Failed to get resolution from V4L2 driver.");
        return false;
    }

    if (mOutputQueue->isStreaming()) {
        mOutputQueue->streamoff();
    }
    if (mOutputQueue->allocatedBuffersCount() > 0) {
        mOutputQueue->deallocateBuffers();
    }

    mFrameAtDevice.clear();
    mBlockIdToV4L2Id.clear();
    while (!mReuseFrameQueue.empty()) {
        mReuseFrameQueue.pop();
    }

    const size_t adjustedNumOutputBuffers =
            mOutputQueue->allocateBuffers(*numOutputBuffers, memory);
    if (adjustedNumOutputBuffers == 0) {
        ALOGE("Failed to allocate output buffer.");
        return false;
    }

    ALOGV("Allocated %zu output buffers.", adjustedNumOutputBuffers);
    if (!mOutputQueue->streamon()) {
        ALOGE("Failed to streamon output queue.");
        return false;
    }

    return true;
}

void V4L2Decoder::decode(std::unique_ptr<ConstBitstreamBuffer> buffer, DecodeCB decodeCb) {
    ATRACE_CALL();
    ALOGV("%s(id=%ld)", __func__, buffer->id);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Error) {
        ALOGE("Ignore due to error state.");
        mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(decodeCb),
                                                          VideoDecoder::DecodeStatus::kError));
        return;
    }

    if (mState == State::Idle) {
        setState(State::Init);
    }

    // To determine if the DRC is pending, the access to the frame data is
    // required. It's not possible to access the frame directly for the secure
    // playback, so this check must be skipped. b/279834186
    if (!mIsSecure && mInitialEosBuffer && !mPendingDRC)
        mPendingDRC = waitForDRC(buffer->dmabuf, mCodec);

    mDecodeRequests.push(DecodeRequest(std::move(buffer), std::move(decodeCb)));
    pumpDecodeRequest();
}

void V4L2Decoder::drain(DecodeCB drainCb) {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    switch (mState) {
    case State::Idle:
    case State::Init:
        ALOGV("Nothing need to drain, ignore.");
        mTaskRunner->PostTask(
                FROM_HERE, ::base::BindOnce(std::move(drainCb), VideoDecoder::DecodeStatus::kOk));
        if (mInputBuffer) {
            // Reset current input buffer pointer.
            mInputBuffer.reset();
        }
        return;

    case State::Decoding:
        mDecodeRequests.push(DecodeRequest(nullptr, std::move(drainCb)));
        pumpDecodeRequest();
        return;

    case State::Draining:
    case State::Error:
        ALOGE("Ignore due to wrong state: %s", StateToString(mState));
        mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(drainCb),
                                                          VideoDecoder::DecodeStatus::kError));
        return;
    }
}

void V4L2Decoder::pumpDecodeRequest() {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if ((mState != State::Decoding) && (mState != State::Init)) return;

    while (!mDecodeRequests.empty()) {
        // Drain the decoder.
        if (mDecodeRequests.front().buffer == nullptr) {
            ALOGV("Get drain request.");

            // If output queue is not streaming, then device is unable to notify
            // whenever draining is finished. (EOS frame cannot be dequeued).
            // This is likely to happen in the event of that the first resolution
            // change event wasn't dequeued before the drain request.
            if (!mOutputQueue->isStreaming()) {
                ALOGV("Wait for output queue to start streaming");
                return;
            }

            auto request = std::move(mDecodeRequests.front());
            mDecodeRequests.pop();

            // There is one more case that EOS frame cannot be dequeued because
            // the first resolution change event wasn't dequeued before - output
            // queues on the host are not streaming but ARCVM has no knowledge about
            // it. Check if first resolution change event was received and if there
            // was no previously sent non-empty frame (other than SPS/PPS/EOS) that
            // may trigger config from host side.
            // Drain can only be finished if we are sure there was no stream = no
            // single frame in the stack.
            if (mInitialEosBuffer && !mPendingDRC) {
                ALOGV("Terminate drain, because there was no stream");
                mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(std::move(request.decodeCb),
                                                                  VideoDecoder::DecodeStatus::kOk));
                return;
            }

            if (!sendV4L2DecoderCmd(false)) {
                std::move(request.decodeCb).Run(VideoDecoder::DecodeStatus::kError);
                onError();
                return;
            }
            mDrainCb = std::move(request.decodeCb);
            setState(State::Draining);
            return;
        }

        auto request = std::move(mDecodeRequests.front());
        auto flags = request.buffer->flags;
        int64_t bitstreamId = request.buffer->id;

        // Initialized input buffer.
        if (flags & ConstBitstreamBuffer::StatusFlag::Init) {
            auto dma_buf_id = getDmabufId(request.buffer->dmabuf.handle()->data[0]);
            if (!dma_buf_id) {
                ALOGE("Failed to get dmabuf id");
                onError();
                return;
            }
            size_t targetIndex = 0;
            // If there's an existing input buffer for this dma buffer, use it.
            for (; targetIndex < mNextInputBufferId; targetIndex++) {
                if (mLastDmaBufferId[targetIndex] == dma_buf_id) {
                    break;
                }
            }

            if (targetIndex < kNumInputBuffers) {
                // If we didn't find a buffer and there is an unused buffer, use that one.
                if (targetIndex == mNextInputBufferId) {
                    mNextInputBufferId++;
                }

                mInputBuffer = mInputQueue->getFreeBuffer(targetIndex);
            }

            // If we didn't find a reusable/unused input buffer, clobber a free one.
            if (!mInputBuffer) {
                mInputBuffer = mInputQueue->getFreeBuffer();
            }

            // Pause if no free input buffer. We resume decoding after dequeueing input buffers.
            if (!mInputBuffer) {
                ALOGV("There is no free input buffer.");
                return;
            }

            mLastDmaBufferId[mInputBuffer->bufferId()] = *dma_buf_id;

            ALOGV("Prepare input buffer, bitstreamId=%ld", bitstreamId);
            mInputBuffer->setTimeStamp(
            {.tv_sec = static_cast<time_t>(bitstreamId / ::base::Time::kMicrosecondsPerSecond),
            .tv_usec = static_cast<time_t>(bitstreamId % ::base::Time::kMicrosecondsPerSecond)});
            size_t planeSize = mInputBuffer->getPlaneSize(0);
            if (request.buffer->size > planeSize) {
                ALOGE("The input size (%zu) is not enough, we need %zu", planeSize,
                    request.buffer->size);
                onError();
                return;
            }

            ALOGV("Set bytes_used=%zu, offset=%zu", request.buffer->offset + request.buffer->size,
            request.buffer->offset);

            mInputBuffer->setPlaneDataOffset(0, request.buffer->offset);
            mInputBuffer->setPlaneBytesUsed(0, request.buffer->offset + request.buffer->size);

            if (mInputBuffer->memory() == V4L2_MEMORY_MMAP) {
                mInputBuffer->fillBuffer(request.buffer->data, request.buffer->offset, request.buffer->size);
            }
        } else {
            if (!mInputBuffer) {
                ALOGE("There is no initialized input buffer.");
                onError();
                return;
            }
        }

        mDecodeRequests.pop();

        // Append data to initialized buffer.
        if (flags & ConstBitstreamBuffer::StatusFlag::Append) {
            if (mInputBuffer->memory() == V4L2_MEMORY_MMAP) {
                // Get bitstreamId of current input buffer.
                bitstreamId = static_cast<int64_t>(
                    mInputBuffer->getTimeStamp().tv_usec +
                    mInputBuffer->getTimeStamp().tv_sec * ::base::Time::kMicrosecondsPerSecond);
                ALOGV("Append input buffer, bitstreamId=%ld", bitstreamId);
                size_t planeSize = mInputBuffer->getPlaneSize(0);
                if (mInputBuffer->getPlaneBytesUsed(0) + request.buffer->size > planeSize) {
                    ALOGE("The input size (%zu) is not enough, we need %zu", planeSize,
                        mInputBuffer->getPlaneBytesUsed(0) + request.buffer->size);
                    onError();
                    return;
                }

                ALOGV("Append bytes_used=%zu", request.buffer->size);

                mInputBuffer->fillBuffer(request.buffer->data, mInputBuffer->getPlaneBytesUsed(0), request.buffer->size);
                mInputBuffer->setPlaneBytesUsed(0, mInputBuffer->getPlaneBytesUsed(0) + request.buffer->size);
            } else {
                ALOGE("Appending input buffer works only for MMAP memory type");
            }
        }

        // Put buffer to input queue.
        if (flags & ConstBitstreamBuffer::StatusFlag::Send) {
            // Get bitstreamId of current input buffer.
            bitstreamId = static_cast<int64_t>(
                mInputBuffer->getTimeStamp().tv_usec +
                mInputBuffer->getTimeStamp().tv_sec * ::base::Time::kMicrosecondsPerSecond);
            ALOGV("QBUF to input queue, bitstreamId=%ld", bitstreamId);
            bool result;
            if (mInputBuffer->memory() == V4L2_MEMORY_MMAP) {
                result = std::move(*mInputBuffer).queueMMap();
            } else {
                std::vector<int> fds;
                fds.push_back(std::move(request.buffer->dmabuf.handle()->data[0]));
                result = std::move(*mInputBuffer).queueDMABuf(fds);
            }

            if (!result) {
                ALOGE("%s(): Failed to QBUF to input queue, bitstreamId=%ld", __func__, bitstreamId);
                onError();
                return;
            }

            mPendingDecodeCbs.insert(std::make_pair(bitstreamId, std::move(request.decodeCb)));

            if (mState == State::Init) {
                setState(State::Decoding);
            }

            // Reset current input buffer pointer.
            mInputBuffer.reset();
        }
    }
}

void V4L2Decoder::flush() {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Error) {
        ALOGE("Ignore due to error state.");
        return;
    }

    // Call all pending callbacks.
    for (auto& item : mPendingDecodeCbs) {
        std::move(item.second).Run(VideoDecoder::DecodeStatus::kAborted);
    }
    mPendingDecodeCbs.clear();
    if (mDrainCb) {
        std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kAborted);
    }

    // Streamoff both V4L2 queues to drop input and output buffers.
    const bool isOutputStreaming = mOutputQueue->isStreaming();
    mDevice->stopPolling();
    mOutputQueue->streamoff();

    // Extract currently enqueued output picture buffers to be queued later first.
    // See b/270003218 and b/297228544.
    for (auto& [v4l2Id, frame] : mFrameAtDevice) {
        // Find corresponding mapping block ID (DMABUF ID) to V4L2 buffer ID.
        // The buffer was enqueued to device therefore such mapping have to exist.
        auto blockIdIter =
                std::find_if(mBlockIdToV4L2Id.begin(), mBlockIdToV4L2Id.end(),
                             [v4l2Id = v4l2Id](const auto& el) { return el.second == v4l2Id; });

        ALOG_ASSERT(blockIdIter != mBlockIdToV4L2Id.end());
        size_t blockId = blockIdIter->first;
        mReuseFrameQueue.push(std::make_pair(blockId, std::move(frame)));
    }
    mFrameAtDevice.clear();

    mInputQueue->streamoff();

    // Streamon both V4L2 queues.
    mInputQueue->streamon();
    if (isOutputStreaming) {
        mOutputQueue->streamon();
    }

    // If there is no free buffer at mOutputQueue, tryFetchVideoFrame() should be triggerred after
    // a buffer is DQBUF from output queue. Now all the buffers are dropped at mOutputQueue, we
    // have to trigger tryFetchVideoFrame() here.
    if (mVideoFramePool) {
        tryFetchVideoFrame();
    }

    if (!mDevice->startPolling(mTaskRunner,
                               ::base::BindRepeating(&V4L2Decoder::serviceDeviceTask, mWeakThis),
                               ::base::BindRepeating(&V4L2Decoder::onError, mWeakThis))) {
        ALOGE("Failed to start polling V4L2 device.");
        onError();
        return;
    }

    setState(State::Idle);
}

bool V4L2Decoder::dequeueInputQueue() {
    bool inputDequeued = false;
    while (mInputQueue->queuedBuffersCount() > 0) {
        bool success;
        V4L2ReadableBufferRef dequeuedBuffer;
        std::tie(success, dequeuedBuffer) = mInputQueue->dequeueBuffer();
        if (!success) {
            ALOGE("Failed to dequeue buffer from input queue.");
            onError();
            return false;
        }
        if (!dequeuedBuffer) break;

        inputDequeued = true;

        // Run the corresponding decode callback.
        int64_t id = static_cast<int64_t>(
                dequeuedBuffer->getTimeStamp().tv_usec +
                dequeuedBuffer->getTimeStamp().tv_sec * ::base::Time::kMicrosecondsPerSecond);
        ALOGV("DQBUF from input queue, bitstreamId=%ld", id);
        auto it = mPendingDecodeCbs.find(id);
        if (it == mPendingDecodeCbs.end()) {
            ALOGW("Callback is already abandoned.");
            continue;
        }
        std::move(it->second).Run(VideoDecoder::DecodeStatus::kOk);
        mPendingDecodeCbs.erase(it);
    }

    return inputDequeued;
}

void V4L2Decoder::serviceDeviceTask(bool event) {
    ATRACE_CALL();
    ALOGV("%s(event=%d) state=%s InputQueue(%s):%zu+%zu/%zu, OutputQueue(%s):%zu+%zu/%zu", __func__,
          event, StateToString(mState), (mInputQueue->isStreaming() ? "streamon" : "streamoff"),
          mInputQueue->freeBuffersCount(), mInputQueue->queuedBuffersCount(),
          mInputQueue->allocatedBuffersCount(),
          (mOutputQueue->isStreaming() ? "streamon" : "streamoff"),
          mOutputQueue->freeBuffersCount(), mOutputQueue->queuedBuffersCount(),
          mOutputQueue->allocatedBuffersCount());

    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == State::Error) return;

    // Dequeue output and input queue.
    bool inputDequeued = dequeueInputQueue();

    bool outputDequeued = false;
    while ((mOutputQueue->queuedBuffersCount() > 0) && mVideoFramePool) {
        bool success;
        V4L2ReadableBufferRef dequeuedBuffer;
        std::tie(success, dequeuedBuffer) = mOutputQueue->dequeueBuffer();
        if (!success) {
            ALOGE("Failed to dequeue buffer from output queue.");
            onError();
            return;
        }
        if (!dequeuedBuffer) break;

        outputDequeued = true;

        const size_t bufferId = dequeuedBuffer->bufferId();
        const int64_t bitstreamId = static_cast<int64_t>(
                dequeuedBuffer->getTimeStamp().tv_usec +
                dequeuedBuffer->getTimeStamp().tv_sec * ::base::Time::kMicrosecondsPerSecond);
        const size_t bytesUsed = dequeuedBuffer->getPlaneBytesUsed(0);
        const bool isLast = dequeuedBuffer->isLast();
        ALOGV("DQBUF from output queue, bufferId=%zu, bitstreamId=%ld, bytesused=%zu, isLast=%d",
              bufferId, bitstreamId, bytesUsed, isLast);

        // Get the corresponding VideoFrame of the dequeued buffer.
        auto it = mFrameAtDevice.find(bufferId);
        ALOG_ASSERT(it != mFrameAtDevice.end(), "buffer %zu is not found at mFrameAtDevice",
                    bufferId);
        auto frame = std::move(it->second);
        mFrameAtDevice.erase(it);

        if (bytesUsed > 0) {
            if (dequeuedBuffer->memory() == V4L2_MEMORY_MMAP) {
                C2GraphicView graphicView = frame->getGraphicView();
                if (graphicView.error() != C2_OK) {
                    ALOGE("%s(): Get graphic view err = %d", __func__, graphicView.error());
                    onError();
                    return;
                }
                memcpy(*graphicView.data(), dequeuedBuffer->getPlaneMapping(0), bytesUsed);
            }
            ALOGV("Send output frame(bitstreamId=%ld) to client", bitstreamId);
            frame->setBitstreamId(bitstreamId);
            frame->setVisibleRect(mVisibleRect);
            mOutputCb.Run(std::move(frame));
        } else {
            // Workaround(b/168750131): If the buffer is not enqueued before the next drain is done,
            // then the driver will fail to notify EOS. So we recycle the buffer immediately.
            ALOGV("Recycle empty buffer %zu back to V4L2 output queue.", bufferId);
            dequeuedBuffer.reset();
            auto outputBuffer = mOutputQueue->getFreeBuffer(bufferId);
            ALOG_ASSERT(outputBuffer, "V4L2 output queue slot %zu is not freed.", bufferId);

            bool result;
            if (outputBuffer->memory() == V4L2_MEMORY_MMAP) {
                result = std::move(*outputBuffer).queueMMap();
            } else {
                result = std::move(*outputBuffer).queueDMABuf(frame->getFDs());
            }

            if (!result) {
                ALOGE("%s(): Failed to recycle empty buffer to output queue.", __func__);
                onError();
                return;
            }

            mFrameAtDevice.insert(std::make_pair(bufferId, std::move(frame)));
        }

        if (mDrainCb && isLast) {
            // Dequeue remaining input buffers before drain completed.
            dequeueInputQueue();
            ALOGV("All buffers are drained.");
            sendV4L2DecoderCmd(true);
            std::move(mDrainCb).Run(VideoDecoder::DecodeStatus::kOk);
            setState(State::Idle);
        }
    }

    // Handle resolution change event.
    if (event && dequeueResolutionChangeEvent()) {
        if (!changeResolution()) {
            onError();
            return;
        }
    }

    // We freed some input buffers, continue handling decode requests.
    if (inputDequeued) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::BindOnce(&V4L2Decoder::pumpDecodeRequest, mWeakThis));
    }
    // We free some output buffers, try to get VideoFrame.
    if (outputDequeued) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::BindOnce(&V4L2Decoder::tryFetchVideoFrame, mWeakThis));
    }
}

bool V4L2Decoder::dequeueResolutionChangeEvent() {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_event ev;
    memset(&ev, 0, sizeof(ev));
    while (mDevice->ioctl(VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE &&
            ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION) {
            return true;
        }
    }
    return false;
}

bool V4L2Decoder::changeResolution() {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mInitialEosBuffer) {
        mInitialEosBuffer = nullptr;
    }

    v4l2_memory memtype;
    if (property_get_bool("ro.vendor.v4l2_codec2.decoder.use_dmabuf_outputqueue", true)) {
        memtype = V4L2_MEMORY_DMABUF;
    } else {
        memtype = V4L2_MEMORY_MMAP;
    }

    if (!startOutputQueue(mMinNumOutputBuffers, memtype, mOutputColorAspects)) {
        ALOGE("Failed to start output queue during DRC.");
        return false;
    }

    // If drain request is pending then it means that previous call to pumpDecodeRequest
    // stalled the request, bacause there was no way of notifing the component that
    // drain has finished. Send this request the drain to device.
    if (!mDecodeRequests.empty() && mDecodeRequests.front().buffer == nullptr) {
        mTaskRunner->PostTask(FROM_HERE,
                              ::base::BindOnce(&V4L2Decoder::pumpDecodeRequest, mWeakThis));
    }

    // Release the previous VideoFramePool before getting a new one to guarantee only one pool
    // exists at the same time.
    mVideoFramePool.reset();

    const std::optional<struct v4l2_format> adjustedFormat = getFormatInfo();
    if (!adjustedFormat) {
        ALOGE("Failed to get format info");
        return false;
    }
    
    mVideoFramePool = 
        mGetPoolCb.Run(mCodedSize, V4L2PixFmtToHalPixelFormat(adjustedFormat->fmt.pix_mp.pixelformat), mOutputQueue->allocatedBuffersCount());

    if (!mVideoFramePool) {
        ALOGE("Failed to get block pool with size: %s", toString(mCodedSize).c_str());
        return false;
    }

    tryFetchVideoFrame();
    return true;
}

bool V4L2Decoder::setupOutputFormat(const ui::Size& size,
    const std::shared_ptr<C2StreamColorAspectsInfo::output>& colorAspects) {
    uint32_t expectedOutputFormatPixel = HalPixelFormatToV4L2PixFmt(mOutputPixelFormat);
    uint32_t outputFormatPixel = 0;
    for (const uint32_t& pixfmt :
        mDevice->enumerateSupportedPixelformats(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)) {
        if (std::find(kSupportedOutputFourccs.begin(), kSupportedOutputFourccs.end(), pixfmt) ==
            kSupportedOutputFourccs.end()) {
            ALOGD("Pixel format %s is not supported, skipping...", fourccToString(pixfmt).c_str());
            continue;
        }
        if (!outputFormatPixel) {
            outputFormatPixel = pixfmt;
        }
        
        if (expectedOutputFormatPixel == pixfmt) {
            outputFormatPixel = pixfmt;
            ALOGD("Expected pixel format %s is supported", fourccToString(pixfmt).c_str());
            break;
        }
    }

    if (outputFormatPixel) {
        const C2ColorAspectsStruct *c2ColorAspects =
            static_cast<const C2ColorAspectsStruct *>(colorAspects.get());

        if (mOutputQueue->setFormat(outputFormatPixel, size, 0, 0, c2ColorAspects) != std::nullopt) {
            return true;
        }
        else {
            ALOGE("Failed to set output format");
            return false;
        }
    }

    ALOGE("Failed to find supported pixel format");
    return false;
}

void V4L2Decoder::tryFetchVideoFrame() {
    ATRACE_CALL();
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (!mVideoFramePool) {
        ALOGE("mVideoFramePool is null, failed to get the instance after resolution change?");
        onError();
        return;
    }

    if (mOutputQueue->freeBuffersCount() == 0) {
        ALOGV("No free V4L2 output buffers, ignore.");
        return;
    }

    if (mReuseFrameQueue.empty()) {
        if (!mVideoFramePool->getVideoFrame(
                    ::base::BindOnce(&V4L2Decoder::onVideoFrameReady, mWeakThis))) {
            ALOGV("%s(): Previous callback is running, ignore.", __func__);
        }

        return;
    }

    // Reuse output picture buffers that were abandoned after STREAMOFF first.
    // NOTE(b/270003218 and b/297228544): This avoids issues with lack of
    // ability to return all picture buffers on STREAMOFF from VDA and
    // saves on IPC with BufferQueue increasing overall responsiveness.
    uint32_t blockId = mReuseFrameQueue.front().first;
    std::unique_ptr<VideoFrame> frame = std::move(mReuseFrameQueue.front().second);
    mReuseFrameQueue.pop();

    // Avoid recursive calls
    mTaskRunner->PostTask(FROM_HERE, ::base::BindOnce(&V4L2Decoder::onVideoFrameReady, mWeakThis,
                                                      std::make_pair(std::move(frame), blockId)));
}

void V4L2Decoder::onVideoFrameReady(
        std::optional<VideoFramePool::FrameWithBlockId> frameWithBlockId) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (!frameWithBlockId) {
        ALOGE("Got nullptr VideoFrame.");
        onError();
        return;
    }

    // Unwrap our arguments.
    std::unique_ptr<VideoFrame> frame;
    uint32_t blockId;
    std::tie(frame, blockId) = std::move(*frameWithBlockId);

    std::optional<V4L2WritableBufferRef> outputBuffer;
    // Find the V4L2 buffer that is associated with this block.
    auto iter = mBlockIdToV4L2Id.find(blockId);
    if (iter != mBlockIdToV4L2Id.end()) {
        // If we have met this block in the past, reuse the same V4L2 buffer.
        outputBuffer = mOutputQueue->getFreeBuffer(iter->second);
        if (!outputBuffer) {
            // NOTE(b/281477122): There is a bug in C2BufferQueueBlock. Its buffer queue slots
            // cache is inconsistent when MediaSync is used and a buffer with the same dmabuf id
            // can be returned twice despite being already in use by V4L2Decoder. We drop the
            // buffer here in order to prevent unwanted errors. It is safe, bacause its allocation
            // will be kept alive by the C2GraphicBlock instance.
            ALOGW("%s(): The frame have been supplied again, despite being already enqueued",
                  __func__);
            tryFetchVideoFrame();
            return;
        }
    } else if (mBlockIdToV4L2Id.size() < mOutputQueue->allocatedBuffersCount()) {
        // If this is the first time we see this block, give it the next
        // available V4L2 buffer.
        const size_t v4l2BufferId = mBlockIdToV4L2Id.size();
        mBlockIdToV4L2Id.emplace(blockId, v4l2BufferId);
        outputBuffer = mOutputQueue->getFreeBuffer(v4l2BufferId);
        ALOG_ASSERT(v4l2BufferId == outputBuffer->bufferId());
    } else {
        // If this happens, this is a bug in VideoFramePool. It should never
        // provide more blocks than we have V4L2 buffers.
        ALOGE("Got more different blocks than we have V4L2 buffers for.");
    }

    if (!outputBuffer) {
        ALOGE("V4L2 buffer not available. blockId=%u", blockId);
        onError();
        return;
    }

    uint32_t v4l2Id = outputBuffer->bufferId();
    ALOGV("QBUF to output queue, blockId=%u, V4L2Id=%u", blockId, v4l2Id);

    bool result;
    if (outputBuffer->memory() == V4L2_MEMORY_MMAP) {
        result = std::move(*outputBuffer).queueMMap();
    } else {
        result = std::move(*outputBuffer).queueDMABuf(frame->getFDs());
    }

    if (!result) {
        ALOGE("%s(): Failed to QBUF to output queue, blockId=%u, V4L2Id=%u", __func__, blockId,
              v4l2Id);
        onError();
        return;
    }

    if (mFrameAtDevice.find(v4l2Id) != mFrameAtDevice.end()) {
        ALOGE("%s(): V4L2 buffer %d already enqueued.", __func__, v4l2Id);
        onError();
        return;
    }
    mFrameAtDevice.insert(std::make_pair(v4l2Id, std::move(frame)));

    tryFetchVideoFrame();
}

std::optional<size_t> V4L2Decoder::getNumOutputBuffers() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if (mDevice->ioctl(VIDIOC_G_CTRL, &ctrl) != 0) {
        ALOGE("ioctl() failed: VIDIOC_G_CTRL");
        return std::nullopt;
    }
    ALOGV("%s() V4L2_CID_MIN_BUFFERS_FOR_CAPTURE returns %u", __func__, ctrl.value);

    return ctrl.value + kNumExtraOutputBuffers;
}

std::optional<struct v4l2_format> V4L2Decoder::getFormatInfo() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (mDevice->ioctl(VIDIOC_G_FMT, &format) != 0) {
        ALOGE("ioctl() failed: VIDIOC_G_FMT");
        return std::nullopt;
    }

    return format;
}

Rect V4L2Decoder::getVisibleRect(const ui::Size& codedSize) {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_rect* visible_rect = nullptr;
    struct v4l2_selection selection_arg;
    memset(&selection_arg, 0, sizeof(selection_arg));
    selection_arg.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection_arg.target = V4L2_SEL_TGT_COMPOSE;

    if (mDevice->ioctl(VIDIOC_G_SELECTION, &selection_arg) == 0) {
        ALOGV("VIDIOC_G_SELECTION is supported");
        visible_rect = &selection_arg.r;
    } else {
        ALOGV("Fallback to VIDIOC_G_CROP");
        struct v4l2_crop crop_arg;
        memset(&crop_arg, 0, sizeof(crop_arg));
        crop_arg.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        if (mDevice->ioctl(VIDIOC_G_CROP, &crop_arg) != 0) {
            ALOGW("ioctl() VIDIOC_G_CROP failed");
            return Rect(codedSize.width, codedSize.height);
        }
        visible_rect = &crop_arg.c;
    }

    Rect rect(visible_rect->left, visible_rect->top, visible_rect->left + visible_rect->width,
              visible_rect->top + visible_rect->height);
    ALOGV("visible rectangle is %s", toString(rect).c_str());
    if (!contains(Rect(codedSize.width, codedSize.height), rect)) {
        ALOGW("visible rectangle %s is not inside coded size %s", toString(rect).c_str(),
              toString(codedSize).c_str());
        return Rect(codedSize.width, codedSize.height);
    }
    if (rect.isEmpty()) {
        ALOGW("visible size is empty");
        return Rect(codedSize.width, codedSize.height);
    }

    return rect;
}

bool V4L2Decoder::sendV4L2DecoderCmd(bool start) {
    ALOGV("%s(start=%d)", __func__, start);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    struct v4l2_decoder_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = start ? V4L2_DEC_CMD_START : V4L2_DEC_CMD_STOP;
    if (mDevice->ioctl(VIDIOC_DECODER_CMD, &cmd) != 0) {
        ALOGE("ioctl() VIDIOC_DECODER_CMD failed: start=%d", start);
        return false;
    }

    return true;
}

void V4L2Decoder::onError() {
    ALOGV("%s()", __func__);
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    setState(State::Error);
    mErrorCb.Run();
}

void V4L2Decoder::setState(State newState) {
    ALOGV("%s(%s)", __func__, StateToString(newState));
    ALOG_ASSERT(mTaskRunner->RunsTasksInCurrentSequence());

    if (mState == newState) return;
    if (mState == State::Error) {
        ALOGV("Already in Error state.");
        return;
    }

    switch (newState) {
    case State::Idle:
        break;
    case State::Init:
        break;
    case State::Decoding:
        break;
    case State::Draining:
        if (mState != State::Decoding) newState = State::Error;
        break;
    case State::Error:
        break;
    }

    ALOGI("Set state %s => %s", StateToString(mState), StateToString(newState));
    mState = newState;
}

// static
const char* V4L2Decoder::StateToString(State state) {
    switch (state) {
    case State::Idle:
        return "Idle";
    case State::Init:
        return "Init";
    case State::Decoding:
        return "Decoding";
    case State::Draining:
        return "Draining";
    case State::Error:
        return "Error";
    }
}

}  // namespace android
