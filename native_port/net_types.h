#ifndef NET_TYPES_H
#define NET_TYPES_H

#include <stdint.h>

/* Online co-op wire protocol (TCP, little-endian).
 *
 * The host runs the game; the guest's character is a companion driven by
 * the guest's input. The host streams JPEG video and PCM audio; the guest
 * sends controller state. Every message is: u8 type, u32 length, payload. */

#define NET_DEFAULT_PORT  27015
#define NET_VERSION       2
#define NET_MAX_MESSAGE   (8u * 1024u * 1024u)
#define NET_CODE_LEN      32

enum {
    MSG_HELLO  = 1,  /* both ways: MsgHello */
    MSG_VIDEO  = 2,  /* host -> guest: MsgVideoHeader + JPEG */
    MSG_AUDIO  = 3,  /* host -> guest: s16 stereo 44.1 kHz PCM */
    MSG_INPUT  = 4,  /* guest -> host: MsgInput */
    MSG_REJECT = 5,  /* host -> guest: reason string */

    /* Own-world co-op (lockstep): each player runs the game; only inputs
     * travel. See lockstep.c. */
    MSG_LS_HELLO = 20, /* guest -> host: MsgLsHello */
    MSG_LS_SETUP = 21, /* host -> guest: MsgLsSetup */
    MSG_LS_FILE  = 22, /* host -> guest: char name[64] + file bytes (host's saves) */
    MSG_LS_START = 23, /* host -> guest: all sent, start the game */
    MSG_LS_INPUT = 24, /* both ways, every frame: MsgLsInput + events */
    MSG_LS_HASH  = 25, /* both ways, every second: MsgLsHash */
    MSG_LS_BYE   = 26  /* both ways: leaving */
};

#define LS_VERSION        1
#define LS_FILE_NAME_LEN  64
#define LS_MAX_EVENTS     32   /* input events per player per frame */

#pragma pack(push, 1)
typedef struct {
    uint32_t version;
    char code[NET_CODE_LEN];  /* optional shared join code */
} MsgHello;

typedef struct {
    uint16_t width, height;
    float p2_x, p2_y;         /* player 2 on screen, 0..1 (y down) */
    uint8_t in_level;
    uint8_t p2_alive;
    float p2_health;          /* 0..1, for the guest's health bar */
    uint8_t own_view;         /* 1: this is player 2's own camera */
} MsgVideoHeader;

/* Sticks use the game's pad convention: x right, y down (as on screen), length <= 1 */
typedef struct {
    float move_x, move_y;
    float aim_x, aim_y;
    uint8_t fire;
    uint8_t buttons;          /* reserved */
} MsgInput;
typedef struct {
    uint32_t version;         /* LS_VERSION */
    char code[NET_CODE_LEN];
    char build[32];           /* both must run the same code */
    uint32_t game_hash;       /* and the same libminigore2.so */
    uint32_t event_size;
} MsgLsHello;

typedef struct {
    int32_t width, height;    /* the game's screen: touches and UI layout depend on it */
    int32_t delay;            /* input delay, frames */
    /* simulation settings, so both worlds behave the same */
    float gun_damage, gun_ammo;
    int32_t smart_autoaim;
    int32_t ai_enabled;
    float ai_flank_fraction, ai_flank_degrees, ai_lead, ai_separation;
    int32_t ai_ranged_tactics, ai_boss_tactics;
} MsgLsSetup;

typedef struct {
    uint32_t frame;           /* the frame these inputs apply on */
    float sticks[4];          /* move x, y, aim x, y (pad convention) */
    uint8_t fresh;            /* sticks are live (else the game's own controls) */
    uint8_t event_count;      /* followed by this many input events (host only) */
} MsgLsInput;

typedef struct {
    uint32_t frame;
    uint64_t characters, entities, rng;
} MsgLsHash;
#pragma pack(pop)

#endif /* NET_TYPES_H */
