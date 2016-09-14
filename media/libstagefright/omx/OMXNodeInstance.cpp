/*
 * Copyright (C) 2009 The Android Open Source Project
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
#define LOG_TAG "OMXNodeInstance"
#include <utils/Log.h>

#include "../include/OMXNodeInstance.h"
#include "OMXMaster.h"
#include "GraphicBufferSource.h"

#include <OMX_Component.h>
#include <OMX_IndexExt.h>

#include <binder/IMemory.h>
#include <gui/BufferQueue.h>
#include <utils/misc.h>
#include <HardwareAPI.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaErrors.h>

static const OMX_U32 kPortIndexInput = 0;
static const OMX_U32 kPortIndexOutput = 1;

namespace android {

struct BufferMeta {
    BufferMeta(
            const sp<IMemory> &mem, OMX_U32 portIndex, bool copyToOmx,
            bool copyFromOmx, OMX_U8 *backup)
        : mMem(mem),
          mCopyFromOmx(copyFromOmx),
          mCopyToOmx(copyToOmx),
          mPortIndex(portIndex),
          mBackup(backup) {
    }

    BufferMeta(size_t size, OMX_U32 portIndex)
        : mSize(size),
          mCopyFromOmx(false),
          mCopyToOmx(false),
          mPortIndex(portIndex),
          mBackup(NULL) {
    }

    BufferMeta(const sp<GraphicBuffer> &graphicBuffer, OMX_U32 portIndex)
        : mGraphicBuffer(graphicBuffer),
          mCopyFromOmx(false),
          mCopyToOmx(false),
          mPortIndex(portIndex),
          mBackup(NULL) {
    }

    void CopyFromOMX(const OMX_BUFFERHEADERTYPE *header) {
        if (!mCopyFromOmx) {
            return;
        }
#ifdef TF101_OMX
        size_t bytesToCopy = header->nFlags & OMX_BUFFERFLAG_EXTRADATA ?
            header->nAllocLen - header->nOffset : header->nFilledLen;
#endif
        memcpy((OMX_U8 *)mMem->pointer() + header->nOffset,
#ifdef TF101_OMX
               header->pBuffer + header->nOffset, bytesToCopy);
#else
               header->pBuffer + header->nOffset,
               header->nFilledLen);
#endif
    }

    void CopyToOMX(const OMX_BUFFERHEADERTYPE *header) {
        if (!mCopyToOmx) {
            return;
        }

#ifdef TF101_OMX
        size_t bytesToCopy = header->nFlags & OMX_BUFFERFLAG_EXTRADATA ?
            header->nAllocLen - header->nOffset : header->nFilledLen;
#endif
        memcpy(header->pBuffer + header->nOffset,
#ifdef TF101_OMX
               (const OMX_U8 *)mMem->pointer() + header->nOffset, bytesToCopy);
#else
               (const OMX_U8 *)mMem->pointer() + header->nOffset,
               header->nFilledLen);
#endif
    }

    void setGraphicBuffer(const sp<GraphicBuffer> &graphicBuffer) {
        mGraphicBuffer = graphicBuffer;
    }

    OMX_U32 getPortIndex() {
        return mPortIndex;
    }

    ~BufferMeta() {
        delete[] mBackup;
    }

private:
    sp<GraphicBuffer> mGraphicBuffer;
    sp<IMemory> mMem;
    size_t mSize;
    bool mCopyFromOmx;
    bool mCopyToOmx;
    OMX_U32 mPortIndex;
    OMX_U8 *mBackup;

    BufferMeta(const BufferMeta &);
    BufferMeta &operator=(const BufferMeta &);
};

// static
OMX_CALLBACKTYPE OMXNodeInstance::kCallbacks = {
    &OnEvent, &OnEmptyBufferDone, &OnFillBufferDone
};

OMXNodeInstance::OMXNodeInstance(
        OMX *owner, const sp<IOMXObserver> &observer, const char *name)
    : mOwner(owner),
      mNodeID(NULL),
      mHandle(NULL),
      mObserver(observer),
      mDying(false),
      mSailed(false),
      mQueriedProhibitedExtensions(false) {
    mUsingMetadata[0] = false;
    mUsingMetadata[1] = false;
    mIsSecure = AString(name).endsWith(".secure");
}

OMXNodeInstance::~OMXNodeInstance() {
    CHECK(mHandle == NULL);
}

void OMXNodeInstance::setHandle(OMX::node_id node_id, OMX_HANDLETYPE handle) {
    CHECK(mHandle == NULL);
    mNodeID = node_id;
    mHandle = handle;
}

sp<GraphicBufferSource> OMXNodeInstance::getGraphicBufferSource() {
    Mutex::Autolock autoLock(mGraphicBufferSourceLock);
    return mGraphicBufferSource;
}

void OMXNodeInstance::setGraphicBufferSource(
        const sp<GraphicBufferSource>& bufferSource) {
    Mutex::Autolock autoLock(mGraphicBufferSourceLock);
    mGraphicBufferSource = bufferSource;
}

OMX *OMXNodeInstance::owner() {
    return mOwner;
}

sp<IOMXObserver> OMXNodeInstance::observer() {
    return mObserver;
}

OMX::node_id OMXNodeInstance::nodeID() {
    return mNodeID;
}

static status_t StatusFromOMXError(OMX_ERRORTYPE err) {
    switch (err) {
        case OMX_ErrorNone:
            return OK;
        case OMX_ErrorUnsupportedSetting:
            return ERROR_UNSUPPORTED;
        default:
            return UNKNOWN_ERROR;
    }
}

status_t OMXNodeInstance::freeNode(OMXMaster *master) {
    static int32_t kMaxNumIterations = 10;

    // Transition the node from its current state all the way down
    // to "Loaded".
    // This ensures that all active buffers are properly freed even
    // for components that don't do this themselves on a call to
    // "FreeHandle".

    // The code below may trigger some more events to be dispatched
    // by the OMX component - we want to ignore them as our client
    // does not expect them.
    mDying = true;

    OMX_STATETYPE state;
    CHECK_EQ(OMX_GetState(mHandle, &state), OMX_ErrorNone);
    switch (state) {
        case OMX_StateExecuting:
        {
            ALOGV("forcing Executing->Idle");
            sendCommand(OMX_CommandStateSet, OMX_StateIdle);
            OMX_ERRORTYPE err;
            int32_t iteration = 0;
            while ((err = OMX_GetState(mHandle, &state)) == OMX_ErrorNone
                   && state != OMX_StateIdle
                   && state != OMX_StateInvalid) {
                if (++iteration > kMaxNumIterations) {
                    ALOGE("component failed to enter Idle state, aborting.");
                    state = OMX_StateInvalid;
                    break;
                }

                usleep(100000);
            }
            CHECK_EQ(err, OMX_ErrorNone);

            if (state == OMX_StateInvalid) {
                break;
            }

            // fall through
        }

        case OMX_StateIdle:
        {
            ALOGV("forcing Idle->Loaded");
            sendCommand(OMX_CommandStateSet, OMX_StateLoaded);

            freeActiveBuffers();

            OMX_ERRORTYPE err;
            int32_t iteration = 0;
            while ((err = OMX_GetState(mHandle, &state)) == OMX_ErrorNone
                   && state != OMX_StateLoaded
                   && state != OMX_StateInvalid) {
                if (++iteration > kMaxNumIterations) {
                    ALOGE("component failed to enter Loaded state, aborting.");
                    state = OMX_StateInvalid;
                    break;
                }

                ALOGV("waiting for Loaded state...");
                usleep(100000);
            }
            CHECK_EQ(err, OMX_ErrorNone);

            // fall through
        }

        case OMX_StateLoaded:
        case OMX_StateInvalid:
            break;

        default:
            CHECK(!"should not be here, unknown state.");
            break;
    }

    ALOGV("calling destroyComponentInstance");
    OMX_ERRORTYPE err = master->destroyComponentInstance(
            static_cast<OMX_COMPONENTTYPE *>(mHandle));
    ALOGV("destroyComponentInstance returned err %d", err);

    mHandle = NULL;

    if (err != OMX_ErrorNone) {
        ALOGE("FreeHandle FAILED with error 0x%08x.", err);
    }

    mOwner->invalidateNodeID(mNodeID);
    mNodeID = NULL;

    ALOGV("OMXNodeInstance going away.");
    delete this;

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::sendCommand(
        OMX_COMMANDTYPE cmd, OMX_S32 param) {
    if (cmd == OMX_CommandStateSet && param != OMX_StateIdle) {
        // Normally there are no configurations past first StateSet; however, OMXCodec supports
        // meta configuration past Stateset:Idle.
        mSailed = true;
    }
    const sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && cmd == OMX_CommandStateSet) {
        if (param == OMX_StateIdle) {
            // Initiating transition from Executing -> Idle
            // ACodec is waiting for all buffers to be returned, do NOT
            // submit any more buffers to the codec.
            bufferSource->omxIdle();
        } else if (param == OMX_StateLoaded) {
            // Initiating transition from Idle/Executing -> Loaded
            // Buffers are about to be freed.
            bufferSource->omxLoaded();
            setGraphicBufferSource(NULL);
        }

        // fall through
    }

    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_SendCommand(mHandle, cmd, param, NULL);
    return StatusFromOMXError(err);
}

bool OMXNodeInstance::isProhibitedIndex_l(OMX_INDEXTYPE index) {
    // these extensions can only be used from OMXNodeInstance, not by clients directly.
    static const char *restricted_extensions[] = {
        "OMX.google.android.index.storeMetaDataInBuffers",
        "OMX.google.android.index.prepareForAdaptivePlayback",
        "OMX.google.android.index.useAndroidNativeBuffer2",
        "OMX.google.android.index.useAndroidNativeBuffer",
        "OMX.google.android.index.enableAndroidNativeBuffers",
        "OMX.google.android.index.getAndroidNativeBufferUsage",
    };

    if ((index > OMX_IndexComponentStartUnused && index <= OMX_IndexParamStandardComponentRole)
            || (index > OMX_IndexPortStartUnused && index <= OMX_IndexParamCompBufferSupplier)
            || (index > OMX_IndexAudioStartUnused && index <= OMX_IndexConfigAudioChannelVolume)
            || (index > OMX_IndexVideoStartUnused && index <= OMX_IndexConfigVideoNalSize)
            || (index > OMX_IndexCommonStartUnused
                    && index <= OMX_IndexConfigCommonTransitionEffect)
            || (index > (OMX_INDEXTYPE)OMX_IndexExtVideoStartUnused
                    && index <= (OMX_INDEXTYPE)OMX_IndexConfigVideoVp8ReferenceFrameType)) {
        return false;
    }

    if (!mQueriedProhibitedExtensions) {
        for (size_t i = 0; i < NELEM(restricted_extensions); ++i) {
            OMX_INDEXTYPE ext;
            if (OMX_GetExtensionIndex(mHandle, (OMX_STRING)restricted_extensions[i], &ext) == OMX_ErrorNone) {
                mProhibitedExtensions.add(ext);
            }
        }
        mQueriedProhibitedExtensions = true;
    }

    return mProhibitedExtensions.indexOf(index) >= 0;
}

status_t OMXNodeInstance::getParameter(
        OMX_INDEXTYPE index, void *params, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

    OMX_ERRORTYPE err = OMX_GetParameter(mHandle, index, params);

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::setParameter(
        OMX_INDEXTYPE index, const void *params, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

    OMX_ERRORTYPE err = OMX_SetParameter(
            mHandle, index, const_cast<void *>(params));

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getConfig(
        OMX_INDEXTYPE index, void *params, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

    OMX_ERRORTYPE err = OMX_GetConfig(mHandle, index, params);
    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::setConfig(
        OMX_INDEXTYPE index, const void *params, size_t size) {
    Mutex::Autolock autoLock(mLock);

    if (isProhibitedIndex_l(index)) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_INDEX;
    }

    OMX_ERRORTYPE err = OMX_SetConfig(
            mHandle, index, const_cast<void *>(params));

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getState(OMX_STATETYPE* state) {
    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_GetState(mHandle, state);

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::enableGraphicBuffers(
        OMX_U32 portIndex, OMX_BOOL enable) {
    Mutex::Autolock autoLock(mLock);
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.enableAndroidNativeBuffers");

    OMX_INDEXTYPE index;
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);

    if (err != OMX_ErrorNone) {
        if (enable) {
            ALOGE("OMX_GetExtensionIndex %s failed", name);
        }

        return StatusFromOMXError(err);
    }

    OMX_VERSIONTYPE ver;
    ver.s.nVersionMajor = 1;
    ver.s.nVersionMinor = 0;
    ver.s.nRevision = 0;
    ver.s.nStep = 0;
    EnableAndroidNativeBuffersParams params = {
        sizeof(EnableAndroidNativeBuffersParams), ver, portIndex, enable,
    };

    err = OMX_SetParameter(mHandle, index, &params);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_EnableAndroidNativeBuffers failed with error %d (0x%08x)",
                err, err);

        return UNKNOWN_ERROR;
    }

    return OK;
}

status_t OMXNodeInstance::getGraphicBufferUsage(
        OMX_U32 portIndex, OMX_U32* usage) {
    Mutex::Autolock autoLock(mLock);

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.getAndroidNativeBufferUsage");
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_GetExtensionIndex %s failed", name);

        return StatusFromOMXError(err);
    }

    OMX_VERSIONTYPE ver;
    ver.s.nVersionMajor = 1;
    ver.s.nVersionMinor = 0;
    ver.s.nRevision = 0;
    ver.s.nStep = 0;
    GetAndroidNativeBufferUsageParams params = {
        sizeof(GetAndroidNativeBufferUsageParams), ver, portIndex, 0,
    };

    err = OMX_GetParameter(mHandle, index, &params);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_GetAndroidNativeBufferUsage failed with error %d (0x%08x)",
                err, err);
        return UNKNOWN_ERROR;
    }

    *usage = params.nUsage;

    return OK;
}

status_t OMXNodeInstance::storeMetaDataInBuffers(
        OMX_U32 portIndex,
        OMX_BOOL enable) {
    Mutex::Autolock autolock(mLock);
    return storeMetaDataInBuffers_l(portIndex, enable);
}

status_t OMXNodeInstance::storeMetaDataInBuffers_l(
        OMX_U32 portIndex,
        OMX_BOOL enable) {
    if (mSailed) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }
    if (portIndex != kPortIndexInput && portIndex != kPortIndexOutput) {
        android_errorWriteLog(0x534e4554, "26324358");
        return BAD_VALUE;
    }

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.storeMetaDataInBuffers");

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);
    if (err != OMX_ErrorNone) {
        ALOGE("OMX_GetExtensionIndex %s failed", name);

        return StatusFromOMXError(err);
    }

    StoreMetaDataInBuffersParams params;
    memset(&params, 0, sizeof(params));
    params.nSize = sizeof(params);

    // Version: 1.0.0.0
    params.nVersion.s.nVersionMajor = 1;

    params.nPortIndex = portIndex;
    params.bStoreMetaData = enable;
    if ((err = OMX_SetParameter(mHandle, index, &params)) != OMX_ErrorNone) {
        ALOGE("OMX_SetParameter() failed for StoreMetaDataInBuffers: 0x%08x", err);
        if (enable) {
            mUsingMetadata[portIndex] = false;
        }
        return UNKNOWN_ERROR;
    } else {
        mUsingMetadata[portIndex] = enable;
    }
    return err;
}

status_t OMXNodeInstance::prepareForAdaptivePlayback(
        OMX_U32 portIndex, OMX_BOOL enable, OMX_U32 maxFrameWidth,
        OMX_U32 maxFrameHeight) {
    Mutex::Autolock autolock(mLock);
    if (mSailed) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }

    OMX_INDEXTYPE index;
    OMX_STRING name = const_cast<OMX_STRING>(
            "OMX.google.android.index.prepareForAdaptivePlayback");

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);
    if (err != OMX_ErrorNone) {
        ALOGW_IF(enable, "OMX_GetExtensionIndex %s failed", name);
        return StatusFromOMXError(err);
    }

    PrepareForAdaptivePlaybackParams params;
    params.nSize = sizeof(params);
    params.nVersion.s.nVersionMajor = 1;
    params.nVersion.s.nVersionMinor = 0;
    params.nVersion.s.nRevision = 0;
    params.nVersion.s.nStep = 0;

    params.nPortIndex = portIndex;
    params.bEnable = enable;
    params.nMaxFrameWidth = maxFrameWidth;
    params.nMaxFrameHeight = maxFrameHeight;
    if ((err = OMX_SetParameter(mHandle, index, &params)) != OMX_ErrorNone) {
        ALOGW("OMX_SetParameter failed for PrepareForAdaptivePlayback "
              "with error %d (0x%08x)", err, err);
        return UNKNOWN_ERROR;
    }
    return err;
}

status_t OMXNodeInstance::useBuffer(
        OMX_U32 portIndex, const sp<IMemory> &params,
        OMX::buffer_id *buffer, OMX_BOOL crossProcess) {
    Mutex::Autolock autoLock(mLock);
    if (portIndex >= NELEM(mUsingMetadata)) {
        return BAD_VALUE;
    }
    // We do not support metadata mode changes past buffer allocation
    mSailed = true;

    // metadata buffers are not connected cross process
    BufferMeta *buffer_meta;
    bool isMeta = mUsingMetadata[portIndex];
    bool useBackup = crossProcess && isMeta; // use a backup buffer instead of the actual buffer
    OMX_U8 *data = static_cast<OMX_U8 *>(params->pointer());
    // allocate backup buffer
    if (useBackup) {
        data = new (std::nothrow) OMX_U8[params->size()];
        if (data == NULL) {
            return NO_MEMORY;
        }
        memset(data, 0, params->size());

        buffer_meta = new BufferMeta(
                params, portIndex, false /* copyToOmx */, false /* copyFromOmx */, data);
    } else {
        buffer_meta = new BufferMeta(
                params, portIndex, false /* copyToOmx */, false /* copyFromOmx */, NULL);
    }

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_UseBuffer(
            mHandle, &header, portIndex, buffer_meta,
            params->size(), data);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_UseBuffer failed with error %d (0x%08x)", err, err);

        delete buffer_meta;
        buffer_meta = NULL;

        *buffer = 0;

        return UNKNOWN_ERROR;
    }

    CHECK_EQ(header->pAppPrivate, buffer_meta);

    *buffer = header;

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }

    return OK;
}

