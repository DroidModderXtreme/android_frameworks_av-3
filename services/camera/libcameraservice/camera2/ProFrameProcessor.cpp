/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Camera2-ProFrameProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <utils/Trace.h>

#include "ProFrameProcessor.h"
#include "../CameraDeviceBase.h"
#include "../ProCamera2Client.h"

namespace android {
namespace camera2 {

ProFrameProcessor::ProFrameProcessor(wp<ProCamera2Client> client):
        Thread(false), mClient(client) {
}

ProFrameProcessor::~ProFrameProcessor() {
    ALOGV("%s: Exit", __FUNCTION__);
}

status_t ProFrameProcessor::registerListener(int32_t minId,
        int32_t maxId, wp<FilteredListener> listener) {
    Mutex::Autolock l(mInputMutex);
    ALOGV("%s: Registering listener for frame id range %d - %d",
            __FUNCTION__, minId, maxId);
    RangeListener rListener = { minId, maxId, listener };
    mRangeListeners.push_back(rListener);
    return OK;
}

status_t ProFrameProcessor::removeListener(int32_t minId,
        int32_t maxId, wp<FilteredListener> listener) {
    Mutex::Autolock l(mInputMutex);
    List<RangeListener>::iterator item = mRangeListeners.begin();
    while (item != mRangeListeners.end()) {
        if (item->minId == minId &&
                item->maxId == maxId &&
                item->listener == listener) {
            item = mRangeListeners.erase(item);
        } else {
            item++;
        }
    }
    return OK;
}

void ProFrameProcessor::dump(int fd, const Vector<String16>& /*args*/) {
    String8 result("    Latest received frame:\n");
    write(fd, result.string(), result.size());
    mLastFrame.dump(fd, 2, 6);
}

bool ProFrameProcessor::threadLoop() {
    status_t res;

    sp<CameraDeviceBase> device;
    {
        sp<ProCamera2Client> client = mClient.promote();
        if (client == 0) return false;
        device = client->getCameraDevice();
        if (device == 0) return false;
    }

    res = device->waitForNextFrame(kWaitDuration);
    if (res == OK) {
        sp<ProCamera2Client> client = mClient.promote();
        if (client == 0) return false;
        processNewFrames(client);
    } else if (res != TIMED_OUT) {
        ALOGE("ProCamera2Client::ProFrameProcessor: Error waiting for new "
                "frames: %s (%d)", strerror(-res), res);
    }

    return true;
}

void ProFrameProcessor::processNewFrames(sp<ProCamera2Client> &client) {
    status_t res;
    ATRACE_CALL();
    CameraMetadata frame;
    while ( (res = client->getCameraDevice()->getNextFrame(&frame)) == OK) {
        camera_metadata_entry_t entry;

        entry = frame.find(ANDROID_REQUEST_FRAME_COUNT);
        if (entry.count == 0) {
            ALOGE("%s: Camera %d: Error reading frame number",
                    __FUNCTION__, client->getCameraId());
            break;
        }
        ATRACE_INT("cam2_frame", entry.data.i32[0]);

        res = processListeners(frame, client);
        if (res != OK) break;

        if (!frame.isEmpty()) {
            mLastFrame.acquire(frame);
        }
    }
    if (res != NOT_ENOUGH_DATA) {
        ALOGE("%s: Camera %d: Error getting next frame: %s (%d)",
                __FUNCTION__, client->getCameraId(), strerror(-res), res);
        return;
    }

    return;
}

status_t ProFrameProcessor::processListeners(const CameraMetadata &frame,
        sp<ProCamera2Client> &client) {
    ATRACE_CALL();
    camera_metadata_ro_entry_t entry;

    entry = frame.find(ANDROID_REQUEST_ID);
    if (entry.count == 0) {
        ALOGE("%s: Camera %d: Error reading frame id",
                __FUNCTION__, client->getCameraId());
        return BAD_VALUE;
    }
    int32_t frameId = entry.data.i32[0];

    List<sp<FilteredListener> > listeners;
    {
        Mutex::Autolock l(mInputMutex);

        List<RangeListener>::iterator item = mRangeListeners.begin();
        while (item != mRangeListeners.end()) {
            if (frameId >= item->minId &&
                    frameId < item->maxId) {
                sp<FilteredListener> listener = item->listener.promote();
                if (listener == 0) {
                    item = mRangeListeners.erase(item);
                    continue;
                } else {
                    listeners.push_back(listener);
                }
            }
            item++;
        }
    }
    ALOGV("Got %d range listeners out of %d", listeners.size(), mRangeListeners.size());
    List<sp<FilteredListener> >::iterator item = listeners.begin();
    for (; item != listeners.end(); item++) {
        (*item)->onFrameAvailable(frameId, frame);
    }
    return OK;
}

}; // namespace camera2
}; // namespace android