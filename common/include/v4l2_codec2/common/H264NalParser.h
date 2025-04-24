// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Changes from Qualcomm Technologies, Inc. are provided under the following license:
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef ANDROID_V4L2_CODEC2_COMMON_H264NALPARSER_H
#define ANDROID_V4L2_CODEC2_COMMON_H264NALPARSER_H

#include <stdint.h>

#include <v4l2_codec2/common/NalParser.h>

namespace android {

// Helper class to parse H264 NAL units from data.
class H264NalParser : public NalParser {
public:

    /**
    * H264NalUnitType:
    * @H264_NAL_UNKNOWN: Unknown nal type
    * @H264_NAL_SLICE: Slice nal
    * @H264_NAL_SLICE_DPA: DPA slice nal
    * @H264_NAL_SLICE_DPB: DPB slice nal
    * @H264_NAL_SLICE_DPC: DPC slice nal
    * @H264_NAL_SLICE_IDR: DPR slice nal
    * @H264_NAL_SEI: Supplemental enhancement information (SEI) nal unit
    * @H264_NAL_SPS: Sequence parameter set (SPS) nal unit
    * @H264_NAL_PPS: Picture parameter set (PPS) nal unit
    * @H264_NAL_AU_DELIMITER: Access unit (AU) delimiter nal unit
    * @H264_NAL_SEQ_END: End of sequence nal unit
    * @H264_NAL_STREAM_END: End of stream nal unit
    * @H264_NAL_FILLER_DATA: Filler data nal lunit
    * @H264_NAL_SPS_EXT: Sequence parameter set (SPS) extension NAL unit
    * @H264_NAL_PREFIX_UNIT: Prefix NAL unit
    * @H264_NAL_SUBSET_SPS: Subset sequence parameter set (SSPS) NAL unit
    * @H264_NAL_DEPTH_SPS: Depth parameter set (DPS) NAL unit
    * @H264_NAL_SLICE_AUX: Auxiliary coded picture without partitioning NAL unit
    * @H264_NAL_SLICE_EXT: Coded slice extension NAL unit
    * @H264_NAL_SLICE_DEPTH: Coded slice extension for depth or 3D-AVC texture view
    *
    * Indicates the type of H264 Nal Units
    */
    enum NalUnitType : uint8_t {
        H264_NAL_UNKNOWN      = 0,
        H264_NAL_SLICE        = 1,
        H264_NAL_SLICE_DPA    = 2,
        H264_NAL_SLICE_DPB    = 3,
        H264_NAL_SLICE_DPC    = 4,
        H264_NAL_SLICE_IDR    = 5,
        H264_NAL_SEI          = 6,
        H264_NAL_SPS          = 7,
        H264_NAL_PPS          = 8,
        H264_NAL_AU_DELIMITER = 9,
        H264_NAL_SEQ_END      = 10,
        H264_NAL_STREAM_END   = 11,
        H264_NAL_FILLER_DATA  = 12,
        H264_NAL_SPS_EXT      = 13,
        H264_NAL_PREFIX_UNIT  = 14,
        H264_NAL_SUBSET_SPS   = 15,
        H264_NAL_DEPTH_SPS    = 16,
        H264_NAL_SLICE_AUX    = 19,
        H264_NAL_SLICE_EXT    = 20,
        H264_NAL_SLICE_DEPTH  = 21
    };

    H264NalParser(const uint8_t* data, size_t length);
    ~H264NalParser() = default;

    // Locate the sequence parameter set (SPS).
    bool locateSPS() override;
    bool locatePPS() override;
    bool locateIDR() override;

    // Get the type of the current NAL unit.
    uint8_t type() const;

    // Find the H.264 video's color aspects in the current SPS NAL.
    bool findCodedColorAspects(ColorAspects* colorAspects) override;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMMON_H264NALPARSER_H