#ifdef SEMC_ICS_CAMERA_BLOB
status_t OMXNodeInstance::useBufferPmem(
        OMX_U32 portIndex, OMX_QCOM_PLATFORM_PRIVATE_PMEM_INFO *pmem_info, OMX_U32 size, void *vaddr,
        OMX::buffer_id *buffer) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_UseBuffer(
            mHandle, &header, portIndex, pmem_info,
            size, static_cast<OMX_U8 *>(vaddr));

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_UseBuffer failed with error %d (0x%08x)", err, err);

        *buffer = 0;

        return UNKNOWN_ERROR;
    }

    header->pAppPrivate = NULL;

    *buffer = header;

    addActiveBuffer(portIndex, *buffer);

    return OK;
}
#endif

status_t OMXNodeInstance::useGraphicBuffer2_l(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id *buffer) {

    // port definition
    OMX_PARAM_PORTDEFINITIONTYPE def;
    def.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
    def.nVersion.s.nVersionMajor = 1;
    def.nVersion.s.nVersionMinor = 0;
    def.nVersion.s.nRevision = 0;
    def.nVersion.s.nStep = 0;
    def.nPortIndex = portIndex;
    OMX_ERRORTYPE err = OMX_GetParameter(mHandle, OMX_IndexParamPortDefinition, &def);
    if (err != OMX_ErrorNone)
    {
        ALOGE("%s::%d:Error getting OMX_IndexParamPortDefinition", __FUNCTION__, __LINE__);
        return err;
    }

    BufferMeta *bufferMeta = new BufferMeta(graphicBuffer, portIndex);

    OMX_BUFFERHEADERTYPE *header = NULL;
    OMX_U8* bufferHandle = const_cast<OMX_U8*>(
            reinterpret_cast<const OMX_U8*>(graphicBuffer->handle));

    err = OMX_UseBuffer(
            mHandle,
            &header,
            portIndex,
            bufferMeta,
            def.nBufferSize,
            bufferHandle);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_UseBuffer failed with error %d (0x%08x)", err, err);
        delete bufferMeta;
        bufferMeta = NULL;
        *buffer = 0;
        return UNKNOWN_ERROR;
    }

    CHECK_EQ(header->pBuffer, bufferHandle);
    CHECK_EQ(header->pAppPrivate, bufferMeta);

    *buffer = header;

    addActiveBuffer(portIndex, *buffer);

    return OK;
}

