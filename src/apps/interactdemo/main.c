/*
 * openfpgaOS Interact + Input Demo
 *
 * Two panels:
 *   1. Live interact variables from the Pocket menu
 *   2. ASCII controller — buttons light up when pressed
 *
 * 40x30 terminal, CP437 box drawing, ANSI 16-color.
 */

#include "of.h"
#include <stdio.h>
#include <unistd.h>

/* ── ANSI helpers ────────────────────────────────────────────────── */
#define CSI          "\033["
#define GOTO(r,c)    printf(CSI "%d;%dH", (r), (c))
#define CLR          printf(CSI "2J" CSI "H")
#define RESET        printf(CSI "0m")
#define BOLD         printf(CSI "1m")
#define DIM          printf(CSI "2m")
#define FG_WHITE     printf(CSI "37m")
#define FG_YELLOW    printf(CSI "93m")
#define FG_GREEN     printf(CSI "92m")
#define FG_CYAN      printf(CSI "96m")
#define FG_RED       printf(CSI "91m")
#define FG_GRAY      printf(CSI "90m")
#define BG_GREEN     printf(CSI "42m")
#define BG_DEFAULT   printf(CSI "49m")

/* ── Interact variable names ─────────────────────────────────────── */
static const char *opt1_names[] = { "Off", "On" };
static const char *opt3_names[] = { "Easy", "Normal", "Hard" };

/* ── Draw static frame (once) ────────────────────────────────────── */
static void draw_frame(void) {
    CLR;

    /* Title */
    GOTO(1, 2); FG_CYAN; BOLD;
    printf("openfpgaOS Input + Interact Demo");
    RESET;

    /* ── Interact panel ──────────────────────────────────────────── */
    GOTO(3, 2); FG_YELLOW; BOLD;
    printf("Interact");
    RESET; FG_GRAY;
    printf(" (Pocket menu)");
    RESET;

    GOTO(5, 3);  printf("Option 1:");
    GOTO(6, 3);  printf("Option 2:");
    GOTO(7, 3);  printf("Option 3:");

    /* Slider track */
    GOTO(8, 3); FG_GRAY;
    printf("[..........] 0");
    RESET;

    /* ── Controller panel ────────────────────────────────────────── */
    GOTO(11, 2); FG_YELLOW; BOLD;
    printf("Controller");
    RESET; FG_GRAY;
    printf(" (press buttons)");
    RESET;

    /*
     * ASCII controller layout (rows 13-24):
     *
     *    L1              R1
     *    L2              R2
     *
     *      ^        [Y]
     *    < + >   [X]   [A]
     *      v        [B]
     *
     *    SEL   START
     *
     *   L-Stick   R-Stick
     *   lx:  ly:  rx:  ry:
     *   LT:       RT:
     */

    /* Shoulders */
    GOTO(13, 3);  printf("[  ]");
    GOTO(13, 31); printf("[  ]");
    GOTO(14, 3);  printf("[  ]");
    GOTO(14, 31); printf("[  ]");
    GOTO(13, 4);  FG_GRAY; printf("L1"); RESET;
    GOTO(13, 32); FG_GRAY; printf("R1"); RESET;
    GOTO(14, 4);  FG_GRAY; printf("L2"); RESET;
    GOTO(14, 32); FG_GRAY; printf("R2"); RESET;

    /* D-pad frame */
    GOTO(16, 6);  printf("[ ]");
    GOTO(17, 3);  printf("[ ]");
    GOTO(17, 6);  printf("[+]");
    GOTO(17, 9);  printf("[ ]");
    GOTO(18, 6);  printf("[ ]");

    /* D-pad labels (dimmed) */
    GOTO(16, 7);  FG_GRAY; printf("%c", ACS_UARROW); RESET;
    GOTO(17, 4);  FG_GRAY; printf("%c", ACS_LARROW); RESET;
    GOTO(17, 10); FG_GRAY; printf("%c", ACS_RARROW); RESET;
    GOTO(18, 7);  FG_GRAY; printf("%c", ACS_DARROW); RESET;

    /* Face buttons */
    GOTO(16, 26); printf("[  ]");
    GOTO(17, 22); printf("[  ]");
    GOTO(17, 30); printf("[  ]");
    GOTO(18, 26); printf("[  ]");

    GOTO(16, 27); FG_GRAY; printf("X "); RESET;
    GOTO(17, 23); FG_GRAY; printf("Y "); RESET;
    GOTO(17, 31); FG_GRAY; printf("A "); RESET;
    GOTO(18, 27); FG_GRAY; printf("B "); RESET;

    /* Select / Start */
    GOTO(20, 5);  printf("[   ]");
    GOTO(20, 14); printf("[    ]");
    GOTO(20, 6);  FG_GRAY; printf("SEL"); RESET;
    GOTO(20, 15); FG_GRAY; printf("STRT"); RESET;

    /* Stick / trigger labels */
    GOTO(22, 3);  FG_GRAY; printf("L-Stick    R-Stick"); RESET;
    GOTO(23, 3);  FG_GRAY; printf("lx:     rx:"); RESET;
    GOTO(24, 3);  FG_GRAY; printf("ly:     ry:"); RESET;
    GOTO(25, 3);  FG_GRAY; printf("LT:     RT:"); RESET;

    /* Player indicator */
    GOTO(28, 2); FG_GRAY; printf("P1"); RESET;
}

