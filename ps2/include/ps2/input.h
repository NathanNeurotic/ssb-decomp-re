/*
 * ps2/include/ps2/input.h - logical N64 controller state produced by the
 * PS2 pad layer (ps2/src/input/pad.c), consumed by osCont* (ps2/src/ultra/cont.c).
 */
#ifndef PS2_INPUT_H
#define PS2_INPUT_H

#include <stdint.h>

#define PS2_INPUT_MAX_PLAYERS 4

/* N64 button bits (same values as CONT_* / *_JPAD in PR/os.h) */
#define N64_BTN_A     0x8000
#define N64_BTN_B     0x4000
#define N64_BTN_Z     0x2000
#define N64_BTN_START 0x1000
#define N64_BTN_DU    0x0800
#define N64_BTN_DD    0x0400
#define N64_BTN_DL    0x0200
#define N64_BTN_DR    0x0100
#define N64_BTN_L     0x0020
#define N64_BTN_R     0x0010
#define N64_BTN_CU    0x0008
#define N64_BTN_CD    0x0004
#define N64_BTN_CL    0x0002
#define N64_BTN_CR    0x0001

typedef struct PS2InputState
{
    int connected;
    uint16_t buttons; /* N64 mask */
    int8_t stick_x;   /* N64 convention: right = +, up = + */
    int8_t stick_y;
} PS2InputState;

void ps2_input_init(void);
void ps2_input_poll(void);
const PS2InputState *ps2_input_state(int player);
int ps2_input_raw_combo(int player, uint16_t ps2_combo);
int ps2_input_overlay_toggle_pressed(void);
int ps2_input_clear_toggle_pressed(void);
int ps2_input_capture_pressed(void);
int ps2_input_has_rumble(int player);
void ps2_input_set_rumble(int player, int on);

#endif /* PS2_INPUT_H */
