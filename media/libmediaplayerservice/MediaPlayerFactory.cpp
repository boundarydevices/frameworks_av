/*
**
** Copyright 2012, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/* Copyright (C) 2013 Freescale Semiconductor, Inc. */

#define LOG_TAG "MediaPlayerFactory"
#include <utils/Log.h>

#include <cutils/properties.h>
#include <media/IMediaPlayer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <utils/Errors.h>
#include <utils/misc.h>

#include "MediaPlayerFactory.h"

#include "MidiFile.h"
#include "TestPlayerStub.h"
#include "StagefrightPlayer.h"
#include "nuplayer/NuPlayerDriver.h"

#ifdef FSL_GM_PLAYER
#include <dlfcn.h>
#include "../libstagefright/include/AwesomePlayer.h"
#include "../libstagefright/include/NuCachedSource2.h"
#include "../libstagefright/include/WVMExtractor.h"
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/OMXPlayer.h>
#endif

namespace android {

Mutex MediaPlayerFactory::sLock;
MediaPlayerFactory::tFactoryMap MediaPlayerFactory::sFactoryMap;
bool MediaPlayerFactory::sInitComplete = false;

status_t MediaPlayerFactory::registerFactory_l(IFactory* factory,
                                               player_type type) {
    if (NULL == factory) {
        ALOGE("Failed to register MediaPlayerFactory of type %d, factory is"
              " NULL.", type);
        return BAD_VALUE;
    }

    if (sFactoryMap.indexOfKey(type) >= 0) {
        ALOGE("Failed to register MediaPlayerFactory of type %d, type is"
              " already registered.", type);
        return ALREADY_EXISTS;
    }

    if (sFactoryMap.add(type, factory) < 0) {
        ALOGE("Failed to register MediaPlayerFactory of type %d, failed to add"
              " to map.", type);
        return UNKNOWN_ERROR;
    }

    return OK;
}

player_type MediaPlayerFactory::getDefaultPlayerType() {
    char value[PROPERTY_VALUE_MAX];
    if (property_get("media.stagefright.use-nuplayer", value, NULL)
            && (!strcmp("1", value) || !strcasecmp("true", value))) {
        return NU_PLAYER;
    }

    return STAGEFRIGHT_PLAYER;
}

status_t MediaPlayerFactory::registerFactory(IFactory* factory,
                                             player_type type) {
    Mutex::Autolock lock_(&sLock);
    return registerFactory_l(factory, type);
}

void MediaPlayerFactory::unregisterFactory(player_type type) {
    Mutex::Autolock lock_(&sLock);
    sFactoryMap.removeItem(type);
}

#define GET_PLAYER_TYPE_IMPL(a...)                      \
    Mutex::Autolock lock_(&sLock);                      \
                                                        \
    player_type ret = STAGEFRIGHT_PLAYER;               \
    float bestScore = 0.0;                              \
                                                        \
    for (size_t i = 0; i < sFactoryMap.size(); ++i) {   \
                                                        \
        IFactory* v = sFactoryMap.valueAt(i);           \
        float thisScore;                                \
        CHECK(v != NULL);                               \
        thisScore = v->scoreFactory(a);      \
        if (thisScore > bestScore) {                    \
            ret = sFactoryMap.keyAt(i);                 \
            bestScore = thisScore;                      \
        }                                               \
    }                                                   \
                                                        \
    if (0.0 == bestScore) {                             \
        ret = getDefaultPlayerType();                   \
    }                                                   \
                                                        \
    return ret;

player_type MediaPlayerFactory::getPlayerType(const sp<IMediaPlayer>& client,
                                              const char* url,
                                              const KeyedVector<String8, String8> *headers) {
    GET_PLAYER_TYPE_IMPL(client, url, bestScore, headers);
}

player_type MediaPlayerFactory::getPlayerType(const sp<IMediaPlayer>& client,
                                              int fd,
                                              int64_t offset,
                                              int64_t length) {
    GET_PLAYER_TYPE_IMPL(client, fd, offset, length, bestScore);
}

player_type MediaPlayerFactory::getPlayerType(const sp<IMediaPlayer>& client,
                                              const sp<IStreamSource> &source) {
    GET_PLAYER_TYPE_IMPL(client, source, bestScore);
}

#undef GET_PLAYER_TYPE_IMPL

sp<MediaPlayerBase> MediaPlayerFactory::createPlayer(
        player_type playerType,
        void* cookie,
        notify_callback_f notifyFunc) {
    sp<MediaPlayerBase> p;
    IFactory* factory;
    status_t init_result;
    Mutex::Autolock lock_(&sLock);

    if (sFactoryMap.indexOfKey(playerType) < 0) {
        ALOGE("Failed to create player object of type %d, no registered"
              " factory", playerType);
        return p;
    }

    factory = sFactoryMap.valueFor(playerType);
    CHECK(NULL != factory);
    p = factory->createPlayer();

    if (p == NULL) {
        ALOGE("Failed to create player object of type %d, create failed",
               playerType);
        return p;
    }

    init_result = p->initCheck();
    if (init_result == NO_ERROR) {
        p->setNotifyCallback(cookie, notifyFunc);
    } else {
        ALOGE("Failed to create player object of type %d, initCheck failed"
              " (res = %d)", playerType, init_result);
        p.clear();
    }

    return p;
}

/*****************************************************************************
 *                                                                           *
 *                     Built-In Factory Implementations                      *
 *                                                                           *
 *****************************************************************************/

#ifdef FSL_GM_PLAYER
enum {
    INCOGNITO           = 0x8000,
};

bool isWVM(const char* url,
           const KeyedVector<String8, String8> *headers) {
    sp<DataSource> dataSource;
    String8 mUri;
    KeyedVector<String8, String8> mUriHeaders;
    sp<HTTPBase> mConnectingDataSource;
    sp<NuCachedSource2> mCachedSource;
    uint32_t mFlags;

    mUri = url;

    void *VendorLibHandle = NULL;
    if (VendorLibHandle == NULL) {
        VendorLibHandle = dlopen("libwvm.so", RTLD_NOW);
    }

    if (!VendorLibHandle) {
        return false;
    }

    if (headers) {
        mUriHeaders = *headers;

        ssize_t index = mUriHeaders.indexOfKey(String8("x-hide-urls-from-log"));
        if (index >= 0) {
            // Browser is in "incognito" mode, suppress logging URLs.

            // This isn't something that should be passed to the server.
            mUriHeaders.removeItemsAt(index);

            mFlags |= INCOGNITO;
        }
    }

    if (!strncasecmp("http://", mUri.string(), 7)
            || !strncasecmp("https://", mUri.string(), 8)) {
        mConnectingDataSource = HTTPBase::Create(
                (mFlags & INCOGNITO)
                    ? HTTPBase::kFlagIncognito
                    : 0);

        String8 cacheConfig;
        bool disconnectAtHighwatermark;
        NuCachedSource2::RemoveCacheSpecificHeaders(
                &mUriHeaders, &cacheConfig, &disconnectAtHighwatermark);

        status_t err = mConnectingDataSource->connect(mUri, &mUriHeaders);

        if (err != OK) {
            mConnectingDataSource.clear();

            ALOGI("mConnectingDataSource->connect() returned %d", err);
            return false;
        }

        // The widevine extractor does its own caching.
        mCachedSource = new NuCachedSource2(
                mConnectingDataSource,
                cacheConfig.isEmpty() ? NULL : cacheConfig.string(),
                disconnectAtHighwatermark);

        dataSource = mCachedSource;

        mConnectingDataSource.clear();

        // Removed prefill as we can't abort it.
        //
        // We're going to prefill the cache before trying to instantiate
        // the extractor below, as the latter is an operation that otherwise
        // could block on the datasource for a significant amount of time.
        // During that time we'd be unable to abort the preparation phase
        // without this prefill.

    } else {
        dataSource = DataSource::CreateFromURI(mUri.string(), &mUriHeaders);
    }

    if (dataSource == NULL) {
        return false;
    }

    typedef WVMLoadableExtractor *(*SnifferFunc)(const sp<DataSource>&);
    SnifferFunc snifferFunc =
        (SnifferFunc) dlsym(VendorLibHandle,
                "_ZN7android15IsWidevineMediaERKNS_2spINS_10DataSourceEEE");

    if (snifferFunc) {
        if ((*snifferFunc)(dataSource)) {
            return true;
        }
    }

    return false;
}

bool isWVM(int fd,
           int64_t offset,
           int64_t length) {
    sp<DataSource> dataSource;

    void *VendorLibHandle = NULL;
    if (VendorLibHandle == NULL) {
        VendorLibHandle = dlopen("libwvm.so", RTLD_NOW);
    }

    if (!VendorLibHandle) {
        return false;
    }

    dataSource = new FileSource(dup(fd), offset, length);

    if (dataSource == NULL) {
        return false;
    }

    typedef WVMLoadableExtractor *(*SnifferFunc)(const sp<DataSource>&);
    SnifferFunc snifferFunc =
        (SnifferFunc) dlsym(VendorLibHandle,
                "_ZN7android15IsWidevineMediaERKNS_2spINS_10DataSourceEEE");

    if (snifferFunc) {
        if ((*snifferFunc)(dataSource)) {
            return true;
        }
    }

    return false;
}

class OMXPlayerFactory : public MediaPlayerFactory::IFactory {
    public:
        virtual float scoreFactory(const sp<IMediaPlayer>& client,
                const char* url,
                float curScore,
                const KeyedVector<String8, String8> *headers) {
            static const float kOurScore = 1.0;
            static const char* const FILE_EXTS[] = {
                ".avi",
                ".mkv",
                ".mp4",
                ".m4a",
                ".3gp",
                ".3g2",
                ".3gpp",
                ".mov",
                ".rmvb",
                ".rm", 
                ".wmv",
                ".asf",
                ".flv",
                ".mpg",
                ".vob",
                ".ts", 
                ".f4v",
                ".mp3",
                ".aac",
                ".wma",
                ".ra", 
                ".wav",
                ".flac",
                ".divx",
                ".m4v",        
                //".ogg,   
#ifdef MX6X
                ".webm",   
#endif
                ".amr",
                ".awb"
            };

            char value[PROPERTY_VALUE_MAX];
            if (!(property_get("media.omxgm.enable-player", value, NULL)
                    && (!strcmp("1", value) || !strcasecmp("true", value)))) {
                return 0.0;
            }

            if (kOurScore <= curScore)
                return 0.0;

            if (!strncasecmp("widevine://", url, 11)) {
                return 0.0;
            }

            if (!strncasecmp(url, "http://", 7)) { 
                if (isWVM(url, headers))
                return 0.0;
            }

            if (!strncasecmp(url, "http://", 7) \
                    || !strncasecmp(url, "rtsp://", 7) \
                    || !strncasecmp(url, "udp://", 6) \
                    || !strncasecmp(url, "rtp://", 6))
                return kOurScore;

            int lenURL = strlen(url);
            for (int i = 0; i < NELEM(FILE_EXTS); ++i) {
                int len = strlen(FILE_EXTS[i]);
                int start = lenURL - len;
                if (start > 0) {
                    if (!strncasecmp(url + start, FILE_EXTS[i], len)) {
                        return kOurScore;
                    }
                }
            }

            return 0.0;
        }

