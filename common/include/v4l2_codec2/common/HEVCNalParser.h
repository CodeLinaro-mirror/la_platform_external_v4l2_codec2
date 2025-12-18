// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Changes from Qualcomm Technologies, Inc. are provided under the following license:
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef ANDROID_V4L2_CODEC2_COMMON_HEVCNALPARSER_H
#define ANDROID_V4L2_CODEC2_COMMON_HEVCNALPARSER_H

#include <stdint.h>

#include <v4l2_codec2/common/NalParser.h>

namespace android {

// Helper class to parse HEVC NAL units from data.
class HEVCNalParser : public NalParser {
public:

    /**
    * H265NalUnitType:
    * @HEVC_NAL_SLICE_TRAIL_N: Slice nal of a non-TSA, non-STSA trailing picture
    * @HEVC_NAL_SLICE_TRAIL_R: Slice nal of a non-TSA, non-STSA trailing picture
    * @HEVC_NAL_SLICE_TSA_N: Slice nal of a TSA picture
    * @HEVC_NAL_SLICE_TSA_R: Slice nal of a TSA picture
    * @HEVC_NAL_SLICE_STSA_N: Slice nal of a STSA picture
    * @HEVC_NAL_SLICE_STSA_R: Slice nal of a STSA picture
    * @HEVC_NAL_SLICE_RADL_N: Slice nal of a RADL picture
    * @HEVC_NAL_SLICE_RADL_R: Slice nal of a RADL piicture
    * @HEVC_NAL_SLICE_RASL_N: Slice nal of a RASL picture
    * @HEVC_NAL_SLICE_RASL_R: Slice nal of a RASL picture
    * @HEVC_NON_IRAP_SUBLAYER_NAL_TYPE_MIN: Non-IRAP sublayer nal minimum value
    * @HEVC_NON_IRAP_SUBLAYER_NAL_TYPE_MAX: Non-IRAP sublayer nal maximum value
    * @HEVC_NAL_SLICE_BLA_W_LP: Slice nal of a BLA picture
    * @HEVC_NAL_SLICE_BLA_W_RADL: Slice nal of a BLA picture
    * @HEVC_NAL_SLICE_BLA_N_LP: Slice nal of a BLA picture
    * @HEVC_NAL_SLICE_IDR_W_RADL: Slice nal of an IDR picture
    * @HEVC_NAL_SLICE_IDR_N_LP: Slice nal of an IDR picture
    * @HEVC_NAL_SLICE_CRA_NUT: Slice nal of a CRA picture
    * @HEVC_IRAP_NAL_TYPE_MIN: IRAP nal minimum value
    * @HEVC_IRAP_NAL_TYPE_MAX: IRAP nal maximum value
    * @HEVC_NON_IRAP_NAL_TYPE_MIN: Non-IRAP nal minimum value
    * @HEVC_NON_IRAP_NAL_TYPE_MAX: Non-IRAP nal maximum value
    * @HEVC_NAL_VPS: Video parameter set(VPS) nal unit
    * @HEVC_NAL_SPS: Sequence parameter set (SPS) nal unit
    * @HEVC_NAL_PPS: Picture parameter set (PPS) nal unit
    * @HEVC_NAL_AUD: Access unit (AU) delimiter nal unit
    * @HEVC_NAL_EOS: End of sequence (EOS) nal unit
    * @HEVC_NAL_EOB: End of bitstream (EOB) nal unit
    * @HEVC_NAL_FD: Filler data (FD) nal lunit
    * @HEVC_NAL_PREFIX_SEI: Supplemental enhancement information prefix nal unit
    * @HEVC_NAL_SUFFIX_SEI: Suppliemental enhancement information suffix nal unit
    * @HEVC_NON_VCL_NAL_TYPE_MIN: Non-VCL nal minimum value
    * @HEVC_NON_VCL_NAL_TYPE_MAX: Non-VCL nal maximum value
    * @HEVC_UNSPEC_NON_VCL_NAL_TYPE_MIN: Unspecified non-VCL nal minimum value
    * @HEVC_UNSPEC_NON_VCL_NAL_TYPE_MAX: Unspecified non-VCL nal maximum value
    *
    * Indicates the type of HEVC Nal Units
    */
    enum NalUnitType : uint8_t {
        HEVC_NAL_SLICE_TRAIL_N    = 0,
        HEVC_NAL_SLICE_TRAIL_R    = 1,
        HEVC_NAL_SLICE_TSA_N      = 2,
        HEVC_NAL_SLICE_TSA_R      = 3,
        HEVC_NAL_SLICE_STSA_N     = 4,
        HEVC_NAL_SLICE_STSA_R     = 5,
        HEVC_NAL_SLICE_RADL_N     = 6,
        HEVC_NAL_SLICE_RADL_R     = 7,
        HEVC_NAL_SLICE_RASL_N     = 8,
        HEVC_NAL_SLICE_RASL_R     = 9,
        HEVC_NON_IRAP_SUBLAYER_NAL_TYPE_MIN = 10,
        HEVC_NON_IRAP_SUBLAYER_NAL_TYPE_MAX = 11,
        HEVC_NAL_SLICE_BLA_W_LP   = 16,
        HEVC_NAL_SLICE_BLA_W_RADL = 17,
        HEVC_NAL_SLICE_BLA_N_LP   = 18,
        HEVC_NAL_SLICE_IDR_W_RADL = 19,
        HEVC_NAL_SLICE_IDR_N_LP   = 20,
        HEVC_NAL_SLICE_CRA_NUT    = 21,
        HEVC_IRAP_NAL_TYPE_MIN    = 22,
        HEVC_IRAP_NAL_TYPE_MAX    = 23,
        HEVC_NON_IRAP_NAL_TYPE_MIN = 24,
        HEVC_NON_IRAP_NAL_TYPE_MAX = 31,
        HEVC_NAL_VPS              = 32,
        HEVC_NAL_SPS              = 33,
        HEVC_NAL_PPS              = 34,
        HEVC_NAL_AUD              = 35,
        HEVC_NAL_EOS              = 36,
        HEVC_NAL_EOB              = 37,
        HEVC_NAL_FD               = 38,
        HEVC_NAL_PREFIX_SEI       = 39,
        HEVC_NAL_SUFFIX_SEI       = 40,
        HEVC_NON_VCL_NAL_TYPE_MIN = 41,
        HEVC_NON_VCL_NAL_TYPE_MAX = 47,
        HEVC_UNSPEC_NON_VCL_NAL_TYPE_MIN = 48,
        HEVC_UNSPEC_NON_VCL_NAL_TYPE_MAX = 63,
    };

    HEVCNalParser();
    HEVCNalParser(const uint8_t* data, size_t length);
    ~HEVCNalParser() = default;

    bool parse() override;
    
    // Locate the sequence parameter set (SPS).
    bool locateSPS() override;
    bool locatePPS() override;
    bool locateIDR() override;

    // Get the type of the current NAL unit.
    uint8_t type() const override;

    // Find the HEVC video's color aspects in the current SPS NAL.
    bool findCodedColorAspects(ColorAspects* colorAspects) override;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMMON_HEVCNALPARSER_H
