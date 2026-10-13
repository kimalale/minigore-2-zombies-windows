#include "opensles.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

/* Forward declare interface types */
typedef const struct SLObjectItf_ * const * SLObjectItf;
typedef const struct SLEngineItf_ * const * SLEngineItf;
typedef const struct SLPlayItf_ * const * SLPlayItf;
typedef const struct SLBufferQueueItf_ * const * SLBufferQueueItf;
typedef const struct SLVolumeItf_ * const * SLVolumeItf;
typedef const struct SLSeekItf_ * const * SLSeekItf;
typedef const struct SLPlaybackRateItf_ * const * SLPlaybackRateItf;
typedef const struct SLEffectSendItf_ * const * SLEffectSendItf;

typedef void (*slBufferQueueCallback)(SLBufferQueueItf caller, void *pContext);

typedef struct {
    SLuint32 count;
    SLuint32 playIndex;
} SLBufferQueueState;

/* OpenSL ES Audio Source / PCM descriptor */
typedef struct {
    void *pLocator;
    void *pFormat;
} SLDataSource;

typedef struct {
    SLuint32 formatType;    /* SL_DATAFORMAT_PCM = 2 */
    SLuint32 numChannels;   /* 1 = mono, 2 = stereo */
    SLuint32 samplesPerSec; /* milliHz, e.g. 44100000 */
    SLuint32 bitsPerSample; /* 16 */
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;

/* Vtable definitions */
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

struct SLSeekItf_ {
    SLresult (CDECL *SetPosition)(SLSeekItf self, int32_t msec, uint32_t seekMode);
    SLresult (CDECL *SetLoop)(SLSeekItf self, uint32_t enable, int32_t startPos, int32_t endPos);
    SLresult (CDECL *GetLoop)(SLSeekItf self, uint32_t *pEnable, int32_t *pStartPos, int32_t *pEndPos);
};

struct SLPlaybackRateItf_ {
    SLresult (CDECL *SetRate)(SLPlaybackRateItf self, int16_t rate);
    SLresult (CDECL *GetRate)(SLPlaybackRateItf self, int16_t *pRate);
    SLresult (CDECL *SetPropertyConstraints)(SLPlaybackRateItf self, uint32_t constraints);
    SLresult (CDECL *GetProperties)(SLPlaybackRateItf self, uint32_t *pProperties);
    SLresult (CDECL *GetCapabilitiesOfRate)(SLPlaybackRateItf self, int16_t rate, uint32_t *pCapabilities);
    SLresult (CDECL *GetRateRange)(SLPlaybackRateItf self, uint8_t index, int16_t *pMinRate, int16_t *pMaxRate, int16_t *pStepSize, uint32_t *pCapabilities);
};

struct SLEffectSendItf_ {
    SLresult (CDECL *EnableEffectSend)(SLEffectSendItf self, const void *pAuxEffect, uint32_t enable, int16_t initialLevel);
    SLresult (CDECL *IsEnabled)(SLEffectSendItf self, const void *pAuxEffect, uint32_t *pEnable);
    SLresult (CDECL *SetDirectLevel)(SLEffectSendItf self, int16_t directLevel);
    SLresult (CDECL *GetDirectLevel)(SLEffectSendItf self, int16_t *pDirectLevel);
    SLresult (CDECL *SetSendLevel)(SLEffectSendItf self, const void *pAuxEffect, int16_t sendLevel);
    SLresult (CDECL *GetSendLevel)(SLEffectSendItf self, const void *pAuxEffect, int16_t *pSendLevel);
};

/* Forward declare OpenSLPlayer */
struct OpenSLPlayer;

typedef struct {
    const struct SLObjectItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerObjItf;

typedef struct {
    const struct SLPlayItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerPlayItf;

typedef struct {
    const struct SLBufferQueueItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerBQItf;

typedef struct {
    const struct SLVolumeItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerVolItf;

typedef struct {
    const struct SLSeekItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerSeekItf;

typedef struct {
    const struct SLPlaybackRateItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerPRItf;

typedef struct {
    const struct SLEffectSendItf_ *vtable;
    struct OpenSLPlayer *player;
} PlayerESItf;

#define MAX_OPENSL_PLAYERS 64
#define PLAYER_RING_SIZE (128 * 1024)
#define MAX_PENDING_BUFFERS 32

typedef struct {
    uint32_t bytes_remaining;
} PendingBuffer;

typedef struct OpenSLPlayer {
    PlayerObjItf  obj_itf;
    PlayerPlayItf play_itf;
    PlayerBQItf   bq_itf;
    PlayerVolItf  vol_itf;
    PlayerSeekItf seek_itf;
    PlayerPRItf   pr_itf;
    PlayerESItf   es_itf;
    
    int in_use;
    SLuint32 play_state;
    SLmillibel volume_level;
    
    int channels;    /* 1 = mono, 2 = stereo */
    int sample_rate; /* 44100 */
    
    uint8_t ring_buf[PLAYER_RING_SIZE];
    uint32_t ring_read;
    uint32_t ring_write;
    
    PendingBuffer pending[MAX_PENDING_BUFFERS];
    int pending_head;
    int pending_tail;
    int pending_count;
    
    uint32_t play_index;
    
    slBufferQueueCallback bq_callback;
    void *bq_context;
} OpenSLPlayer;

/* Engine and Mix singletons */
typedef struct {
    const struct SLObjectItf_ *vtable;
} GlobalObjItf;

typedef struct {
    const struct SLEngineItf_ *vtable;
} GlobalEngineItf;

static GlobalObjItf    s_engine_obj;
static GlobalEngineItf s_engine_itf;
static GlobalObjItf    s_mix_obj;

static OpenSLPlayer g_players[MAX_OPENSL_PLAYERS];
static SDL_AudioDeviceID g_audio_dev = 0;
static SDL_mutex *g_audio_mutex = NULL;

/* Vtable instances */
static struct SLObjectItf_       s_engine_obj_vtable;
static struct SLObjectItf_       s_player_obj_vtable;
static struct SLEngineItf_       s_engine_vtable;
static struct SLPlayItf_         s_play_vtable;
static struct SLBufferQueueItf_  s_bq_vtable;
static struct SLVolumeItf_       s_vol_vtable;
static struct SLSeekItf_         s_seek_vtable;
static struct SLPlaybackRateItf_ s_pr_vtable;
static struct SLEffectSendItf_   s_es_vtable;

/* Audio Mixer Callback */
static void sdl_audio_cb(void *userdata, Uint8 *stream, int len) {
    memset(stream, 0, len);
    
    int16_t *out = (int16_t *)stream;
    int samples_to_render = len / sizeof(int16_t);
    
    static int32_t mix[8192];
    if (samples_to_render > 8192) samples_to_render = 8192;
    memset(mix, 0, samples_to_render * sizeof(int32_t));
    
    OpenSLPlayer *cb_players[MAX_OPENSL_PLAYERS];
    int cb_count = 0;
    
    SDL_LockMutex(g_audio_mutex);
    for (int p = 0; p < MAX_OPENSL_PLAYERS; p++) {
        OpenSLPlayer *player = &g_players[p];
        if (!player->in_use || player->play_state != SL_PLAYSTATE_PLAYING) continue;
        
        int available = (player->ring_write - player->ring_read + PLAYER_RING_SIZE) % PLAYER_RING_SIZE;
        if (available == 0) {
            if (player->pending_count > 0) {
                while (player->pending_count > 0) {
                    player->pending_head = (player->pending_head + 1) % MAX_PENDING_BUFFERS;
                    player->pending_count--;
                    player->play_index++;
                    if (player->bq_callback && cb_count < MAX_OPENSL_PLAYERS) {
                        cb_players[cb_count++] = player;
                    }
                }
            }
            continue;
        }
        
        float vol = 1.0f;
        if (player->volume_level < 0) {
            vol = powf(10.0f, (float)player->volume_level / 2000.0f);
            if (vol < 0.0f) vol = 0.0f;
        }
        int vol_int = (int)(vol * 256.0f);
        
        int bytes_to_read = (available < len) ? available : len;
        bytes_to_read &= ~3; /* Align to 4-byte stereo frames */
        int samples_to_mix = bytes_to_read / sizeof(int16_t);
        
        for (int i = 0; i < samples_to_mix; i++) {
            int16_t s = *(int16_t *)&player->ring_buf[player->ring_read];
            player->ring_read = (player->ring_read + 2) % PLAYER_RING_SIZE;
            mix[i] += (s * vol_int) >> 8;
        }
        
        /* Decrement byte counter from pending buffer FIFO */
        int bytes_left = bytes_to_read;
        while (bytes_left > 0 && player->pending_count > 0) {
            uint32_t *p_rem = &player->pending[player->pending_head].bytes_remaining;
            if ((uint32_t)bytes_left >= *p_rem) {
                bytes_left -= *p_rem;
                *p_rem = 0;
                player->pending_head = (player->pending_head + 1) % MAX_PENDING_BUFFERS;
                player->pending_count--;
                player->play_index++;
                if (player->bq_callback && cb_count < MAX_OPENSL_PLAYERS) {
                    cb_players[cb_count++] = player;
                }
            } else {
                *p_rem -= bytes_left;
                bytes_left = 0;
            }
        }
        
        /* If ring buffer emptied out, drain all remaining pending buffers */
        int rem = (player->ring_write - player->ring_read + PLAYER_RING_SIZE) % PLAYER_RING_SIZE;
        if (rem == 0 && player->pending_count > 0) {
            while (player->pending_count > 0) {
                player->pending_head = (player->pending_head + 1) % MAX_PENDING_BUFFERS;
                player->pending_count--;
                player->play_index++;
                if (player->bq_callback && cb_count < MAX_OPENSL_PLAYERS) {
                    cb_players[cb_count++] = player;
                }
            }
        }
    }
    SDL_UnlockMutex(g_audio_mutex);
    
    for (int i = 0; i < samples_to_render; i++) {
        int32_t s = mix[i];
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        out[i] = (int16_t)s;
    }
    
    /* Fire completed buffer callbacks outside the audio mutex */
    for (int i = 0; i < cb_count; i++) {
        OpenSLPlayer *pl = cb_players[i];
        if (pl->bq_callback) {
            pl->bq_callback((SLBufferQueueItf)&pl->bq_itf, pl->bq_context);
        }
    }
}

/* Object Interface Implementations */
static SLresult CDECL obj_Realize(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_Resume(SLObjectItf self, SLboolean async) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_GetState(SLObjectItf self, SLuint32 *pState) { if (pState) *pState = 2; return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_RegisterCallback(SLObjectItf self, void *callback, void *pContext) { return SL_RESULT_SUCCESS; }
static void CDECL obj_AbortAsyncOperation(SLObjectItf self) {}
static SLresult CDECL obj_SetPriority(SLObjectItf self, int32_t priority, SLboolean preemptable) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_GetPriority(SLObjectItf self, int32_t *pPriority, SLboolean *pPreemptable) { return SL_RESULT_SUCCESS; }
static SLresult CDECL obj_SetLossOfControlInterfaces(SLObjectItf self, int16_t numInterfaces, SLInterfaceID *pInterfaceIDs, SLboolean enabled) { return SL_RESULT_SUCCESS; }

static void CDECL obj_Destroy(SLObjectItf self) {
    if (!self) return;
    PlayerObjItf *obj = (PlayerObjItf *)self;
    OpenSLPlayer *player = obj->player;
    if (player) {
        SDL_LockMutex(g_audio_mutex);
        player->in_use = 0;
        player->play_state = SL_PLAYSTATE_STOPPED;
        player->pending_count = 0;
        player->pending_head = 0;
        player->pending_tail = 0;
        player->ring_read = 0;
        player->ring_write = 0;
        player->bq_callback = NULL;
        player->bq_context = NULL;
        SDL_UnlockMutex(g_audio_mutex);
    }
}

static SLresult CDECL engine_obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface) {
    if (!pInterface || !iid) return -1;
    if (memcmp(iid, SL_IID_ENGINE, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &s_engine_itf;
        return SL_RESULT_SUCCESS;
    }
    return -1;
}

static SLresult CDECL player_obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface) {
    if (!pInterface || !iid || !self) return -1;
    PlayerObjItf *obj = (PlayerObjItf *)self;
    OpenSLPlayer *player = obj->player;
    if (!player) return -1;
    
    if (memcmp(iid, SL_IID_PLAY, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &player->play_itf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_BUFFERQUEUE, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &player->bq_itf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_VOLUME, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &player->vol_itf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_SEEK, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &player->seek_itf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_PLAYBACKRATE, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &player->pr_itf;
        return SL_RESULT_SUCCESS;
    }
    if (memcmp(iid, SL_IID_EFFECTSEND, sizeof(SLInterfaceID_struct)) == 0) {
        *(void **)pInterface = &player->es_itf;
        return SL_RESULT_SUCCESS;
    }
    return -1;
}

/* Engine Interface Implementations */
static SLresult CDECL engine_Create3DGroup(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (pGroup) *pGroup = (SLObjectItf)&s_mix_obj;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL engine_CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (pMix) *pMix = (SLObjectItf)&s_mix_obj;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL engine_CreateAudioPlayer(SLEngineItf self, SLObjectItf *pPlayer, void *pAudioSrc, void *pAudioSnk, SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired) {
    if (!pPlayer) return -1;
    
    SDL_LockMutex(g_audio_mutex);
    OpenSLPlayer *player = NULL;
    for (int i = 0; i < MAX_OPENSL_PLAYERS; i++) {
        if (!g_players[i].in_use) {
            player = &g_players[i];
            break;
        }
    }
    if (!player) {
        player = &g_players[0];
    }
    
    memset(player->ring_buf, 0, sizeof(player->ring_buf));
    player->ring_read = 0;
    player->ring_write = 0;
    player->pending_head = 0;
    player->pending_tail = 0;
    player->pending_count = 0;
    player->play_index = 0;
    player->play_state = SL_PLAYSTATE_STOPPED;
    player->volume_level = 0;
    player->bq_callback = NULL;
    player->bq_context = NULL;
    player->channels = 2;
    player->sample_rate = 44100;
    
    if (pAudioSrc) {
        SLDataSource *src = (SLDataSource *)pAudioSrc;
        if (src->pFormat) {
            SLDataFormat_PCM *pcm = (SLDataFormat_PCM *)src->pFormat;
            if (pcm->formatType == 2 /* SL_DATAFORMAT_PCM */) {
                if (pcm->numChannels == 1 || pcm->numChannels == 2) {
                    player->channels = pcm->numChannels;
                }
                if (pcm->samplesPerSec > 0) {
                    player->sample_rate = pcm->samplesPerSec / 1000;
                }
            }
        }
    }
    
    player->obj_itf.vtable = &s_player_obj_vtable;
    player->obj_itf.player = player;
    
    player->play_itf.vtable = &s_play_vtable;
    player->play_itf.player = player;
    
    player->bq_itf.vtable = &s_bq_vtable;
    player->bq_itf.player = player;
    
    player->vol_itf.vtable = &s_vol_vtable;
    player->vol_itf.player = player;
    
    player->seek_itf.vtable = &s_seek_vtable;
    player->seek_itf.player = player;
    
    player->pr_itf.vtable = &s_pr_vtable;
    player->pr_itf.player = player;
    
    player->es_itf.vtable = &s_es_vtable;
    player->es_itf.player = player;
    
    player->in_use = 1;
    SDL_UnlockMutex(g_audio_mutex);
    
    *pPlayer = (SLObjectItf)&player->obj_itf;
    return SL_RESULT_SUCCESS;
}

/* Play Interface Implementations */
static SLresult CDECL play_SetPlayState(SLPlayItf self, SLuint32 state) {
    if (!self) return SL_RESULT_SUCCESS;
    PlayerPlayItf *itf = (PlayerPlayItf *)self;
    OpenSLPlayer *player = itf->player;
    if (player) {
        player->play_state = state;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL play_GetPlayState(SLPlayItf self, SLuint32 *pState) {
    if (!self || !pState) return SL_RESULT_SUCCESS;
    PlayerPlayItf *itf = (PlayerPlayItf *)self;
    OpenSLPlayer *player = itf->player;
    if (player) {
        *pState = player->play_state;
    } else {
        *pState = SL_PLAYSTATE_STOPPED;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL play_GetDuration(SLPlayItf self, void *pMsec) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_GetPosition(SLPlayItf self, void *pMsec) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_RegisterCallback(SLPlayItf self, void *callback, void *pContext) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_SetCallbackEventsMask(SLPlayItf self, SLuint32 eventFlags) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_GetCallbackEventsMask(SLPlayItf self, SLuint32 *pEventFlags) { if (pEventFlags) *pEventFlags = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL play_SetMarkerPosition(SLPlayItf self, void *mSec) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_ClearMarkerPosition(SLPlayItf self) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_GetMarkerPosition(SLPlayItf self, void *pMsec) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_SetPositionUpdatePeriod(SLPlayItf self, void *mSec) { return SL_RESULT_SUCCESS; }
static SLresult CDECL play_GetPositionUpdatePeriod(SLPlayItf self, void *pMsec) { return SL_RESULT_SUCCESS; }

/* BufferQueue Interface Implementations */
static SLresult CDECL bq_Enqueue(SLBufferQueueItf self, const void *pBuffer, SLuint32 size) {
    if (!self || !pBuffer || size == 0) return SL_RESULT_SUCCESS;
    PlayerBQItf *itf = (PlayerBQItf *)self;
    OpenSLPlayer *player = itf->player;
    if (!player) return SL_RESULT_SUCCESS;
    
    SDL_LockMutex(g_audio_mutex);
    uint32_t bytes_written = 0;
    if (player->channels == 1) {
        /* Expand 16-bit Mono PCM to 16-bit Stereo PCM */
        const int16_t *src = (const int16_t *)pBuffer;
        uint32_t num_samples = size / sizeof(int16_t);
        for (uint32_t i = 0; i < num_samples; i++) {
            int16_t s = src[i];
            *(int16_t *)&player->ring_buf[player->ring_write] = s;
            player->ring_write = (player->ring_write + 2) % PLAYER_RING_SIZE;
            *(int16_t *)&player->ring_buf[player->ring_write] = s;
            player->ring_write = (player->ring_write + 2) % PLAYER_RING_SIZE;
        }
        bytes_written = num_samples * 4;
    } else {
        /* Standard 16-bit Stereo PCM */
        const uint8_t *src = (const uint8_t *)pBuffer;
        for (uint32_t i = 0; i < size; i++) {
            player->ring_buf[player->ring_write] = src[i];
            player->ring_write = (player->ring_write + 1) % PLAYER_RING_SIZE;
        }
        bytes_written = size;
    }
    
    if (player->pending_count < MAX_PENDING_BUFFERS) {
        player->pending[player->pending_tail].bytes_remaining = bytes_written;
        player->pending_tail = (player->pending_tail + 1) % MAX_PENDING_BUFFERS;
        player->pending_count++;
    }
    SDL_UnlockMutex(g_audio_mutex);
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_Clear(SLBufferQueueItf self) {
    if (!self) return SL_RESULT_SUCCESS;
    PlayerBQItf *itf = (PlayerBQItf *)self;
    OpenSLPlayer *player = itf->player;
    if (player) {
        SDL_LockMutex(g_audio_mutex);
        player->ring_read = 0;
        player->ring_write = 0;
        player->pending_head = 0;
        player->pending_tail = 0;
        player->pending_count = 0;
        SDL_UnlockMutex(g_audio_mutex);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_GetState(SLBufferQueueItf self, SLBufferQueueState *pState) {
    if (!pState) return SL_RESULT_SUCCESS;
    if (!self) {
        pState->count = 0;
        pState->playIndex = 0;
        return SL_RESULT_SUCCESS;
    }
    PlayerBQItf *itf = (PlayerBQItf *)self;
    OpenSLPlayer *player = itf->player;
    if (player) {
        SDL_LockMutex(g_audio_mutex);
        pState->count = (SLuint32)player->pending_count;
        pState->playIndex = player->play_index;
        SDL_UnlockMutex(g_audio_mutex);
    } else {
        pState->count = 0;
        pState->playIndex = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL bq_RegisterCallback(SLBufferQueueItf self, slBufferQueueCallback callback, void *pContext) {
    if (!self) return SL_RESULT_SUCCESS;
    PlayerBQItf *itf = (PlayerBQItf *)self;
    OpenSLPlayer *player = itf->player;
    if (player) {
        SDL_LockMutex(g_audio_mutex);
        player->bq_callback = callback;
        player->bq_context = pContext;
        SDL_UnlockMutex(g_audio_mutex);
    }
    return SL_RESULT_SUCCESS;
}

/* Volume Interface Implementations */
static SLresult CDECL vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) {
    if (!self) return SL_RESULT_SUCCESS;
    PlayerVolItf *itf = (PlayerVolItf *)self;
    if (itf->player) {
        itf->player->volume_level = level;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *pLevel) {
    if (!self || !pLevel) return SL_RESULT_SUCCESS;
    PlayerVolItf *itf = (PlayerVolItf *)self;
    if (itf->player) {
        *pLevel = itf->player->volume_level;
    } else {
        *pLevel = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel *pMaxLevel) {
    if (pMaxLevel) *pMaxLevel = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult CDECL vol_SetMute(SLVolumeItf self, SLboolean mute) { return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_GetMute(SLVolumeItf self, SLboolean *pMute) { if (pMute) *pMute = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_EnableStereoPosition(SLVolumeItf self, SLboolean enable) { return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean *pEnable) { if (pEnable) *pEnable = 1; return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_SetStereoPosition(SLVolumeItf self, void *pPosition) { return SL_RESULT_SUCCESS; }
static SLresult CDECL vol_GetStereoPosition(SLVolumeItf self, void *pPosition) { return SL_RESULT_SUCCESS; }

/* Seek Interface Implementations */
static SLresult CDECL seek_SetPosition(SLSeekItf self, int32_t msec, uint32_t seekMode) { return SL_RESULT_SUCCESS; }
static SLresult CDECL seek_SetLoop(SLSeekItf self, uint32_t enable, int32_t startPos, int32_t endPos) { return SL_RESULT_SUCCESS; }
static SLresult CDECL seek_GetLoop(SLSeekItf self, uint32_t *pEnable, int32_t *pStartPos, int32_t *pEndPos) {
    if (pEnable) *pEnable = 1;
    if (pStartPos) *pStartPos = 0;
    if (pEndPos) *pEndPos = -1;
    return SL_RESULT_SUCCESS;
}

/* PlaybackRate Interface Implementations */
static SLresult CDECL pr_SetRate(SLPlaybackRateItf self, int16_t rate) { return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetRate(SLPlaybackRateItf self, int16_t *pRate) { if (pRate) *pRate = 1000; return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_SetPropertyConstraints(SLPlaybackRateItf self, uint32_t constraints) { return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetProperties(SLPlaybackRateItf self, uint32_t *pProperties) { if (pProperties) *pProperties = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetCapabilitiesOfRate(SLPlaybackRateItf self, int16_t rate, uint32_t *pCapabilities) { if (pCapabilities) *pCapabilities = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL pr_GetRateRange(SLPlaybackRateItf self, uint8_t index, int16_t *pMinRate, int16_t *pMaxRate, int16_t *pStepSize, uint32_t *pCapabilities) { return SL_RESULT_SUCCESS; }

/* EffectSend Interface Implementations */
static SLresult CDECL es_EnableEffectSend(SLEffectSendItf self, const void *pAuxEffect, uint32_t enable, int16_t initialLevel) { return SL_RESULT_SUCCESS; }
static SLresult CDECL es_IsEnabled(SLEffectSendItf self, const void *pAuxEffect, uint32_t *pEnable) { if (pEnable) *pEnable = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL es_SetDirectLevel(SLEffectSendItf self, int16_t directLevel) { return SL_RESULT_SUCCESS; }
static SLresult CDECL es_GetDirectLevel(SLEffectSendItf self, int16_t *pDirectLevel) { if (pDirectLevel) *pDirectLevel = 0; return SL_RESULT_SUCCESS; }
static SLresult CDECL es_SetSendLevel(SLEffectSendItf self, const void *pAuxEffect, int16_t sendLevel) { return SL_RESULT_SUCCESS; }
static SLresult CDECL es_GetSendLevel(SLEffectSendItf self, const void *pAuxEffect, int16_t *pSendLevel) { if (pSendLevel) *pSendLevel = 0; return SL_RESULT_SUCCESS; }

void opensles_init(void) {
    g_audio_mutex = SDL_CreateMutex();
    memset(g_players, 0, sizeof(g_players));
    
    /* Setup engine object vtable */
    memset(&s_engine_obj_vtable, 0, sizeof(s_engine_obj_vtable));
    s_engine_obj_vtable.Realize = obj_Realize;
    s_engine_obj_vtable.Resume = obj_Resume;
    s_engine_obj_vtable.GetState = obj_GetState;
    s_engine_obj_vtable.GetInterface = engine_obj_GetInterface;
    s_engine_obj_vtable.RegisterCallback = obj_RegisterCallback;
    s_engine_obj_vtable.AbortAsyncOperation = obj_AbortAsyncOperation;
    s_engine_obj_vtable.Destroy = obj_Destroy;
    s_engine_obj_vtable.SetPriority = obj_SetPriority;
    s_engine_obj_vtable.GetPriority = obj_GetPriority;
    s_engine_obj_vtable.SetLossOfControlInterfaces = obj_SetLossOfControlInterfaces;
    
    s_engine_obj.vtable = &s_engine_obj_vtable;
    s_mix_obj.vtable = &s_engine_obj_vtable;
    
    /* Setup player object vtable */
    memset(&s_player_obj_vtable, 0, sizeof(s_player_obj_vtable));
    s_player_obj_vtable.Realize = obj_Realize;
    s_player_obj_vtable.Resume = obj_Resume;
    s_player_obj_vtable.GetState = obj_GetState;
    s_player_obj_vtable.GetInterface = player_obj_GetInterface;
    s_player_obj_vtable.RegisterCallback = obj_RegisterCallback;
    s_player_obj_vtable.AbortAsyncOperation = obj_AbortAsyncOperation;
    s_player_obj_vtable.Destroy = obj_Destroy;
    s_player_obj_vtable.SetPriority = obj_SetPriority;
    s_player_obj_vtable.GetPriority = obj_GetPriority;
    s_player_obj_vtable.SetLossOfControlInterfaces = obj_SetLossOfControlInterfaces;
    
    /* Setup engine vtable */
    memset(&s_engine_vtable, 0, sizeof(s_engine_vtable));
    s_engine_vtable.Create3DGroup = engine_Create3DGroup;
    s_engine_vtable.CreateOutputMix = engine_CreateOutputMix;
    s_engine_vtable.CreateAudioPlayer = engine_CreateAudioPlayer;
    s_engine_itf.vtable = &s_engine_vtable;
    
    /* Setup player play vtable */
    memset(&s_play_vtable, 0, sizeof(s_play_vtable));
    s_play_vtable.SetPlayState = play_SetPlayState;
    s_play_vtable.GetPlayState = play_GetPlayState;
    s_play_vtable.GetDuration = play_GetDuration;
    s_play_vtable.GetPosition = play_GetPosition;
    s_play_vtable.RegisterCallback = play_RegisterCallback;
    s_play_vtable.SetCallbackEventsMask = play_SetCallbackEventsMask;
    s_play_vtable.GetCallbackEventsMask = play_GetCallbackEventsMask;
    s_play_vtable.SetMarkerPosition = play_SetMarkerPosition;
    s_play_vtable.ClearMarkerPosition = play_ClearMarkerPosition;
    s_play_vtable.GetMarkerPosition = play_GetMarkerPosition;
    s_play_vtable.SetPositionUpdatePeriod = play_SetPositionUpdatePeriod;
    s_play_vtable.GetPositionUpdatePeriod = play_GetPositionUpdatePeriod;
    
    /* Setup player buffer queue vtable */
    memset(&s_bq_vtable, 0, sizeof(s_bq_vtable));
    s_bq_vtable.Enqueue = bq_Enqueue;
    s_bq_vtable.Clear = bq_Clear;
    s_bq_vtable.GetState = bq_GetState;
    s_bq_vtable.RegisterCallback = bq_RegisterCallback;
    
    /* Setup player volume vtable */
    memset(&s_vol_vtable, 0, sizeof(s_vol_vtable));
    s_vol_vtable.SetVolumeLevel = vol_SetVolumeLevel;
    s_vol_vtable.GetVolumeLevel = vol_GetVolumeLevel;
    s_vol_vtable.GetMaxVolumeLevel = vol_GetMaxVolumeLevel;
    s_vol_vtable.SetMute = vol_SetMute;
    s_vol_vtable.GetMute = vol_GetMute;
    s_vol_vtable.EnableStereoPosition = vol_EnableStereoPosition;
    s_vol_vtable.IsEnabledStereoPosition = vol_IsEnabledStereoPosition;
    s_vol_vtable.SetStereoPosition = vol_SetStereoPosition;
    s_vol_vtable.GetStereoPosition = vol_GetStereoPosition;
    
    /* Setup player seek vtable */
    memset(&s_seek_vtable, 0, sizeof(s_seek_vtable));
    s_seek_vtable.SetPosition = seek_SetPosition;
    s_seek_vtable.SetLoop = seek_SetLoop;
    s_seek_vtable.GetLoop = seek_GetLoop;
    
    /* Setup player playback rate vtable */
    memset(&s_pr_vtable, 0, sizeof(s_pr_vtable));
    s_pr_vtable.SetRate = pr_SetRate;
    s_pr_vtable.GetRate = pr_GetRate;
    s_pr_vtable.SetPropertyConstraints = pr_SetPropertyConstraints;
    s_pr_vtable.GetProperties = pr_GetProperties;
    s_pr_vtable.GetCapabilitiesOfRate = pr_GetCapabilitiesOfRate;
    s_pr_vtable.GetRateRange = pr_GetRateRange;
    
    /* Setup player effect send vtable */
    memset(&s_es_vtable, 0, sizeof(s_es_vtable));
    s_es_vtable.EnableEffectSend = es_EnableEffectSend;
    s_es_vtable.IsEnabled = es_IsEnabled;
    s_es_vtable.SetDirectLevel = es_SetDirectLevel;
    s_es_vtable.GetDirectLevel = es_GetDirectLevel;
    s_es_vtable.SetSendLevel = es_SetSendLevel;
    s_es_vtable.GetSendLevel = es_GetSendLevel;
    
    /* Open SDL audio device: 44100 Hz stereo 16-bit */
    SDL_AudioSpec wanted, obtained;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = sdl_audio_cb;
    wanted.userdata = NULL;
    
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
    if (g_audio_dev) {
        printf("[AUDIO] OpenSL ES mapped to SDL2 Audio: %d Hz, %d channels (device ID %u)\n", obtained.freq, obtained.channels, g_audio_dev);
        SDL_PauseAudioDevice(g_audio_dev, 0); /* Start mixer immediately */
    } else {
        printf("[AUDIO] Failed to open SDL audio device: %s\n", SDL_GetError());
    }
    fflush(stdout);
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
        *pEngine = (SLObjectItf)&s_engine_obj;
    }
    return SL_RESULT_SUCCESS;
}