        virtual float scoreFactory(const sp<IMediaPlayer>& client,
                int fd,
                int64_t offset,
                int64_t length,
                float curScore) {
            static const float kOurScore = 1.0;

            char value[PROPERTY_VALUE_MAX];
            if (!(property_get("media.omxgm.enable-player", value, NULL)
                    && (!strcmp("1", value) || !strcasecmp("true", value)))) {
                return 0.0;
            }

            if (kOurScore <= curScore)
                return 0.0;

            if (isWVM(fd, offset, length))
                return 0.0;

            char url[128];
            int ret = 0;

            OMXPlayerType *pType = new OMXPlayerType();
            sprintf(url, "sharedfd://%d:%lld:%lld",  fd, offset, length);
            ret = pType->IsSupportedContent(url);
            delete pType;
            if(ret) {
                return kOurScore;
            }

            return 0.0;
        }

        virtual sp<MediaPlayerBase> createPlayer() {
            ALOGV(" create OMXPlayer");
            return new OMXPlayer();
        }
};
#endif

class StagefrightPlayerFactory :
    public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               int fd,
                               int64_t offset,
                               int64_t length,
                               float curScore) {
        char buf[20];
        lseek(fd, offset, SEEK_SET);
        read(fd, buf, sizeof(buf));
        lseek(fd, offset, SEEK_SET);

        long ident = *((long*)buf);

        // Ogg vorbis?
        if (ident == 0x5367674f) // 'OggS'
            return 1.0;

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create StagefrightPlayer");
        return new StagefrightPlayer();
    }
};

class NuPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const char* url,
                               float curScore,
                               const KeyedVector<String8, String8> *headers) {
        static const float kOurScore = 0.8;

        if (kOurScore <= curScore)
            return 0.0;

        if (!strncasecmp("http://", url, 7)
                || !strncasecmp("https://", url, 8)
                || !strncasecmp("file://", url, 7)) {
            size_t len = strlen(url);
            if (len >= 5 && !strcasecmp(".m3u8", &url[len - 5])) {
                return kOurScore;
            }

            if (strstr(url,"m3u8")) {
                return kOurScore;
            }

            if ((len >= 4 && !strcasecmp(".sdp", &url[len - 4])) || strstr(url, ".sdp?")) {
                return kOurScore;
            }
        }

        if (!strncasecmp("rtsp://", url, 7)) {
            return kOurScore;
        }

        return 0.0;
    }

    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const sp<IStreamSource> &source,
                               float curScore) {
        return 1.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create NuPlayer");
        return new NuPlayerDriver;
    }
};

class SonivoxPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const char* url,
                               float curScore,
                               const KeyedVector<String8, String8> *headers) {
        static const float kOurScore = 0.4;
        static const char* const FILE_EXTS[] = { ".mid",
                                                 ".midi",
                                                 ".smf",
                                                 ".xmf",
                                                 ".mxmf",
                                                 ".imy",
                                                 ".rtttl",
                                                 ".rtx",
                                                 ".ota" };
        if (kOurScore <= curScore)
            return 0.0;

        // use MidiFile for MIDI extensions
        int lenURL = strlen(url);
        for (int i = 0; i < NELEM(FILE_EXTS); ++i) {
            int len = strlen(FILE_EXTS[i]);
            int start = lenURL - len;
            if (start > 0) {
                if (!strncasecmp(url + start, FILE_EXTS[i], len)) {
                    return kOurScore;
                }
            }
        }

        return 0.0;
    }

    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               int fd,
                               int64_t offset,
                               int64_t length,
                               float curScore) {
        static const float kOurScore = 0.8;

        if (kOurScore <= curScore)
            return 0.0;

        // Some kind of MIDI?
        EAS_DATA_HANDLE easdata;
        if (EAS_Init(&easdata) == EAS_SUCCESS) {
            EAS_FILE locator;
            locator.path = NULL;
            locator.fd = fd;
            locator.offset = offset;
            locator.length = length;
            EAS_HANDLE  eashandle;
            if (EAS_OpenFile(easdata, &locator, &eashandle) == EAS_SUCCESS) {
                EAS_CloseFile(easdata, eashandle);
                EAS_Shutdown(easdata);
                return kOurScore;
            }
            EAS_Shutdown(easdata);
        }

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV(" create MidiFile");
        return new MidiFile();
    }
};

class TestPlayerFactory : public MediaPlayerFactory::IFactory {
  public:
    virtual float scoreFactory(const sp<IMediaPlayer>& client,
                               const char* url,
                               float curScore,
                               const KeyedVector<String8, String8> *headers) {
        if (TestPlayerStub::canBeUsed(url)) {
            return 1.0;
        }

        return 0.0;
    }

    virtual sp<MediaPlayerBase> createPlayer() {
        ALOGV("Create Test Player stub");
        return new TestPlayerStub();
    }
};

void MediaPlayerFactory::registerBuiltinFactories() {
    Mutex::Autolock lock_(&sLock);

    if (sInitComplete)
        return;

#ifdef FSL_GM_PLAYER
    registerFactory_l(new OMXPlayerFactory(), OMX_PLAYER);
#endif
    registerFactory_l(new StagefrightPlayerFactory(), STAGEFRIGHT_PLAYER);
    registerFactory_l(new NuPlayerFactory(), NU_PLAYER);
    registerFactory_l(new SonivoxPlayerFactory(), SONIVOX_PLAYER);
    registerFactory_l(new TestPlayerFactory(), TEST_PLAYER);

    sInitComplete = true;
}

}  // namespace android
