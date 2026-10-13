#include "opensles.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CDECL __attribute__((cdecl))

typedef uint32_t SLuint32;
typedef int32_t  SLresult;
typedef uint32_t SLboolean;
typedef int16_t  SLmillibel;

#define SL_RESULT_SUCCESS 0
#define SL_BOOLEAN_FALSE  0
#define SL_BOOLEAN_TRUE   1
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

typedef struct {
    uint32_t d1;
    uint16_t d2;
    uint16_t d3;
    uint8_t  d4[8];
} SLInterfaceID_struct;

typedef const SLInterfaceID_struct *SLInterfaceID;

/* IID Definitions */
const SLInterfaceID_struct s_iid_engine = { 0x8d2ac100, 0x84d5, 0x11df, { 0x93, 0x30, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_play = { 0x969d4700, 0x84d5, 0x11df, { 0xa1, 0x44, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_bufferqueue = { 0x9f5bc080, 0x84d5, 0x11df, { 0x8e, 0x87, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_volume = { 0xa3a778c0, 0x84d5, 0x11df, { 0x90, 0x43, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_seek = { 0xa701c400, 0x84d5, 0x11df, { 0xa0, 0x85, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_playbackrate = { 0xaa5b5600, 0x84d5, 0x11df, { 0xbd, 0x21, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_dim = { 0xb4e1f700, 0x84d5, 0x11df, { 0xbb, 0xbe, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
const SLInterfaceID_struct s_iid_effectsend = { 0xba9b37c0, 0x84d5, 0x11df, { 0x98, 0x15, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };

const void *SL_IID_ENGINE = &s_iid_engine;
const void *SL_IID_PLAY = &s_iid_play;
const void *SL_IID_BUFFERQUEUE = &s_iid_bufferqueue;
const void *SL_IID_VOLUME = &s_iid_volume;
const void *SL_IID_SEEK = &s_iid_seek;
const void *SL_IID_PLAYBACKRATE = &s_iid_playbackrate;
const void *SL_IID_DYNAMICINTERFACEMANAGEMENT = &s_iid_dim;
const void *SL_IID_EFFECTSEND = &s_iid_effectsend;

/* Forward declare interface pointers */
typedef const struct SLObjectItf_ * const * SLObjectItf;
typedef const struct SLEngineItf_ * const * SLEngineItf;
typedef const struct SLPlayItf_ * const * SLPlayItf;
typedef const struct SLBufferQueueItf_ * const * SLBufferQueueItf;
typedef const struct SLVolumeItf_ * const * SLVolumeItf;

typedef void (*slBufferQueueCallback)(SLBufferQueueItf caller, void *pContext);

typedef struct {
    SLuint32 count;
    SLuint32 playIndex;
} SLBufferQueueState;

/* Buffer queue item */
#define RING_BUF_SIZE (64 * 1024)
static uint8_t g_audio_ring[RING_BUF_SIZE];
static int g_ring_read = 0;
static int g_ring_write = 0;
static SDL_AudioDeviceID g_audio_dev = 0;
static slBufferQueueCallback g_bq_callback = NULL;
static void *g_bq_context = NULL;
static SLuint32 g_play_state = SL_PLAYSTATE_STOPPED;
static SDL_mutex *g_audio_mutex = NULL;

static void sdl_audio_cb(void *userdata, Uint8 *stream, int len) {
    if (g_play_state != SL_PLAYSTATE_PLAYING) {
        memset(stream, 0, len);
        return;
    }
    
    SDL_LockMutex(g_audio_mutex);
    int available = (g_ring_write - g_ring_read + RING_BUF_SIZE) % RING_BUF_SIZE;
    int to_copy = (len < available) ? len : available;
    
    for (int i = 0; i < to_copy; i++) {
        stream[i] = g_audio_ring[g_ring_read];
        g_ring_read = (g_ring_read + 1) % RING_BUF_SIZE;
    }
    if (len > to_copy) {
        memset(stream + to_copy, 0, len - to_copy);
    }
    
    int remaining = (g_ring_write - g_ring_read + RING_BUF_SIZE) % RING_BUF_SIZE;
    SDL_UnlockMutex(g_audio_mutex);
    
    /* If buffer is getting low, request more audio data */
    if (remaining < (RING_BUF_SIZE / 2) && g_bq_callback) {
        g_bq_callback((SLBufferQueueItf)userdata, g_bq_context);
    }
}

/* Vtables */
struct SLObjectItf_ {
    SLresult (CDECL *Realize)(SLObjectItf self, SLboolean async);
    SLresult (CDECL *Resume)(SLObjectItf self, SLboolean async);
    SLresult (CDECL *GetState)(SLObjectItf self, SLuint32 *pState);
    SLresult (CDECL *GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *pInterface);
    SLresult (CDECL *RegisterCallback)(SLObjectItf self, void *callback, void *pContext);
    void (CDECL *AbortAsyncOperation)(SLObjectItf self);
    void (CDECL *Destroy)(SLObjectItf self);
    SLresult (CDECL *SetPriority)(SLObjectItf self, int32_t priority, SLboolean preemptable);
    SLresult (CDECL *GetPriority)(SLObjectItf self, int32_t *pPriority, SLboolean *pPreemptable);
    SLresult (CDECL *SetLossOfControlInterfaces)(SLObjectItf self, int16_t numInterfaces, SLInterfaceID *pInterfaceIDs, SLboolean enabled);
};

struct SLEngineItf_ {
    SLresult (CDECL *CreateLEDDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateVibraDevice)(SLEngineItf self, SLObjectItf *pDevice, SLuint32 deviceID, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateAudioPlayer)(SLEngineItf self, SLObjectItf *pPlayer, void *pAudioSrc, void *pAudioSnk, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateAudioRecorder)(SLEngineItf self, SLObjectItf *pRecorder, void *pAudioSrc, void *pAudioSnk, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateMidiPlayer)(SLEngineItf self, SLObjectItf *pPlayer, void *pMidiSrc, void *pAudioSnk, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateListener)(SLEngineItf self, SLObjectItf *pListener, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *Create3DGroup)(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateOutputMix)(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateMetadataExtractor)(SLEngineItf self, SLObjectItf *pExtractor, void *pMetadataSrc, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *CreateExtensionObject)(SLEngineItf self, SLObjectItf *pObject, void *pParameters, SLuint32 objectID, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (CDECL *QueryNumSupportedInterfaces)(SLEngineItf self, SLuint32 objectID, SLuint32 *pNumSupportedInterfaces);
    SLresult (CDECL *QuerySupportedInterfaces)(SLEngineItf self, SLuint32 objectID, SLuint32 index, SLInterfaceID *pInterfaceId);
    SLresult (CDECL *QueryNumSupportedExtensions)(SLEngineItf self, SLuint32 *pNumExtensions);
    SLresult (CDECL *QuerySupportedExtension)(SLEngineItf self, SLuint32 index, char *pExtensionName, int16_t *pNameLength);
    SLresult (CDECL *IsExtensionSupported)(SLEngineItf self, const char *pExtensionName, SLboolean *pSupported);
};

struct SLPlayItf_ {
    SLresult (CDECL *SetPlayState)(SLPlayItf self, SLuint32 state);
    SLresult (CDECL *GetPlayState)(SLPlayItf self, SLuint32 *pState);
    SLresult (CDECL *GetDuration)(SLPlayItf self, void *pMsec);
    SLresult (CDECL *GetPosition)(SLPlayItf self, void *pMsec);
    SLresult (CDECL *RegisterCallback)(SLPlayItf self, void *callback, void *pContext);
    SLresult (CDECL *SetCallbackEventsMask)(SLPlayItf self, SLuint32 eventFlags);
    SLresult (CDECL *GetCallbackEventsMask)(SLPlayItf self, SLuint32 *pEventFlags);
    SLresult (CDECL *SetMarkerPosition)(SLPlayItf self, void *mSec);
    SLresult (CDECL *ClearMarkerPosition)(SLPlayItf self);
    SLresult (CDECL *GetMarkerPosition)(SLPlayItf self, void *pMsec);
    SLresult (CDECL *SetPositionUpdatePeriod)(SLPlayItf self, void *mSec);
    SLresult (CDECL *GetPositionUpdatePeriod)(SLPlayItf self, void *pMsec);
};

struct SLBufferQueueItf_ {
    SLresult (CDECL *Enqueue)(SLBufferQueueItf self, const void *pBuffer, SLuint32 size);
    SLresult (CDECL *Clear)(SLBufferQueueItf self);
    SLresult (CDECL *GetState)(SLBufferQueueItf self, SLBufferQueueState *pState);
    SLresult (CDECL *RegisterCallback)(SLBufferQueueItf self, slBufferQueueCallback callback, void *pContext);
};

struct SLVolumeItf_ {
    SLresult (CDECL *SetVolumeLevel)(SLVolumeItf self, SLmillibel level);
    SLresult (CDECL *GetVolumeLevel)(SLVolumeItf self, SLmillibel *pLevel);
    SLresult (CDECL *GetMaxVolumeLevel)(SLVolumeItf self, SLmillibel *pMaxLevel);
    SLresult (CDECL *SetMute)(SLVolumeItf self, SLboolean mute);
    SLresult (CDECL *GetMute)(SLVolumeItf self, SLboolean *pMute);
    SLresult (CDECL *EnableStereoPosition)(SLVolumeItf self, SLboolean enable);
    SLresult (CDECL *IsEnabledStereoPosition)(SLVolumeItf self, SLboolean *pEnable);
    SLresult (CDECL *SetStereoPosition)(SLVolumeItf self, void *pPosition);
    SLresult (CDECL *GetStereoPosition)(SLVolumeItf self, void *pPosition);
};

typedef const struct SLSeekItf_ * const * SLSeekItf;
struct SLSeekItf_ {
    SLresult (CDECL *SetPosition)(SLSeekItf self, int32_t msec, uint32_t seekMode);
    SLresult (CDECL *SetLoop)(SLSeekItf self, uint32_t enable, int32_t startPos, int32_t endPos);
    SLresult (CDECL *GetLoop)(SLSeekItf self, uint32_t *pEnable, int32_t *pStartPos, int32_t *pEndPos);
};

typedef const struct SLPlaybackRateItf_ * const * SLPlaybackRateItf;
struct SLPlaybackRateItf_ {
    SLresult (CDECL *SetRate)(SLPlaybackRateItf self, int16_t rate);
    SLresult (CDECL *GetRate)(SLPlaybackRateItf self, int16_t *pRate);
    SLresult (CDECL *SetPropertyConstraints)(SLPlaybackRateItf self, uint32_t constraints);
    SLresult (CDECL *GetProperties)(SLPlaybackRateItf self, uint32_t *pProperties);
    SLresult (CDECL *GetCapabilitiesOfRate)(SLPlaybackRateItf self, int16_t rate, uint32_t *pCapabilities);
    SLresult (CDECL *GetRateRange)(SLPlaybackRateItf self, uint8_t index, int16_t *pMinRate, int16_t *pMaxRate, int16_t *pStepSize, uint32_t *pCapabilities);
};

typedef const struct SLEffectSendItf_ * const * SLEffectSendItf;
struct SLEffectSendItf_ {
    SLresult (CDECL *EnableEffectSend)(SLEffectSendItf self, const void *pAuxEffect, uint32_t enable, int16_t initialLevel);
    SLresult (CDECL *IsEnabled)(SLEffectSendItf self, const void *pAuxEffect, uint32_t *pEnable);
    SLresult (CDECL *SetDirectLevel)(SLEffectSendItf self, int16_t directLevel);
    SLresult (CDECL *GetDirectLevel)(SLEffectSendItf self, int16_t *pDirectLevel);
    SLresult (CDECL *SetSendLevel)(SLEffectSendItf self, const void *pAuxEffect, int16_t sendLevel);
    SLresult (CDECL *GetSendLevel)(SLEffectSendItf self, const void *pAuxEffect, int16_t *pSendLevel);
};

/* Vtable function implementations */
static SLresult CDECL seek_SetPosition(SLSeekItf self, int32_t msec, uint32_t seekMode) { return SL_RESULT_SUCCESS; }
static SLresult CDECL seek_SetLoop(SLSeekItf self, uint32_t enable, int32_t startPos, int32_t endPos) { return SL_RESULT_SUCCESS; }
static SLresult CDECL seek_GetLoop(SLSeekItf self, uint32_t *pEnable, int32_t *pStartPos, int32_t *pEndPos) {
    if (pEnable) *pEnable = 1;
    if (pStartPos) *pStartPos = 0;
    if (pEndPos) *pEndPos = -1;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL pr_SetRate(SLPlaybackRateItf self, int16_t rate) { return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetRate(SLPlaybackRateItf self, int16_t *pRate) { if (pRate) *pRate = 1000; return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_SetPropertyConstraints(SLPlaybackRateItf self, uint32_t constraints) { return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetProperties(SLPlaybackRateItf self, uint32_t *pProperties) { if (pProperties) *pProperties = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetCapabilitiesOfRate(SLPlaybackRateItf self, int16_t rate, uint32_t *pCapabilities) { if (pCapabilities) *pCapabilities = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetRateRange(SLPlaybackRateItf self, uint8_t index, int16_t *pMinRate, int16_t *pMaxRate, int16_t *pStepSize, uint32_t *pCapabilities) { return SL_RESULT_SUCCESS; }

static SLresult CDECL es_EnableEffectSend(SLEffectSendItf self, const void *pAuxEffect, uint32_t enable, int16_t initialLevel) { return SL_RESULT_SUCCESS; }
static SLresult CDECL es_IsEnabled(SLEffectSendItf self, const void *pAuxEffect, uint32_t *pEnable) { if (pEnable) *pEnable = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL es_SetDirectLevel(SLEffectSendItf self, int16_t directLevel) { return SL_RESULT_SUCCESS; }
static SLresult CDECL es_GetDirectLevel(SLEffectSendItf self, int16_t *pDirectLevel) { if (pDirectLevel) *pDirectLevel = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL es_SetSendLevel(SLEffectSendItf self, const void *pAuxEffect, int16_t sendLevel) { return SL_RESULT_SUCCESS; }
static SLresult CDECL es_GetSendLevel(SLEffectSendItf self, const void *pAuxEffect, int16_t *pSendLevel) { if (pSendLevel) *pSendLevel = 0; return SL_RESULT_SUCCESS; }

/* Vtable instances */
static struct SLObjectItf_ s_obj_vtable;
static struct SLEngineItf_ s_engine_vtable;
static struct SLPlayItf_   s_play_vtable;
static struct SLBufferQueueItf_ s_bq_vtable;
static struct SLVolumeItf_ s_vol_vtable;
static struct SLSeekItf_   s_seek_vtable = { seek_SetPosition, seek_SetLoop, seek_GetLoop };
static struct SLPlaybackRateItf_ s_pr_vtable = { pr_SetRate, pr_GetRate, pr_SetPropertyConstraints, pr_GetProperties, pr_GetCapabilitiesOfRate, pr_GetRateRange };
static struct SLEffectSendItf_ s_es_vtable = { es_EnableEffectSend, es_IsEnabled, es_SetDirectLevel, es_GetDirectLevel, es_SetSendLevel, es_GetSendLevel };

/* Instances */
static const struct SLObjectItf_ *s_engine_obj_ptr = &s_obj_vtable;
static const struct SLEngineItf_ *s_engine_itf_ptr = &s_engine_vtable;
static const struct SLObjectItf_ *s_mix_obj_ptr = &s_obj_vtable;
static const struct SLObjectItf_ *s_player_obj_ptr = &s_obj_vtable;
static const struct SLPlayItf_   *s_play_itf_ptr = &s_play_vtable;
static const struct SLBufferQueueItf_ *s_bq_itf_ptr = &s_bq_vtable;
static const struct SLVolumeItf_ *s_vol_itf_ptr = &s_vol_vtable;
static const struct SLSeekItf_   *s_seek_itf_ptr = &s_seek_vtable;
static const struct SLPlaybackRateItf_ *s_pr_itf_ptr = &s_pr_vtable;
static const struct SLEffectSendItf_ *s_es_itf_ptr = &s_es_vtable;

/* Vtable function implementations */
static SLresult CDECL obj_Realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_Resume(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_GetState(SLObjectItf self, SLuint32 *pState) { if (pState) *pState = 2; return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_RegisterCallback(SLObjectItf self, void *callback, void *pContext) { return SL_RESULT_SUCCESS; }
static void CDECL obj_AbortAsyncOperation(SLObjectItf self) {}
static void CDECL obj_Destroy(SLObjectItf self) {}
static SLresult CDECL obj_SetPriority(SLObjectItf self, int32_t priority, SLboolean preemptable) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_GetPriority(SLObjectItf self, int32_t *pPriority, SLboolean *pPreemptable) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_SetLossOfControlInterfaces(SLObjectItf self, int16_t numInterfaces, SLInterfaceID *pInterfaceIDs, SLboolean enabled) { return SL_RESULT_SUCCESS; }

static SLresult CDECL obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface) {
    if (!pInterface || !iid) return -1;
    if (memcmp(iid, SL_IID_ENGINE, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_engine_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_PLAY, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_play_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_BUFFERQUEUE, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_bq_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_VOLUME, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_vol_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_SEEK, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_seek_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_PLAYBACKRATE, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_pr_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_EFFECTSEND, sizeof(SLInterfaceID_struct)) == 0) {
        *(const void ***)pInterface = (const void **)&s_es_itf_ptr;
        return SL_RESULT_SUCCESS;
    }
    return -1;
}

static SLresult CDECL engine_Create3DGroup(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (pGroup) *pGroup = (SLObjectItf)&s_obj_vtable;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL engine_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (pMix) *pMix = (SLObjectItf)&s_mix_obj_ptr;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL engine_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer, void *pAudioSrc, void *pAudioSnk, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (pPlayer) *pPlayer = (SLObjectItf)&s_player_obj_ptr;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL play_SetPlayState(SLPlayItf self, SLuint32 state) {
    g_play_state = state;
    if (g_audio_dev) {
        SDL_PauseAudioDevice(g_audio_dev, (state == SL_PLAYSTATE_PLAYING) ? 0 : 1);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL play_GetPlayState(SLPlayItf self, SLuint32 *pState) {
    if (pState) *pState = g_play_state;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size) {
    if (!pBuffer || size == 0) return SL_RESULT_SUCCESS;
    
    SDL_LockMutex(g_audio_mutex);
    const uint8_t *src = (const uint8_t *)pBuffer;
    for (SLuint32 i = 0; i < size; i++) {
        g_audio_ring[g_ring_write] = src[i];
        g_ring_write = (g_ring_write + 1) % RING_BUF_SIZE;
    }
    SDL_UnlockMutex(g_audio_mutex);
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_Clear(SLBufferQueueItf self) {
    SDL_LockMutex(g_audio_mutex);
    g_ring_read = 0;
    g_ring_write = 0;
    SDL_UnlockMutex(g_audio_mutex);
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_GetState(SLBufferQueueItf self, SLBufferQueueState *pState) {
    if (pState) {
        pState->count = 1;
        pState->playIndex = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_RegisterCallback(SLBufferQueueItf self, slBufferQueueCallback callback, void *pContext) {
    g_bq_callback = callback;
    g_bq_context = pContext;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) { return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *pLevel) { if (pLevel) *pLevel = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *pMaxLevel) { if (pMaxLevel) *pMaxLevel = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_SetMute(SLVolumeItf self, SLboolean mute) { return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_GetMute(SLVolumeItf self, SLboolean *pMute) { if (pMute) *pMute = 0; return SL_RESULT_SUCCESS; }

void opensles_init(void) {
    g_audio_mutex = SDL_CreateMutex();
    
    /* Setup vtables */
    memset(&s_obj_vtable, 0, sizeof(s_obj_vtable));
    s_obj_vtable.Realize = obj_Realize;
    s_obj_vtable.Resume = obj_Resume;
    s_obj_vtable.GetState = obj_GetState;
    s_obj_vtable.GetInterface = obj_GetInterface;
    s_obj_vtable.RegisterCallback = obj_RegisterCallback;
    s_obj_vtable.AbortAsyncOperation = obj_AbortAsyncOperation;
    s_obj_vtable.Destroy = obj_Destroy;
    s_obj_vtable.SetPriority = obj_SetPriority;
    s_obj_vtable.GetPriority = obj_GetPriority;
    s_obj_vtable.SetLossOfControlInterfaces = obj_SetLossOfControlInterfaces;
    
    memset(&s_engine_vtable, 0, sizeof(s_engine_vtable));
    s_engine_vtable.Create3DGroup = engine_Create3DGroup;
    s_engine_vtable.CreateOutputMix = engine_CreateOutputMix;
    s_engine_vtable.CreateAudioPlayer = engine_CreateAudioPlayer;
    
    memset(&s_play_vtable, 0, sizeof(s_play_vtable));
    s_play_vtable.SetPlayState = play_SetPlayState;
    s_play_vtable.GetPlayState = play_GetPlayState;
    
    memset(&s_bq_vtable, 0, sizeof(s_bq_vtable));
    s_bq_vtable.Enqueue = bq_Enqueue;
    s_bq_vtable.Clear = bq_Clear;
    s_bq_vtable.GetState = bq_GetState;
    s_bq_vtable.RegisterCallback = bq_RegisterCallback;
    
    memset(&s_vol_vtable, 0, sizeof(s_vol_vtable));
    s_vol_vtable.SetVolumeLevel = vol_SetVolumeLevel;
    s_vol_vtable.GetVolumeLevel = vol_GetVolumeLevel;
    s_vol_vtable.GetMaxVolumeLevel = vol_GetMaxVolumeLevel;
    s_vol_vtable.SetMute = vol_SetMute;
    s_vol_vtable.GetMute = vol_GetMute;
    
    /* Open SDL audio device: 44100 Hz stereo 16-bit */
    SDL_AudioSpec wanted, obtained;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = sdl_audio_cb;
    wanted.userdata = (void *)&s_bq_itf_ptr;
    
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (g_audio_dev) {
        printf("[AUDIO] OpenSL ES mapped to SDL2 Audio: %d Hz, %d channels\n", obtained.freq, obtained.channels);
    } else {
        printf("[AUDIO] Failed to open SDL audio device: %s\n", SDL_GetError());
    }
}

void opensles_shutdown(void) {
    if (g_audio_dev) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    if (g_audio_mutex) {
        SDL_DestroyMutex(g_audio_mutex);
        g_audio_mutex = NULL;
    }
}

SLresult CDECL wrap_slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, void *pEngineOptions, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (pEngine) {
        *pEngine = (SLObjectItf)&s_engine_obj_ptr;
    }
    return SL_RESULT_SUCCESS;
}
