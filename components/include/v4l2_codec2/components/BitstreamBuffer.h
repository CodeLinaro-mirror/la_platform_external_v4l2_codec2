// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Changes from Qualcomm Technologies, Inc. are provided under the following license:
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef ANDROID_V4L2_CODEC2_COMPONENTS_BITSTREAMBUFFER_H
#define ANDROID_V4L2_CODEC2_COMPONENTS_BITSTREAMBUFFER_H

#include <stdint.h>

#include <C2Buffer.h>

namespace android {

// The ConstBitstreamBuffer class can be used to store non-modifiable encoded video data.
struct ConstBitstreamBuffer {
    // Flags that trigger specific actions on codec input buffer. 
    enum StatusFlag : uint8_t {
        Init   = 1 << 0,  // Initialize input buffer. Get free buffer from queue, configure and load initial data.
        Append = 1 << 1,  // Append data to initialized buffer.
        Send   = 1 << 2   // Put buffer to input queue.
    };

    ConstBitstreamBuffer(const int64_t id, C2ConstLinearBlock dmabuf, const size_t offset,
                         const size_t size, const uint8_t *data, const uint8_t flags = 0)
          : id(id), dmabuf(std::move(dmabuf)), offset(offset), size(size), data(data), flags(flags) {}
    ~ConstBitstreamBuffer() = default;

    const int64_t id;
    C2ConstLinearBlock dmabuf;
    const size_t offset;
    const size_t size;
    const uint8_t *data;
    const uint8_t flags;
};

// The BitstreamBuffer class can be used to store modifiable encoded video data.
struct BitstreamBuffer {
    BitstreamBuffer(std::shared_ptr<C2LinearBlock> dmabuf, const size_t offset, const size_t size)
          : dmabuf(std::move(dmabuf)), offset(offset), size(size) {}
    ~BitstreamBuffer() = default;

    std::shared_ptr<C2LinearBlock> dmabuf;
    const size_t offset;
    const size_t size;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMPONENTS_BITSTREAMBUFFER_H