// XXX: This function is here for backwards compatibility.  Once the OMX
// implementations have been updated this can be removed and useGraphicBuffer2
// can be renamed to useGraphicBuffer.
status_t OMXNodeInstance::useGraphicBuffer(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id *buffer) {
    Mutex::Autolock autoLock(mLock);

    // See if the newer version of the extension is present.
    OMX_INDEXTYPE index;
    if (OMX_GetExtensionIndex(
            mHandle,
            const_cast<OMX_STRING>("OMX.google.android.index.useAndroidNativeBuffer2"),
            &index) == OMX_ErrorNone) {
        return useGraphicBuffer2_l(portIndex, graphicBuffer, buffer);
    }

    OMX_STRING name = const_cast<OMX_STRING>(
        "OMX.google.android.index.useAndroidNativeBuffer");
    OMX_ERRORTYPE err = OMX_GetExtensionIndex(mHandle, name, &index);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_GetExtensionIndex %s failed", name);

        return StatusFromOMXError(err);
    }

    BufferMeta *bufferMeta = new BufferMeta(graphicBuffer, portIndex);

    OMX_BUFFERHEADERTYPE *header;

    OMX_VERSIONTYPE ver;
    ver.s.nVersionMajor = 1;
    ver.s.nVersionMinor = 0;
    ver.s.nRevision = 0;
    ver.s.nStep = 0;
    UseAndroidNativeBufferParams params = {
        sizeof(UseAndroidNativeBufferParams), ver, portIndex, bufferMeta,
        &header, graphicBuffer,
    };

    err = OMX_SetParameter(mHandle, index, &params);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_UseAndroidNativeBuffer failed with error %d (0x%08x)", err,
                err);

        delete bufferMeta;
        bufferMeta = NULL;

        *buffer = 0;

        return UNKNOWN_ERROR;
    }

    CHECK_EQ(header->pAppPrivate, bufferMeta);

    *buffer = header;

    addActiveBuffer(portIndex, *buffer);

    return OK;
}

