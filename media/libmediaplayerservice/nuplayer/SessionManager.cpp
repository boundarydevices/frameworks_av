/*
 * Copyright (C) 2016 The Android Open Source Project
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
/* Copyright (C) 2016 Freescale Semiconductor, Inc. */

#define LOG_TAG "SessoinManager"
#include <utils/Log.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/ANetworkSession.h>
#include <media/stagefright/foundation/ABuffer.h>
#include "SessionManager.h"
#include "RTPSessionManager.h"
#include "UDPSessionManager.h"
#include "GenericStreamSource.h"

#define HIGH_WATERMARK 10 * 1024 * 1024
#define LOW_WATERMARK (0)//this value should be 0 to keep latency lowest.
#define SHIFT_POINT 500 * 1024

namespace android {

NuPlayer::SessionManager::SessionManager(const char *uri, GenericStreamSource *streamSource)
    :mTotalDataSize(0),
    mDownStreamComp(streamSource),
    mNetSession(NULL),
    mUri(uri),
    mSessionID(0),
    mPort(0),
    mLooper(new ALooper),
    pauseState(false),
    bufferingState(false)
{
    ALOGV("SessionManager constructor");
}

NuPlayer::SessionManager::~SessionManager()
{
    ALOGV("SessionManager destructor");
    deInit();
}

bool NuPlayer::SessionManager::parseURI(const char *uri, AString *host, int32_t *port)
{
    host->clear();
    *port = 0;

    // skip "rtp://" and "udp://"
    host->setTo(&uri[6]);

    const char *colon = strchr(host->c_str(), ':');

    if (colon == NULL) {
        return false;
    }

    char *end;
    unsigned long x = strtoul(colon + 1, &end, 10);

    if (end == colon + 1 || *end != '\0' || x >= 65536) {
        return false;
    }

    *port = x;

    size_t colonOffset = colon - host->c_str();
    host->erase(colonOffset, host->size() - colonOffset);

    ALOGI("SessionManager::parseURI(), host %s, port %d", host->c_str(), *port);

    return true;
}

bool NuPlayer::SessionManager::init()
{
    mLooper->setName("SessoinManager");
    mLooper->start();
    mLooper->registerHandler(this);

    if(initNetwork() != OK){
        ALOGE("initialize network %s fail!", mUri);
        return false;
    }

    return true;
}

bool NuPlayer::SessionManager::deInit()
{
    if((mNetSession != NULL) && (mSessionID != 0)){
        mNetSession->stop();
        mNetSession->destroySession(mSessionID);
        mSessionID = 0;
    }

    if (mLooper != NULL) {
        mLooper->unregisterHandler(id());
        mLooper->stop();
    }

    return true;
}

status_t NuPlayer::SessionManager::initNetwork()
{
    ALOGV("SessionManager::init");
    uint32_t MAX_RETRY_TIMES = 20;
    uint32_t retry = 0;

    mNetSession = new ANetworkSession;
    status_t ret = mNetSession->start();
    if(ret != OK)
        return ret;

    sp<AMessage> notify = new AMessage(kWhatNetworkNotify, this);

    CHECK_EQ(mSessionID, 0);

    if(!parseURI(mUri, &mHost, &mPort))
        return -1;

    for (;;) {
        ret = mNetSession->createUDPSession(
                mPort,
                mHost.c_str(),
                0,
                notify,
                &mSessionID);

        if (ret == OK) {
            break;
        }

        mNetSession->destroySession(mSessionID);
        mSessionID = 0;

        if(++retry > MAX_RETRY_TIMES)
            return ret;

        usleep(500000);
        ALOGI("createUDPSession fail, retry...");

    }

    ALOGI("UDP session created, id %d", mSessionID);

    return OK;
}

void NuPlayer::SessionManager::onNetworkNotify(const sp<AMessage> &msg) {
    int32_t reason;
    CHECK(msg->findInt32("reason", &reason));

    switch (reason) {
        case ANetworkSession::kWhatError:
        {
            ALOGV("SessionManager::onNetworkNotify(): error");

            int32_t sessionID;
            CHECK(msg->findInt32("sessionID", &sessionID));

            int32_t err;
            CHECK(msg->findInt32("err", &err));

            int32_t errorOccuredDuringSend;
            CHECK(msg->findInt32("send", &errorOccuredDuringSend));

            AString detail;
            CHECK(msg->findString("detail", &detail));

            ALOGE("An error occurred during %s in session %d "
                    "(%d, '%s' (%s)).",
                    errorOccuredDuringSend ? "send" : "receive",
                    sessionID,
                    err,
                    detail.c_str(),
                    strerror(-err));

            mNetSession->destroySession(sessionID);

            if (sessionID == mSessionID) {
                mSessionID = 0;
            } else
                ALOGE("Incorrect session ID in error msg!");

            break;
        }

        case ANetworkSession::kWhatDatagram:
        {
            ALOGV("SessionManager::onNetworkNotify(): data");

            sp<ABuffer> data;
            CHECK(msg->findBuffer("data", &data));
            if(parseHeader(data) != OK){
                ALOGE("parseHeader fail, ignore this packet");
                break;
            }

            if(!pauseState){
                enqueueFilledBuffer(data);
                checkFlags(true);
            }
            else
                ALOGV("ignore this buffer because cache full");

            //ALOGI("after enqueue, total size is %d", mTotalDataSize);

            if(mDownStreamComp)
                tryOutputFilledBuffers();

            break;
        }

        case ANetworkSession::kWhatClientConnected:
        {
            int32_t sessionID;
            CHECK(msg->findInt32("sessionID", &sessionID));

            ALOGI("Received notify of session %d connected", sessionID);
            break;
        }
    }
}

void NuPlayer::SessionManager::tryOutputFilledBuffers()
{
    while(!mFilledBufferQueue.empty() && !bufferingState){
        sp<ABuffer> src = *mFilledBufferQueue.begin();
        int32_t ret = mDownStreamComp->outputFilledBuffer(src);
        ALOGV("tryOutputFilledBuffers, ret %d, src size %d", ret, src->size());
        if(ret >= (int32_t)src->size()){
            mFilledBufferQueue.erase(mFilledBufferQueue.begin());
            CHECK_GE(mTotalDataSize, src->size());
            mTotalDataSize -= src->size();
            ALOGV("after output, total size is %d", mTotalDataSize);
        }
        else if(ret <= 0){
            // no more empty buffer space
            break;
        }
        else{
            // part of this buffer is outputed
            src->setRange(src->offset() + ret, src->size() - ret);
            CHECK_GE(mTotalDataSize, ret);
            mTotalDataSize -= ret;
            ALOGV("after output, total size is %d", mTotalDataSize);
        }

        checkFlags(false);
    }

    return;
}

void NuPlayer::SessionManager::checkFlags(bool BufferIncreasing)
{
    if(BufferIncreasing){
        if(bufferingState && mTotalDataSize > (LOW_WATERMARK + SHIFT_POINT)){
            ALOGI("--- switch outof bufferingState");
            bufferingState = false;
        }

        if(mTotalDataSize > HIGH_WATERMARK && !pauseState){
            ALOGI("--- switch into pauseState");
            pauseState = true;
        }
    }
    else{
        if(pauseState && mTotalDataSize < (HIGH_WATERMARK - SHIFT_POINT)){
            ALOGI("--- switch outof pauseState");
            pauseState = false;
        }

        if(mTotalDataSize < LOW_WATERMARK && !bufferingState){
            ALOGI("--- switch into bufferingState");
            bufferingState = true;
        }
    }
}

void NuPlayer::SessionManager::onMessageReceived(
        const sp<AMessage> &msg) {

    switch (msg->what()) {
        case kWhatNetworkNotify:
        {
            onNetworkNotify(msg);
            break;
        }
        case kWhatRequireData:
        {
            ALOGV("receive message kWhatRequireData");
            if(mDownStreamComp)
                tryOutputFilledBuffers();
            break;
        }
        default:
        {
            TRESPASS();
        }
    }
}

sp<NuPlayer::SessionManager> NuPlayer::SessionManager::createSessionManager(
    const char *uri,
    GenericStreamSource * pStreamSource)
{
    sp<NuPlayer::SessionManager> smg = NULL;

    if(!strncasecmp(uri, "rtp://", 6))
        smg = new RTPSessionManager(uri, pStreamSource);
    else if(!strncasecmp(uri, "udp://", 6))
        smg = new UDPSessionManager(uri, pStreamSource);

    if(smg != NULL && smg->init())
        return smg;

    return NULL;
}


}

