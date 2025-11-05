// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Changes from Qualcomm Technologies, Inc. are provided under the following license:
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef ANDROID_V4L2_CODEC2_COMPONENTS_VIDEO_FRAME_H
#define ANDROID_V4L2_CODEC2_COMPONENTS_VIDEO_FRAME_H

#include <memory>
#include <vector>

#include <C2Buffer.h>

#include <ui/Rect.h>

namespace android {

// Wrap C2GraphicBlock and provide essential information from C2GraphicBlock.
class VideoFrame {
public:
    // Create the instance from C2GraphicBlock. return nullptr if any error occurs.
    static std::unique_ptr<VideoFrame> Create(std::shared_ptr<C2GraphicBlock> block);
    ~VideoFrame();

    // Return the file descriptors of the corresponding buffer.
    const std::vector<int>& getFDs() const;

    // Getter and setter of the visible rectangle.
    void setVisibleRect(const Rect& visibleRect);
    const Rect& getVisibleRect() const;

    // Getter and setter of the bitstream ID of the corresponding input bitstream.
    void setBitstreamId(int64_t bitstreamId);
    int64_t getBitstreamId() const;

    // Get the read-only C2GraphicBlock, should be called after calling setVisibleRect().
    C2ConstGraphicBlock getGraphicBlock();

    // Get read-write C2GraphicView.
    C2GraphicView getGraphicView();

    // Invocate writable map() to trigger TRANSFER_TO_HOST command.
    void sync();

private:
    VideoFrame(std::shared_ptr<C2GraphicBlock> block, std::vector<int> fds);

    std::shared_ptr<C2GraphicBlock> mGraphicBlock;
    std::vector<int> mFds;
    Rect mVisibleRect;
    int64_t mBitstreamId = -1;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMPONENTS_VIDEO_FRAME_H