status_t OMXNodeInstance::updateGraphicBufferInMeta(
        OMX_U32 portIndex, const sp<GraphicBuffer>& graphicBuffer,
        OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = (OMX_BUFFERHEADERTYPE *)(buffer);
    VideoDecoderOutputMetaData *metadata =
        (VideoDecoderOutputMetaData *)(header->pBuffer);
    BufferMeta *bufferMeta = (BufferMeta *)(header->pAppPrivate);
    if (bufferMeta->getPortIndex() != portIndex) {
        ALOGW("buffer %u has an incorrect port index.", buffer);
        android_errorWriteLog(0x534e4554, "28816827");
        return UNKNOWN_ERROR;
    }
    bufferMeta->setGraphicBuffer(graphicBuffer);
    metadata->eType = kMetadataBufferTypeGrallocSource;
    metadata->pHandle = graphicBuffer->handle;

    return OK;
}

status_t OMXNodeInstance::createInputSurface(
        OMX_U32 portIndex, sp<IGraphicBufferProducer> *bufferProducer) {
    Mutex::Autolock autolock(mLock);
    status_t err;

    // only allow graphic source on input port, when there are no allocated buffers yet
    if (portIndex != kPortIndexInput) {
        android_errorWriteLog(0x534e4554, "29422020");
        return BAD_VALUE;
    } else if (mActiveBuffers.size() > 0) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }

    const sp<GraphicBufferSource> surfaceCheck = getGraphicBufferSource();
    if (surfaceCheck != NULL) {
        return ALREADY_EXISTS;
    }

    // Input buffers will hold meta-data (gralloc references).
    err = storeMetaDataInBuffers_l(portIndex, OMX_TRUE);
    if (err != OK) {
        return err;
    }

    // Retrieve the width and height of the graphic buffer, set when the
    // codec was configured.
    OMX_PARAM_PORTDEFINITIONTYPE def;
    def.nSize = sizeof(def);
    def.nVersion.s.nVersionMajor = 1;
    def.nVersion.s.nVersionMinor = 0;
    def.nVersion.s.nRevision = 0;
    def.nVersion.s.nStep = 0;
    def.nPortIndex = portIndex;
    OMX_ERRORTYPE oerr = OMX_GetParameter(
            mHandle, OMX_IndexParamPortDefinition, &def);
    CHECK(oerr == OMX_ErrorNone);

    if (def.format.video.eColorFormat != OMX_COLOR_FormatAndroidOpaque) {
        ALOGE("createInputSurface requires COLOR_FormatSurface "
              "(AndroidOpaque) color format");
        return INVALID_OPERATION;
    }

    GraphicBufferSource* bufferSource = new GraphicBufferSource(
            this, def.format.video.nFrameWidth, def.format.video.nFrameHeight,
            def.nBufferCountActual);
    if ((err = bufferSource->initCheck()) != OK) {
        delete bufferSource;
        return err;
    }
    setGraphicBufferSource(bufferSource);

    *bufferProducer = bufferSource->getIGraphicBufferProducer();
    return OK;
}

