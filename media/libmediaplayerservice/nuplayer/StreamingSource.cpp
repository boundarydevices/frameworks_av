/*
 * Copyright (C) 2010 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "StreamingSource"
#include <utils/Log.h>

#include <cutils/properties.h>
#include "StreamingSource.h"

#include "ATSParser.h"
#include "AnotherPacketSource.h"
#include "NuPlayerStreamListener.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>

namespace android {

NuPlayer::StreamingSource::StreamingSource(
        const sp<AMessage> &notify,
        const sp<IStreamSource> &source)
    : Source(notify),
      mSource(source),
      mLatencyLowest(0),
      mStasticPeroid(0),
      mLatencyThreshold(LATENCY_THRESHOLD_DEFAULT),
      mDropEndTimeUs(-1),
      mPipeLineLatencyUs(-1),
      mResumeCheckTimeUs(-1),
      mPrevDropTimeUs(-1),
      mTunnelRenderLatency(-1),
      mPositionUs(-1),
      mAnchorTimeRealUs(-1),
      mFinalResult(OK) {
}

NuPlayer::StreamingSource::~StreamingSource() {
}

void NuPlayer::StreamingSource::prepareAsync() {
    notifyVideoSizeChanged(0, 0);
    notifyFlagsChanged(0);
    notifyPrepared();
}

void NuPlayer::StreamingSource::start() {
    mStreamListener = new NuPlayerStreamListener(mSource, 0);

    uint32_t sourceFlags = mSource->flags();

    uint32_t parserFlags = ATSParser::TS_TIMESTAMPS_ARE_ABSOLUTE;
    if (sourceFlags & IStreamSource::kFlagAlignedVideoData) {
        parserFlags |= ATSParser::ALIGNED_VIDEO_DATA;
    }

    mTSParser = new ATSParser(parserFlags);

    mStreamListener->start();
}

status_t NuPlayer::StreamingSource::feedMoreTSData() {
    if (mFinalResult != OK) {
        return mFinalResult;
    }

    for (int32_t i = 0; i < 100; ++i) {
        char buffer[188];
        sp<AMessage> extra;
        ssize_t n = mStreamListener->read(buffer, sizeof(buffer), &extra);
        if (n == 0) {
            ALOGI("input data EOS reached.");
            mTSParser->signalEOS(ERROR_END_OF_STREAM);
            mFinalResult = ERROR_END_OF_STREAM;
            break;
        } else if (n == INFO_DISCONTINUITY) {
            int32_t type = ATSParser::DISCONTINUITY_SEEK;

            int32_t mask;
            if (extra != NULL
                    && extra->findInt32(
                        IStreamListener::kKeyDiscontinuityMask, &mask)) {
                if (mask == 0) {
                    ALOGE("Client specified an illegal discontinuity type.");
                    return ERROR_UNSUPPORTED;
                }

                type = mask;
            }

            int64_t mTunnelRenderLatencyValue;
            if (extra != NULL
                    && extra->findInt64("tunnel-render-latency", &mTunnelRenderLatencyValue)) {
                if (mTunnelRenderLatencyValue > 0) {
                    mTunnelRenderLatency = mTunnelRenderLatencyValue;
                }
            }

            int64_t timeUs;
            if (extra != NULL
                    && extra->findInt64("timeUs", &timeUs))
                mTSParser->signalDiscontinuity(
                        (ATSParser::DiscontinuityType)type, extra);
        } else if (n < 0) {
            CHECK_EQ(n, -EWOULDBLOCK);
            break;
        } else {
            if (buffer[0] == 0x00) {
                // XXX legacy

                if (extra == NULL) {
                    extra = new AMessage;
                }

                uint8_t type = buffer[1];

                if (type & 2) {
                    int64_t mediaTimeUs;
                    memcpy(&mediaTimeUs, &buffer[2], sizeof(mediaTimeUs));

                    extra->setInt64(IStreamListener::kKeyMediaTimeUs, mediaTimeUs);
                }

                mTSParser->signalDiscontinuity(
                        ((type & 1) == 0)
                            ? ATSParser::DISCONTINUITY_SEEK
                            : ATSParser::DISCONTINUITY_FORMATCHANGE,
                        extra);
            } else {
                status_t err = mTSParser->feedTSPacket(buffer, sizeof(buffer));

                if (err != OK) {
                    ALOGE("TS Parser returned error %d", err);

                    mTSParser->signalEOS(err);
                    mFinalResult = err;
                    break;
                }
            }
        }
    }

    return OK;
}

sp<MetaData> NuPlayer::StreamingSource::getFormatMeta(bool audio) {
    ATSParser::SourceType type =
        audio ? ATSParser::AUDIO : ATSParser::VIDEO;

    sp<AnotherPacketSource> source =
        static_cast<AnotherPacketSource *>(mTSParser->getSource(type).get());

    if (source == NULL) {
        return NULL;
    }

    return source->getFormat();
}

/*
Where introduce the latency?

WFD Total: ~250 ms.
WFD source: 33 ms (capture one frame) + 33 ms (VPU has one frame delay) + 
7 ms (encoder time) + ~30 ms (net process time) = ~100 ms
WFD sink: ~150 ms for thread process and net jitter and avoid audio under run.

Quality criteria.

Discard one time media data during certain time, default is discard one time 
during 5 minutes.
As different use scenario have different quality and latency requirement, so my 
proposal is add one adjust button like volume adjust to adjust the threshold by 
user in WFD sink.

How to adjust latency threshold in WFD sink automatically?

If user haven?t set latency threshold. The threshold will adjust automatically.
Threshold will increase if discard media data within 5 minutes, or will 
decrease the threshold.

When WFD sink can discard media data?

Latency in WFD sink is jitter as net and thread process jitter. To avoid 
discard media data frequently, only can discard data out of jitter range. If 
the minimum value of WFD sink latency is bigger than threshold during the 
statistic interval, the media data can be discarded.

When WFD sink discard video data?

To avoid mosaic when discard video data, video data will be discarded only 
when WFD NuPlayer source buffered more than 1 second video and have discarded 
all audio data.
*/

