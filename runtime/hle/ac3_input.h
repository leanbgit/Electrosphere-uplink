#ifndef AC3_INPUT_H
#define AC3_INPUT_H

#include "hle.h"

enum {
    AC3_BTN_SELECT   = 1 << 0,
    AC3_BTN_L3       = 1 << 1,
    AC3_BTN_R3       = 1 << 2,
    AC3_BTN_START    = 1 << 3,
    AC3_BTN_UP       = 1 << 4,
    AC3_BTN_RIGHT    = 1 << 5,
    AC3_BTN_DOWN     = 1 << 6,
    AC3_BTN_LEFT     = 1 << 7,
    AC3_BTN_L2       = 1 << 8,
    AC3_BTN_R2       = 1 << 9,
    AC3_BTN_L1       = 1 << 10,
    AC3_BTN_R1       = 1 << 11,
    AC3_BTN_TRIANGLE = 1 << 12,
    AC3_BTN_CIRCLE   = 1 << 13,
    AC3_BTN_CROSS    = 1 << 14,
    AC3_BTN_SQUARE   = 1 << 15,
};

#define AC3_PAD_DIGITAL 0x41
#define AC3_PAD_ANALOG  0x73

typedef struct {
    int connected;
    u8  type;
    u16 buttons;
    u8  lx, ly, rx, ry;
    u8  rumble_small, rumble_large;
} ac3_pad_t;

extern ac3_pad_t ac3_pad[2];

void ac3_pad_update(ctx_t *ctx);

#endif
