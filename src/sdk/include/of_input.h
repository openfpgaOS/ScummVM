/*
 * of_input.h -- Input subsystem API for openfpgaOS
 *
 * 2 controllers, d-pad + ABXY + L/R + sticks + triggers.
 */

#ifndef OF_INPUT_H
#define OF_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "of_input_types.h"

#ifndef OF_PC

#include "of_services.h"

/* IMPORTANT: This header keeps the polled controller snapshot in static
 * storage so the of_btn* helpers stay branch-free. Include it from
 * exactly ONE translation unit per program; multi-TU apps that include
 * of.h in several files will end up with independent snapshots. */
static of_input_state_t __of_p0, __of_p1;

static inline void of_input_poll(void) {
    OF_SVC->input_poll();
    OF_SVC->input_get_state(0, &__of_p0);
    OF_SVC->input_get_state(1, &__of_p1);
}

/* Single-player fast path: poll + get P0 in one call */
static inline void of_input_poll_p0(void) {
    OF_SVC->input_poll_p0(&__of_p0);
}

static inline int of_btn(uint32_t mask) {
    return (__of_p0.buttons & mask) != 0;
}

static inline int of_btn_pressed(uint32_t mask) {
    return (__of_p0.buttons_pressed & mask) != 0;
}

static inline int of_btn_released(uint32_t mask) {
    return (__of_p0.buttons_released & mask) != 0;
}

static inline int of_btn_p2(uint32_t mask) {
    return (__of_p1.buttons & mask) != 0;
}

static inline int of_btn_pressed_p2(uint32_t mask) {
    return (__of_p1.buttons_pressed & mask) != 0;
}

static inline int of_btn_released_p2(uint32_t mask) {
    return (__of_p1.buttons_released & mask) != 0;
}

static inline uint32_t of_input_state(int player, of_input_state_t *state) {
    OF_SVC->input_get_state(player, state);
    return state->buttons;
}

static inline void of_input_set_deadzone(int16_t deadzone) {
    OF_SVC->input_set_deadzone(deadzone);
}

#else /* OF_PC */

void     of_input_poll(void);
int      of_btn(uint32_t mask);
int      of_btn_pressed(uint32_t mask);
int      of_btn_released(uint32_t mask);
int      of_btn_p2(uint32_t mask);
int      of_btn_pressed_p2(uint32_t mask);
int      of_btn_released_p2(uint32_t mask);
uint32_t of_input_state(int player, of_input_state_t *state);
void     of_input_set_deadzone(int16_t deadzone);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_INPUT_H */