bool NuPlayer::StreamingSource::discardMediaDate(bool audio, int64_t timeUs, sp<ABuffer> *accessUnit) {
    status_t error;
    status_t finalResult;
    int64_t nowUs, nowUsTemp;
    int64_t positionUs;
    int64_t nLatency;

    ATSParser::SourceType type =
        audio ? ATSParser::AUDIO : ATSParser::VIDEO;

    sp<AnotherPacketSource> source =
        static_cast<AnotherPacketSource *>(mTSParser->getSource(type).get());

    int64_t nBufferedTimeUs = source->getBufferedDurationUs(&error);

    nowUs = ALooper::GetNowUs();
    positionUs = (nowUs - mAnchorTimeRealUs) + mPositionUs;
 
    if (mDropEndTimeUs > nowUs) {
        ALOGI("Drop media data.\n");
        while (mTunnelRenderLatency > 0) {
            // Avoid dead loop.
            nowUsTemp = ALooper::GetNowUs();
            if (nowUsTemp > nowUs + 500000)
                break;

            feedMoreTSData();
        }

        type = ATSParser::AUDIO;

        source =
            static_cast<AnotherPacketSource *>(mTSParser->getSource(type).get());
        if (source == NULL)
            return false;

        while (1) {
            if (!source->hasBufferAvailable(&finalResult)) {
                break;
            }
            source->dequeueAccessUnit(accessUnit);
        }

        type = ATSParser::VIDEO;

        source =
            static_cast<AnotherPacketSource *>(mTSParser->getSource(type).get());
        if (source == NULL)
            return false;

        nBufferedTimeUs = source->getBufferedDurationUs(&error);
        if (nBufferedTimeUs > DROP_VIDEO_THRESHOLD) {
            ALOGI("Drop video.");
            while (1) {
                if (!source->hasBufferAvailable(&finalResult)) {
                    break;
                }
                source->dequeueAccessUnit(accessUnit);
            }
        }

        return true;
    }

    if (!audio)
        return false;

    nLatency = timeUs + nBufferedTimeUs - positionUs + mTunnelRenderLatency;
#if 0
    {
        static int32_t nCnt = 0;
        nCnt ++;
        if (nCnt % 100 == 0) {
        ALOGI("Total latency: %lld us TunnelRenderLatency: %lld mediaTimeUs: %lld nBufferedTimeUs: %lld positionUs: %lld", \
                nLatency, mTunnelRenderLatency, timeUs, nBufferedTimeUs, positionUs);
        }
    }
#endif

    bool bDrop = false;

    if (mStasticPeroid > nowUs) {
        if (mLatencyLowest > nLatency) {
            mLatencyLowest = nLatency;
        }
    } else {
        ALOGV("mLatencyLowest: %lld mLatencyThreshold: %lld", mLatencyLowest, mLatencyThreshold);
        if (mLatencyLowest > mLatencyThreshold) {
            bDrop = true;
        }
        mStasticPeroid = nowUs + STATISTIC_PERIOD;
        mLatencyLowest = LOWEST_LATENCY_INIT;
    }

    char value[PROPERTY_VALUE_MAX];
    int64_t nQualityCriteria = QUALITY_CATIRIA;
    // property for adjust quality criteria in ms, shouldn't less than 2 seconds.
    if ((property_get("sys.wifisink.quality", value, NULL))) {
        nQualityCriteria = atoi(value) * 1000;
    } 

    // property for adjust latency threshold in ms.
    if ((property_get("sys.wifisink.latency", value, NULL))) {
        mLatencyThreshold = atoi(value) * 1000;
    } else {
        // Adjust drop threshold.
        if (mPrevDropTimeUs > 0 && bDrop \
                && nowUs < mPrevDropTimeUs + nQualityCriteria) {
            mLatencyThreshold += 10000;
            if (mLatencyThreshold > LATENCY_THRESHOLD_MAX)
                mLatencyThreshold = LATENCY_THRESHOLD_MAX;
        } else if (mPrevDropTimeUs > 0 \
                && nowUs > mPrevDropTimeUs + nQualityCriteria) {
            mLatencyThreshold -= 10000;
            mPrevDropTimeUs = nowUs - nQualityCriteria + (nQualityCriteria >> 2);
            if (mLatencyThreshold < LATENCY_THRESHOLD_DEFAULT)
                mLatencyThreshold = LATENCY_THRESHOLD_DEFAULT;
        }
    }

    if (bDrop && nowUs > mResumeCheckTimeUs) {
        // Too long latency, discard the media data.
        ALOGI("Too long latency: %lld us TunnelRenderLatency: %lld mediaTimeUs: %lld nBufferedTimeUs: %lld positionUs: %lld", \
                nLatency, mTunnelRenderLatency, timeUs, nBufferedTimeUs, positionUs);
        mDropEndTimeUs = nowUs + mPipeLineLatencyUs - REMAIN_DATA_AFTER_DROP;
        mResumeCheckTimeUs = mDropEndTimeUs + RESUME_DROP_CHECK;
        mPrevDropTimeUs = nowUs;
        return true;
    } else {
        mPipeLineLatencyUs = timeUs - positionUs;
        if (mPipeLineLatencyUs <= 0 || mPipeLineLatencyUs > 500000)
            mPipeLineLatencyUs = 100000;
        ALOGV("mPipeLineLatencyUs: %lld\n", mPipeLineLatencyUs);
        return false;
    }
}