status_t OMXNodeInstance::signalEndOfInputStream() {
    // For non-Surface input, the MediaCodec should convert the call to a
    // pair of requests (dequeue input buffer, queue input buffer with EOS
    // flag set).  Seems easier than doing the equivalent from here.
    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource == NULL) {
        ALOGW("signalEndOfInputStream can only be used with Surface input");
        return INVALID_OPERATION;
    };
    return bufferSource->signalEndOfInputStream();
}

status_t OMXNodeInstance::allocateBuffer(
        OMX_U32 portIndex, size_t size, OMX::buffer_id *buffer,
        void **buffer_data) {
    Mutex::Autolock autoLock(mLock);
    // We do not support metadata mode changes past buffer allocation
    mSailed = true;

    BufferMeta *buffer_meta = new BufferMeta(size, portIndex);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_AllocateBuffer(
            mHandle, &header, portIndex, buffer_meta, size);

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_AllocateBuffer failed with error %d (0x%08x)", err, err);

        delete buffer_meta;
        buffer_meta = NULL;

        *buffer = 0;

        return UNKNOWN_ERROR;
    }

    CHECK_EQ(header->pAppPrivate, buffer_meta);

    *buffer = header;
    *buffer_data = header->pBuffer;

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }

    return OK;
}

status_t OMXNodeInstance::allocateBufferWithBackup(
        OMX_U32 portIndex, const sp<IMemory> &params,
        OMX::buffer_id *buffer, OMX_BOOL crossProcess) {
    Mutex::Autolock autoLock(mLock);
    if (portIndex >= NELEM(mUsingMetadata)) {
        return BAD_VALUE;
    }
    // We do not support metadata mode changes past buffer allocation
    mSailed = true;

    // metadata buffers are not connected cross process
    bool isMeta = mUsingMetadata[portIndex];
    bool copy = !(crossProcess && isMeta);

    BufferMeta *buffer_meta = new BufferMeta(
            params, portIndex,
            (portIndex == kPortIndexInput) && copy /* copyToOmx */,
            (portIndex == kPortIndexOutput) && copy /* copyFromOmx */,
            NULL /* data */);

    OMX_BUFFERHEADERTYPE *header;

    OMX_ERRORTYPE err = OMX_AllocateBuffer(
            mHandle, &header, portIndex, buffer_meta, params->size());

    if (err != OMX_ErrorNone) {
        ALOGE("OMX_AllocateBuffer failed with error %d (0x%08x)", err, err);

        delete buffer_meta;
        buffer_meta = NULL;

        *buffer = 0;

        return UNKNOWN_ERROR;
    }

    CHECK_EQ(header->pAppPrivate, buffer_meta);
    memset(header->pBuffer, 0, header->nAllocLen);

    *buffer = header;

    addActiveBuffer(portIndex, *buffer);

    sp<GraphicBufferSource> bufferSource(getGraphicBufferSource());
    if (bufferSource != NULL && portIndex == kPortIndexInput) {
        bufferSource->addCodecBuffer(header);
    }

    return OK;
}