/* ── Highlight helpers ───────────────────────────────────────────── */
/* Print button content: green background when held, gray when not */
static void btn(int row, int col, const char *label, int held) {
    GOTO(row, col);
    if (held) {
        BG_GREEN; FG_WHITE; BOLD;
    } else {
        BG_DEFAULT; FG_GRAY; DIM;
    }
    printf("%s", label);
    RESET;
}

/* ── Main loop ───────────────────────────────────────────────────── */
int main(void) {
    draw_frame();

    uint32_t prev_interact[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    uint32_t prev_buttons = 0xFFFFFFFF;
    int16_t prev_lx = 0, prev_ly = 0, prev_rx = 0, prev_ry = 0;
    uint16_t prev_lt = 0, prev_rt = 0;

    while (1) {
        of_input_poll();

        /* ── Interact values ─────────────────────────────────────── */
        uint32_t v0 = of_interact_get(0);
        uint32_t v1 = of_interact_get(1);
        uint32_t v2 = of_interact_get(2);

        if (v0 != prev_interact[0]) {
            prev_interact[0] = v0;
            const char *s = (v0 < 2) ? opt1_names[v0] : "???";
            GOTO(5, 13); FG_GREEN; printf("%-6s", s); RESET;
        }
        if (v1 != prev_interact[1]) {
            prev_interact[1] = v1;
            GOTO(6, 13); FG_GREEN; printf("%-3d", (int)v1); RESET;
            /* Slider bar */
            GOTO(8, 4);
            for (int i = 0; i < 10; i++) {
                if (i < (int)v1) { FG_GREEN; printf("%c", ACS_BLOCK); }
                else             { FG_GRAY;  printf("."); }
            }
            RESET;
            GOTO(8, 15); printf("%d ", (int)v1);
        }
        if (v2 != prev_interact[2]) {
            prev_interact[2] = v2;
            const char *s = (v2 < 3) ? opt3_names[v2] : "???";
            GOTO(7, 13); FG_GREEN; printf("%-8s", s); RESET;
        }

        /* ── Controller buttons ──────────────────────────────────── */
        of_input_state_t st;
        of_input_state(0, &st);
        uint32_t b = st.buttons;

        if (b != prev_buttons) {
            prev_buttons = b;

            /* D-pad */
            btn(16, 7,  " ",  b & OF_BTN_UP);
            btn(17, 4,  " ",  b & OF_BTN_LEFT);
            btn(17, 10, " ",  b & OF_BTN_RIGHT);
            btn(18, 7,  " ",  b & OF_BTN_DOWN);

            /* Face buttons */
            btn(16, 27, "X ", b & OF_BTN_X);
            btn(17, 23, "Y ", b & OF_BTN_Y);
            btn(17, 31, "A ", b & OF_BTN_A);
            btn(18, 27, "B ", b & OF_BTN_B);

            /* Shoulders */
            btn(13, 4,  "L1", b & OF_BTN_L1);
            btn(13, 32, "R1", b & OF_BTN_R1);
            btn(14, 4,  "L2", b & OF_BTN_L2);
            btn(14, 32, "R2", b & OF_BTN_R2);

            /* Select / Start */
            btn(20, 6,  "SEL",  b & OF_BTN_SELECT);
            btn(20, 15, "STRT", b & OF_BTN_START);
        }

        /* ── Analog sticks + triggers ────────────────────────────── */
        if (st.joy_lx != prev_lx || st.joy_ly != prev_ly ||
            st.joy_rx != prev_rx || st.joy_ry != prev_ry) {
            prev_lx = st.joy_lx; prev_ly = st.joy_ly;
            prev_rx = st.joy_rx; prev_ry = st.joy_ry;

            GOTO(23, 7);  FG_GREEN; printf("%-6d", prev_lx); RESET;
            GOTO(24, 7);  FG_GREEN; printf("%-6d", prev_ly); RESET;
            GOTO(23, 15); FG_GREEN; printf("%-6d", prev_rx); RESET;
            GOTO(24, 15); FG_GREEN; printf("%-6d", prev_ry); RESET;
        }

        if (st.trigger_l != prev_lt || st.trigger_r != prev_rt) {
            prev_lt = st.trigger_l; prev_rt = st.trigger_r;
            GOTO(25, 7);  FG_GREEN; printf("%-6d", prev_lt); RESET;
            GOTO(25, 15); FG_GREEN; printf("%-6d", prev_rt); RESET;
        }

        usleep(16000);  /* ~60 fps */
    }
}
