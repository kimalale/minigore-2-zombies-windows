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
    MSG_REJECT = 5   /* host -> guest: reason string */
};

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
#pragma pack(pop)

#endif /* NET_TYPES_H */