status_t NuPlayer::StreamingSource::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    status_t err;
    status_t finalResult;
    ATSParser::SourceType type =
        audio ? ATSParser::AUDIO : ATSParser::VIDEO;

    sp<AnotherPacketSource> source =
        static_cast<AnotherPacketSource *>(mTSParser->getSource(type).get());

    if (source == NULL) {
        return -EWOULDBLOCK;
    }

    while (1) {
        if (!source->hasBufferAvailable(&finalResult)) {
            return finalResult == OK ? -EWOULDBLOCK : finalResult;
        }

        err = source->dequeueAccessUnit(accessUnit);
        if (err == OK) {
            int64_t timeUs;
            CHECK((*accessUnit)->meta()->findInt64("timeUs", &timeUs));
            ALOGV("dequeueAccessUnit timeUs=%lld us", timeUs);

            if (!(mSource->flags() & IStreamSource::kFlagKeepLowLatency \
                        && mPositionUs > 0)) 
                break;
            if (discardMediaDate(audio, timeUs, accessUnit) == false)
                break;
        }
    }

    return err;
}

bool NuPlayer::StreamingSource::isAVCReorderDisabled() const {
    return mSource->flags() & IStreamSource::kFlagKeepLowLatency;
}

bool NuPlayer::StreamingSource::isRealTime() const {
    return mSource->flags() & IStreamSource::kFlagIsRealTimeData;
}

void NuPlayer::StreamingSource::setRenderPosition(int64_t positionUs) {
    mPositionUs = positionUs;
    mAnchorTimeRealUs = ALooper::GetNowUs();
}

}  // namespace android