status_t OMXNodeInstance::freeBuffer(
        OMX_U32 portIndex, OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);

#ifndef QCOM_HARDWARE
    removeActiveBuffer(portIndex, buffer);
#endif

    OMX_BUFFERHEADERTYPE *header = (OMX_BUFFERHEADERTYPE *)buffer;
    BufferMeta *buffer_meta;
#ifdef SEMC_ICS_CAMERA_BLOB
    if (header->pAppPrivate) {
#endif
        buffer_meta = static_cast<BufferMeta *>(header->pAppPrivate);
#ifdef SEMC_ICS_CAMERA_BLOB
    }
#endif
    if (buffer_meta->getPortIndex() != portIndex) {
        ALOGW("buffer %u has an incorrect port index.", buffer);
        android_errorWriteLog(0x534e4554, "28816827");
        // continue freeing
    }

    OMX_ERRORTYPE err = OMX_FreeBuffer(mHandle, portIndex, header);

#ifdef QCOM_HARDWARE
    if (err != OMX_ErrorNone) {
        ALOGW("OMX_FreeBuffer failed w/ err %x, do not remove from active buffer list", err);
    } else {
        ALOGI("OMX_FreeBuffer for buffer header %p successful", header);
        removeActiveBuffer(portIndex, buffer);

#ifdef SEMC_ICS_CAMERA_BLOB
        if (header->pAppPrivate) {
#endif
            delete buffer_meta;
            buffer_meta = NULL;
#ifdef SEMC_ICS_CAMERA_BLOB
        }
#endif
    }
#else
    delete buffer_meta;
    buffer_meta = NULL;
