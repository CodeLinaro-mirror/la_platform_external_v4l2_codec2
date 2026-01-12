// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Changes from Qualcomm Technologies, Inc. are provided under the following license:
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

//#define LOG_NDEBUG 0
#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
#define LOG_TAG "android.hardware.media.c2-service-v4l2"
#else
#error "V4L2_CODEC2_SERVICE_V4L2_STORE has to be defined"
#endif

#include <C2Component.h>
#include <base/logging.h>
#include <codec2/aidl/ComponentStore.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>
#include <minijail.h>

#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
#include <v4l2_codec2/v4l2/V4L2ComponentStore.h>
#endif

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-default-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-extended-seccomp_policy";

int main(int /* argc */, char** /* argv */) {
    ALOGD("Service starting...");

    signal(SIGPIPE, SIG_IGN);
    android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);
    
    // Configure and start AIDL (Binder) thread pool
    ABinderProcess_setThreadPoolMaxThreadCount(16);
    ABinderProcess_startThreadPool();

#if LOG_NDEBUG == 0
    ALOGD("Enable all verbose logging of libchrome");
    logging::SetMinLogLevel(-5);
#endif

    // Create IComponentStore service.
    {
        using aidl::android::hardware::media::c2::IComponentStore;
        using aidl::android::hardware::media::c2::utils::ComponentStore;
        std::shared_ptr<IComponentStore> store = nullptr;

#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
        ALOGD("Instantiating Codec2's V4L2 IComponentStore service...");
        // Create the underlying C2ComponentStore (V4L2)
        std::shared_ptr<C2ComponentStore> c2Store = android::V4L2ComponentStore::Create();
        // Wrap it with the AIDL service implementation
        store = ndk::SharedRefBase::make<ComponentStore>(c2Store);
#endif

        if (store == nullptr) {
            ALOGE("Cannot create Codec2's IComponentStore service.");
        } else {
            // Register with service manager:
            // instance name: "android.hardware.media.c2.IComponentStore/default"
            const std::string instance =
                    std::string(IComponentStore::descriptor) + "/default";

            binder_status_t status =
                    AServiceManager_addService(store->asBinder().get(), instance.c_str());

            if (status != STATUS_OK) {
                ALOGE("Cannot register Codec2's IComponentStore service: status=%d", status);
            } else {
                ALOGI("Codec2's IComponentStore service registered as %s", instance.c_str());
            }
        }
    }

    // Start processing incoming Binder RPCs (blocks)
    ABinderProcess_joinThreadPool();

    ALOGD("Service shutdown.");
    return 0;
}
