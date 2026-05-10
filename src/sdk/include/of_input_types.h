/*
 * of_input_types.h -- Canonical input ABI types and constants
 *
 * The button bitmask values and the of_input_state_t struct definition.
 * Both the SDK (api/of_input.h) and the kernel HAL (os/hal/input.h)
 * include this header so they share one source of truth without
 * duplicating the types or pulling in each other's accessor inlines.
 */

#ifndef OF_INPUT_TYPES_H
#define OF_INPUT_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_MAX_PLAYERS  2

/* Canonical button masks. Targets translate their native register
 * format into this layout in their input HAL. The Pocket APF native
 * format happens to match bit-for-bit, so its translation is a no-op,
 * but apps must still treat OF_BTN_* as the only stable contract. */
#define OF_BTN_UP       (1 << 0)
#define OF_BTN_DOWN     (1 << 1)
#define OF_BTN_LEFT     (1 << 2)
#define OF_BTN_RIGHT    (1 << 3)
#define OF_BTN_A        (1 << 4)
#define OF_BTN_B        (1 << 5)
#define OF_BTN_X        (1 << 6)
#define OF_BTN_Y        (1 << 7)
#define OF_BTN_L1       (1 << 8)
#define OF_BTN_R1       (1 << 9)
#define OF_BTN_L2       (1 << 10)
#define OF_BTN_R2       (1 << 11)
#define OF_BTN_L3       (1 << 12)
#define OF_BTN_R3       (1 << 13)
#define OF_BTN_SELECT   (1 << 14)
#define OF_BTN_START    (1 << 15)

typedef struct {
    uint32_t buttons;
    uint32_t buttons_pressed;
    uint32_t buttons_released;
    int16_t  joy_lx, joy_ly;
    int16_t  joy_rx, joy_ry;
    uint16_t trigger_l, trigger_r;
} of_input_state_t;

#ifdef __cplusplus
}
#endif

#endif /* OF_INPUT_TYPES_H */