#endif

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::fillBuffer(OMX::buffer_id buffer) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = (OMX_BUFFERHEADERTYPE *)buffer;
    header->nFilledLen = 0;
    header->nOffset = 0;
    header->nFlags = 0;

    BufferMeta *buffer_meta = static_cast<BufferMeta *>(header->pAppPrivate);
    if (buffer_meta->getPortIndex() != kPortIndexOutput) {
        ALOGW("buffer %u has an incorrect port index.", buffer);
        android_errorWriteLog(0x534e4554, "28816827");
        return UNKNOWN_ERROR;
    }

    OMX_ERRORTYPE err = OMX_FillThisBuffer(mHandle, header);

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::emptyBuffer(
        OMX::buffer_id buffer,
        OMX_U32 rangeOffset, OMX_U32 rangeLength,
        OMX_U32 flags, OMX_TICKS timestamp) {
    Mutex::Autolock autoLock(mLock);

    OMX_BUFFERHEADERTYPE *header = (OMX_BUFFERHEADERTYPE *)buffer;

    // no emptybuffer if using input surface
    if (getGraphicBufferSource() != NULL) {
        android_errorWriteLog(0x534e4554, "29422020");
        return INVALID_OPERATION;
    }

    // rangeLength and rangeOffset must be a subset of the allocated data in the buffer.
    // corner case: we permit rangeOffset == end-of-buffer with rangeLength == 0.
    if (header == NULL
            || rangeOffset > header->nAllocLen
            || rangeLength > header->nAllocLen - rangeOffset) {
        return BAD_VALUE;
    }
    header->nFilledLen = rangeLength;
    header->nOffset = rangeOffset;
    header->nFlags = flags;
    header->nTimeStamp = timestamp;

    BufferMeta *buffer_meta;
#ifdef SEMC_ICS_CAMERA_BLOB
    if (header->pAppPrivate) {
#endif
        buffer_meta = static_cast<BufferMeta *>(header->pAppPrivate);
    if (buffer_meta->getPortIndex() != kPortIndexInput) {
        ALOGW("buffer %u has an incorrect port index.", buffer);
        android_errorWriteLog(0x534e4554, "28816827");
        return UNKNOWN_ERROR;
    }
    buffer_meta->CopyToOMX(header);
#ifdef SEMC_ICS_CAMERA_BLOB
    }
#endif

    OMX_ERRORTYPE err = OMX_EmptyThisBuffer(mHandle, header);

    return StatusFromOMXError(err);
}

// like emptyBuffer, but the data is already in header->pBuffer
status_t OMXNodeInstance::emptyDirectBuffer(
        OMX_BUFFERHEADERTYPE *header,
        OMX_U32 rangeOffset, OMX_U32 rangeLength,
        OMX_U32 flags, OMX_TICKS timestamp) {
    Mutex::Autolock autoLock(mLock);

    header->nFilledLen = rangeLength;
    header->nOffset = rangeOffset;
    header->nFlags = flags;
    header->nTimeStamp = timestamp;

    OMX_ERRORTYPE err = OMX_EmptyThisBuffer(mHandle, header);
    if (err != OMX_ErrorNone) {
        ALOGW("emptyDirectBuffer failed, OMX err=0x%x", err);
    }

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::getExtensionIndex(
        const char *parameterName, OMX_INDEXTYPE *index) {
    Mutex::Autolock autoLock(mLock);

    OMX_ERRORTYPE err = OMX_GetExtensionIndex(
            mHandle, const_cast<char *>(parameterName), index);

    return StatusFromOMXError(err);
}

status_t OMXNodeInstance::setInternalOption(
        OMX_U32 portIndex,
        IOMX::InternalOptionType type,
        const void *data,
        size_t size) {
    switch (type) {
        case IOMX::INTERNAL_OPTION_SUSPEND:
        case IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY:
        case IOMX::INTERNAL_OPTION_MAX_TIMESTAMP_GAP:
        {
            const sp<GraphicBufferSource> &bufferSource =
                getGraphicBufferSource();

            if (bufferSource == NULL || portIndex != kPortIndexInput) {
                return ERROR_UNSUPPORTED;
            }

            if (type == IOMX::INTERNAL_OPTION_SUSPEND) {
                if (size != sizeof(bool)) {
                    return INVALID_OPERATION;
                }

                bool suspend = *(bool *)data;
                bufferSource->suspend(suspend);
            } else if (type ==
                    IOMX::INTERNAL_OPTION_REPEAT_PREVIOUS_FRAME_DELAY){
                if (size != sizeof(int64_t)) {
                    return INVALID_OPERATION;
                }

                int64_t delayUs = *(int64_t *)data;

                return bufferSource->setRepeatPreviousFrameDelayUs(delayUs);
            } else {
                if (size != sizeof(int64_t)) {
                    return INVALID_OPERATION;
                }

                int64_t maxGapUs = *(int64_t *)data;

                return bufferSource->setMaxTimestampGapUs(maxGapUs);
            }

            return OK;
        }

        default:
            return ERROR_UNSUPPORTED;
    }
}

void OMXNodeInstance::onMessage(const omx_message &msg) {
    const sp<GraphicBufferSource>& bufferSource(getGraphicBufferSource());

    if (msg.type == omx_message::FILL_BUFFER_DONE) {
        OMX_BUFFERHEADERTYPE *buffer =
            static_cast<OMX_BUFFERHEADERTYPE *>(
                    msg.u.extended_buffer_data.buffer);


        BufferMeta *buffer_meta;
#ifdef SEMC_ICS_CAMERA_BLOB
        if (buffer->pAppPrivate) {
#endif
            buffer_meta = static_cast<BufferMeta *>(buffer->pAppPrivate);
        if (buffer_meta->getPortIndex() != kPortIndexOutput) {
            ALOGW("buffer %u has an incorrect port index.", buffer);
            android_errorWriteLog(0x534e4554, "28816827");
        } else {
            buffer_meta->CopyFromOMX(buffer);
        }
#ifdef SEMC_ICS_CAMERA_BLOB
        }
#endif

        if (bufferSource != NULL) {
            // fix up the buffer info (especially timestamp) if needed
            bufferSource->codecBufferFilled(buffer);

            omx_message newMsg = msg;
            newMsg.u.extended_buffer_data.timestamp = buffer->nTimeStamp;
            mObserver->onMessage(newMsg);
            return;
        }
    } else if (msg.type == omx_message::EMPTY_BUFFER_DONE) {
        if (bufferSource != NULL) {
            // This is one of the buffers used exclusively by
            // GraphicBufferSource.
            // Don't dispatch a message back to ACodec, since it doesn't
            // know that anyone asked to have the buffer emptied and will
            // be very confused.

            OMX_BUFFERHEADERTYPE *buffer =
                static_cast<OMX_BUFFERHEADERTYPE *>(
                        msg.u.buffer_data.buffer);

            bufferSource->codecBufferEmptied(buffer);
            return;
        }
    }

    mObserver->onMessage(msg);
}

void OMXNodeInstance::onObserverDied(OMXMaster *master) {
    ALOGE("!!! Observer died. Quickly, do something, ... anything...");

    // Try to force shutdown of the node and hope for the best.
    freeNode(master);
}

void OMXNodeInstance::onGetHandleFailed() {
    delete this;
}

// OMXNodeInstance::OnEvent calls OMX::OnEvent, which then calls here.
// Don't try to acquire mLock here -- in rare circumstances this will hang.
void OMXNodeInstance::onEvent(
        OMX_EVENTTYPE event, OMX_U32 arg1, OMX_U32 arg2) {
    const sp<GraphicBufferSource>& bufferSource(getGraphicBufferSource());

    if (bufferSource != NULL
            && event == OMX_EventCmdComplete
            && arg1 == OMX_CommandStateSet
            && arg2 == OMX_StateExecuting) {
        bufferSource->omxExecuting();
    }

    // allow configuration if we return to the loaded state
    if (event == OMX_EventCmdComplete
            && arg1 == OMX_CommandStateSet
            && arg2 == OMX_StateLoaded) {
        mSailed = false;
    }
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnEvent(
        OMX_IN OMX_HANDLETYPE hComponent,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_EVENTTYPE eEvent,
        OMX_IN OMX_U32 nData1,
        OMX_IN OMX_U32 nData2,
        OMX_IN OMX_PTR pEventData) {
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    return instance->owner()->OnEvent(
            instance->nodeID(), eEvent, nData1, nData2, pEventData);
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnEmptyBufferDone(
        OMX_IN OMX_HANDLETYPE hComponent,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    return instance->owner()->OnEmptyBufferDone(instance->nodeID(), pBuffer);
}

// static
OMX_ERRORTYPE OMXNodeInstance::OnFillBufferDone(
        OMX_IN OMX_HANDLETYPE hComponent,
        OMX_IN OMX_PTR pAppData,
        OMX_IN OMX_BUFFERHEADERTYPE* pBuffer) {
    OMXNodeInstance *instance = static_cast<OMXNodeInstance *>(pAppData);
    if (instance->mDying) {
        return OMX_ErrorNone;
    }
    return instance->owner()->OnFillBufferDone(instance->nodeID(), pBuffer);
}

void OMXNodeInstance::addActiveBuffer(OMX_U32 portIndex, OMX::buffer_id id) {
    ActiveBuffer active;
    active.mPortIndex = portIndex;
    active.mID = id;
    mActiveBuffers.push(active);
}

void OMXNodeInstance::removeActiveBuffer(
        OMX_U32 portIndex, OMX::buffer_id id) {
    bool found = false;
    for (size_t i = 0; i < mActiveBuffers.size(); ++i) {
        if (mActiveBuffers[i].mPortIndex == portIndex
            && mActiveBuffers[i].mID == id) {
            found = true;
            mActiveBuffers.removeItemsAt(i);
            break;
        }
    }

    if (!found) {
        ALOGW("Attempt to remove an active buffer we know nothing about...");
    }
}

void OMXNodeInstance::freeActiveBuffers() {
    // Make sure to count down here, as freeBuffer will in turn remove
    // the active buffer from the vector...
    for (size_t i = mActiveBuffers.size(); i--;) {
        freeBuffer(mActiveBuffers[i].mPortIndex, mActiveBuffers[i].mID);
    }
}

}  // namespace android
