/*
 * openfpgaOS GPU Accelerator Demo
 *
 * Showcases every GPU feature:
 *   Mode 0 — Wolfenstein-style raycaster maze with an auto-walking
 *            camera (right-hand wall follower). Wall columns and
 *            floor/ceiling use colormap-lit horizontal spans (column
 *            walking is selected by fb_stride, not by a flag bit).
 *   Mode 1 — Perspective-correct textured triangle (software rasterised,
 *            using SPAN_PERSP horizontal scanlines for the inner loop —
 *            the GPU does the 1/z reciprocal + multiply per 16-pixel
 *            sub-segment in hardware).
 *   Mode 2 — Rotating 3D cube via the hardware triangle rasteriser
 *            (one DRAW_TRIANGLES command per face).
 *   Mode 3 — Pinwheel of 32 triangles drawn in a single batched
 *            DRAW_TRIANGLES command (of_gpu_draw_triangles_batch).
 *
 * Controls:
 *   A button — cycle through demo modes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "of.h"
#include "of_cache.h"
#include "of_gpu.h"

#define SCREEN_W 320
#define SCREEN_H 240

/* of_gpu_clear's pixel count is hardware-hardcoded to 320*200 = 64000 bytes
 * (= 16000 32-bit AXI bursts), so it only clears the upper 200 rows of the
 * 320x240 framebuffer. The remaining 40 rows must be cleared by the CPU. */
#define GPU_CLEAR_ROWS 200
#define LETTERBOX_ROWS (SCREEN_H - GPU_CLEAR_ROWS)

/* Ring buffer is now 16 KB M10K BRAM inside the GPU — no CPU allocation needed */

/* Texture data lives on the heap (in SDRAM). The previous static `.sdram`
 * section was an orphan with no output mapping, so the storage ended up
 * in a non-loaded segment and the GPU saw zeros. Heap allocations come
 * from the SDRAM heap and are reachable from both CPU and GPU. */
static uint8_t *checkerboard_tex;   /* 64×64 floor */
static uint8_t *wall_tex;           /* 64×64 wall  */
static uint8_t *sprite_tex;         /* 16×16, 0xFF = transparent (SPAN_SKIP_ZERO) */
static uint8_t *persp_tex;          /* 64×64 grid for the perspective demo */

/* Colormap: 64 light levels */
static uint8_t colormap[64 * 256];

/* Simple sin/cos LUT (8-bit fixed-point, 256 entries = full circle) */
static int16_t sin_lut[256];
static int16_t cos_lut[256];

static void build_sin_table(void) {
    for (int i = 0; i < 256; i++) {
        double a = i * 2.0 * 3.14159265 / 256.0;
        sin_lut[i] = (int16_t)(sin(a) * 256.0);
        cos_lut[i] = (int16_t)(cos(a) * 256.0);
    }
}

static void build_colormap(void) {
    /* Generate a simple colormap: each light level dims the palette.
     *
     * IMPORTANT: the output index must never fall below 16 — palette
     * entries 0..15 are reserved for the OS terminal's VGA 16-colour
     * set (see set_palette), so a heavily-dimmed texel that lands on
     * index 6 would render as terminal-red, not dark grey. Clamp the
     * low end to 16 and rescale the ramp into [16, i] so distant
     * surfaces fade smoothly into the reserved-safe greyscale band
     * instead of flashing VGA primaries. */
    for (int light = 0; light < 64; light++) {
        for (int i = 0; i < 256; i++) {
            int base = (i < 16) ? i : 16;
            int dimmed = base + ((i - base) * (63 - light)) / 63;
            if (dimmed > 255) dimmed = 255;
            if (dimmed < 16 && i >= 16) dimmed = 16;
            colormap[light * 256 + i] = (uint8_t)dimmed;
        }
    }
}

static void build_checkerboard(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            checkerboard_tex[y * 64 + x] = ((x ^ y) & 8) ? 0xE0 : 0x40;
}

static void build_wall_texture(void) {
    /* 8 brick rows of 8 px each, 4 bricks wide (16 px each), staggered
     * every other row. Bricks get a per-row + per-brick colour offset so
     * horizontal banding is the dominant visual cue instead of the four
     * vertical mortar columns the old pattern used to produce. */
    for (int y = 0; y < 64; y++) {
        int row    = y >> 3;                  /* 0..7 brick rows      */
        int by     = y & 7;                   /* 0..7 within brick    */
        int stagger = (row & 1) ? 8 : 0;
        for (int x = 0; x < 64; x++) {
            int sx   = (x + stagger) & 63;
            int bx   = sx & 15;               /* 0..15 within brick   */
            int brick_idx = sx >> 4;          /* 0..3 brick in row    */
            uint8_t v;
            if (by == 0 || bx == 0) {
                v = 0x28;                     /* dark mortar          */
            } else {
                int base  = 0x88 + ((row * 5 + brick_idx * 11) & 0x1f);
                int noise = ((x * 37 + y * 17) >> 1) & 0x07;
                int c     = base + noise;
                if (c > 0xFE) c = 0xFE;
                v = (uint8_t)c;
            }
            wall_tex[y * 64 + x] = v;
        }
    }
}

/* 16×16 sprite — a small filled circle, 0xFF outside (transparent) */
static void build_sprite_texture(void) {
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int dx = x - 8, dy = y - 8;
            int d2 = dx*dx + dy*dy;
            if (d2 > 49) {
                sprite_tex[y * 16 + x] = 0xFF;            /* transparent */
            } else if (d2 > 25) {
                sprite_tex[y * 16 + x] = 0x40 + d2;       /* outline */
            } else {
                sprite_tex[y * 16 + x] = 0xC0 - (d2 * 2); /* fill */
            }
        }
    }
}

/* 64×64 grid texture for the perspective demo */
static void build_persp_texture(void) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int gx = x & 7, gy = y & 7;
            uint8_t v;
            if (gx == 0 || gy == 0)
                v = 0xFF;                                 /* grid line (white) */
            else
                v = 0x20 + ((x >> 3) ^ (y >> 3)) * 0x18;  /* checker fill */
            persp_tex[y * 64 + x] = v;
        }
    }
}

static void set_palette(void) {
    /* Simple grayscale + warm tint for upper half.
     *
     * IMPORTANT: skip indexes 0-15 — those are the OS terminal's VGA
     * 16-color palette (used for white text in OF_DISPLAY_OVERLAY mode).
     * Overwriting them blanks the overlay text. */
    for (int i = 16; i < 256; i++) {
        uint8_t r = i, g = i, b = i;
        if (i >= 0x80) { r = i; g = (i * 7) >> 3; b = (i * 5) >> 3; }
        of_video_palette(i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
}

/* ================================================================
 * Maze Demo: Wolfenstein-style raycaster — walk around with the d-pad
 * ================================================================
 * Map is a 16x16 grid ('#' = wall, '.' = empty). For each screen column
 * we cast a ray through the grid with DDA, find the nearest wall hit,
 * and draw a textured vertical column-walked span (fb_stride=SCREEN_W).
 * Floor and ceiling are filled with horizontal spans using the standard
 * "row distance" floor cast (linear in screen y). Both paths feed the
 * same colormap-lit fragment processor.
 */

#define MAP_W 16
#define MAP_H 16
static const char maze[MAP_H][MAP_W + 1] = {
    "################",
    "#..............#",
    "#.####.###.###.#",
    "#.#......#.#...#",
    "#.#.####.#.#.#.#",
    "#...#......#.#.#",
    "###.#.####.#.#.#",
    "#...#.#......#.#",
    "#.###.#.######.#",
    "#.....#........#",
    "#.#####.######.#",
    "#.....#.#......#",
    "#####.#.#.######",
    "#.....#.#......#",
    "#.#####.#####.##",
    "################",
};

/* Point lights at corridor intersections, baked into a 64×64 lightgrid
 * at startup. Each cell stores the pre-computed "light reduction" (how
 * many colormap steps brighter this position is) from all 6 sources.
 *
 * At runtime the per-span cost is one array lookup + one integer
 * multiply (for flicker), replacing 6 float divisions per span. */
#define NUM_LIGHTS 6
static const struct { float x, y; float intensity; } maze_lights[NUM_LIGHTS] = {
    {  1.5f,  1.5f, 1.8f },   /* start area              */
    {  5.5f,  3.5f, 2.5f },   /* upper corridor junction  */
    {  3.5f,  9.5f, 2.2f },   /* wide corridor            */
    { 13.5f,  1.5f, 2.0f },   /* top-right corner         */
    {  5.5f, 13.5f, 2.5f },   /* lower-left junction      */
    { 13.5f,  9.5f, 2.8f },   /* right-side chamber       */
};

/* 64×64 grid covering the 16×16 maze at 4× resolution (0.25 world
 * units per cell). Each byte = colormap-step reduction from static
 * light sources, range 0..63. */
#define LGRID_SIZE  64
#define LGRID_SCALE 4
static uint8_t light_grid[LGRID_SIZE][LGRID_SIZE];

/* Maximum light range squared. Beyond this distance a light contributes
 * nothing, creating dark corridors between distinct torch pools. A
 * radius of ~3.5 world units (d² = 12) gives a visible glow that
 * fades to black well before reaching the next light. */
#define LIGHT_RANGE_SQ 12.0f

static void build_light_grid(void) {
    for (int gy = 0; gy < LGRID_SIZE; gy++) {
        float wy = ((float)gy + 0.5f) / (float)LGRID_SCALE;
        for (int gx = 0; gx < LGRID_SIZE; gx++) {
            float wx = ((float)gx + 0.5f) / (float)LGRID_SCALE;
            float total = 0.0f;
            for (int i = 0; i < NUM_LIGHTS; i++) {
                float dx = wx - maze_lights[i].x;
                float dy = wy - maze_lights[i].y;
                float d2 = dx * dx + dy * dy;
                if (d2 > LIGHT_RANGE_SQ) continue;
                if (d2 < 0.1f) d2 = 0.1f;
                total += maze_lights[i].intensity / d2;
            }
            int val = (int)(total * 12.0f);
            if (val > 63) val = 63;
            light_grid[gy][gx] = (uint8_t)val;
        }
    }
}

/* Per-frame flicker multiplier (8.8 fixed-point, ~256 = 1.0×).
 * Set once at the top of draw_maze_demo so every span in the frame
 * uses the same flicker phase. */
static int _flicker_256;

static inline int sample_light(float wx, float wy) {
    int gx = (int)(wx * LGRID_SCALE);
    int gy = (int)(wy * LGRID_SCALE);
    if (gx < 0) gx = 0; if (gx >= LGRID_SIZE) gx = LGRID_SIZE - 1;
    if (gy < 0) gy = 0; if (gy >= LGRID_SIZE) gy = LGRID_SIZE - 1;
    return (light_grid[gy][gx] * _flicker_256) >> 8;
}

static float player_x = 1.5f;
static float player_y = 1.5f;
static float player_a = 0.0f;          /* facing angle, radians */
#define MAZE_FOV 1.04719755f           /* 60° */

/* Auto-walker state. Cardinal facings: 0=E, 1=S, 2=W, 3=N. */
static const int cardinal_dx[4] = {  1,  0, -1,  0 };
static const int cardinal_dy[4] = {  0,  1,  0, -1 };
static const float cardinal_ang[4] = { 0.0f, 1.5707963f, 3.1415927f, -1.5707963f };

static int walker_cell_x = 1, walker_cell_y = 1;
static int walker_facing = 0;
static int walker_tgt_cx = 1, walker_tgt_cy = 1;
static int walker_tgt_fc = 0;
static int walker_phase  = 0;    /* 0=idle/plan, 1=moving, 2=turning */

static int map_solid(int mx, int my) {
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return 1;
    return maze[my][mx] != '.';
}

static float wrap_angle(float a) {
    while (a >  3.1415927f) a -= 6.2831853f;
    while (a < -3.1415927f) a += 6.2831853f;
    return a;
}

/* Right-hand wall follower. Each frame either animates toward the
 * current target (cell center or cardinal angle) or, when idle, picks
 * the next action: try turning right, else forward, else left, else
 * turn around. */
static void update_auto_walker(void) {
    const float move_speed = 0.02f;
    const float turn_speed = 0.03f;

    if (walker_phase == 0) {
        int right = (walker_facing + 1) & 3;
        int fwd   =  walker_facing;
        int left  = (walker_facing + 3) & 3;
        int back  = (walker_facing + 2) & 3;

        if (!map_solid(walker_cell_x + cardinal_dx[right],
                       walker_cell_y + cardinal_dy[right])) {
            walker_tgt_fc = right;
            walker_phase  = 2;
        } else if (!map_solid(walker_cell_x + cardinal_dx[fwd],
                              walker_cell_y + cardinal_dy[fwd])) {
            walker_tgt_cx = walker_cell_x + cardinal_dx[fwd];
            walker_tgt_cy = walker_cell_y + cardinal_dy[fwd];
            walker_phase  = 1;
        } else if (!map_solid(walker_cell_x + cardinal_dx[left],
                              walker_cell_y + cardinal_dy[left])) {
            walker_tgt_fc = left;
            walker_phase  = 2;
        } else {
            walker_tgt_fc = back;
            walker_phase  = 2;
        }
    }

    if (walker_phase == 1) {
        float tx = (float)walker_tgt_cx + 0.5f;
        float ty = (float)walker_tgt_cy + 0.5f;
        float dx = tx - player_x;
        float dy = ty - player_y;
        float d  = sqrtf(dx * dx + dy * dy);
        if (d <= move_speed) {
            player_x = tx;
            player_y = ty;
            walker_cell_x = walker_tgt_cx;
            walker_cell_y = walker_tgt_cy;
            walker_phase  = 0;
        } else {
            player_x += dx * (move_speed / d);
            player_y += dy * (move_speed / d);
        }
    } else if (walker_phase == 2) {
        float target = cardinal_ang[walker_tgt_fc];
        float da = wrap_angle(target - player_a);
        if (fabsf(da) <= turn_speed) {
            player_a = target;
            walker_facing = walker_tgt_fc;
            walker_phase  = 0;
        } else {
            player_a = wrap_angle(player_a + (da > 0 ? turn_speed : -turn_speed));
        }
    }
}

/* Frame-stat accumulators — written from inside the draw functions so
 * the CPU-submit / GPU-wait split can be measured without hoisting
 * of_gpu_finish() out of the draw path (which otherwise changes the
 * ring/fence ordering and destabilises the pipeline). Main loop reads
 * these once per FPS window and resets them. */
static unsigned int _stat_cpu_us = 0;
static unsigned int _stat_gpu_us = 0;

/* Per-frame FB setup. CPU clears the framebuffer (SDRAM via M2) and
 * flushes the dirty cache lines back so the GPU sees the cleared
 * bytes when it reads the framebuffer back from M0/M1. */
static void prepare_fb(uint8_t *fb, uint8_t color) {
    memset(fb, color, SCREEN_W * SCREEN_H);
    OF_SVC->cache_clean_range(fb, SCREEN_W * SCREEN_H);
    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb, SCREEN_W);
}

static void draw_maze_demo(int frame) {
    uint8_t *fb = of_video_surface();
    uint32_t fb_addr    = (uint32_t)(uintptr_t)fb;
    uint32_t wall_addr  = (uint32_t)(uintptr_t)wall_tex;
    uint32_t floor_addr = (uint32_t)(uintptr_t)checkerboard_tex;

    unsigned int _t0 = of_time_us();
    update_auto_walker();

    /* Per-frame flicker: ±15 % modulation in 8.8 fixed-point (256 = 1×). */
    _flicker_256 = 256 + (sin_lut[(frame * 3) & 255] * 38) / 256;

    /* No memset / cache_clean — floor + ceiling spans cover every row
     * (0..horizon-1 ceiling, horizon..SCREEN_H-1 floor), and walls
     * overdraw their slices on top. The GPU writes via AXI so there's
     * nothing in the CPU D-cache to flush either. */
    of_gpu_set_framebuffer((uint32_t)(uintptr_t)fb, SCREEN_W);

    float ca = cosf(player_a), sa = sinf(player_a);
    /* Camera plane is perpendicular to the facing direction, length =
     * tan(FOV/2). Rays are dir + plane*camX for camX ∈ [-1, +1]. */
    float plane_scale = tanf(MAZE_FOV * 0.5f);
    float planeX = -sa * plane_scale;
    float planeY =  ca * plane_scale;

    /* --- Floor & ceiling first, so walls overdraw them in their slice. --- */
    float rdx_l = ca - planeX, rdy_l = sa - planeY;   /* leftmost ray  */
    float rdx_r = ca + planeX, rdy_r = sa + planeY;   /* rightmost ray */
    int horizon = SCREEN_H / 2;

    /* Start from horizon (not horizon+1) so that floor covers row 120
     * and the ceiling mirror covers row 119 — closing the 2-row gap
     * that previously required a memset to fill. The +0.5 offset
     * avoids dividing by zero at the horizon itself and corresponds
     * to sampling at the centre of each pixel row. */
    for (int y = horizon; y < SCREEN_H; y++) {
        float p = (float)(y - horizon) + 0.5f;
        float row_dist = (0.5f * SCREEN_H) / p;

        float step_x = row_dist * (rdx_r - rdx_l) / (float)SCREEN_W;
        float step_y = row_dist * (rdy_r - rdy_l) / (float)SCREEN_W;
        float fx = player_x + row_dist * rdx_l;
        float fy = player_y + row_dist * rdy_l;

        /* Distance fog + lightgrid sample at the span midpoint.
         * Fog starts at 0.5 units (not 2.5) so torch pools are visible
         * even in narrow corridors — otherwise the near-bright zone
         * swallows the light contribution. */
        float mid_x = fx + step_x * (float)(SCREEN_W / 2);
        float mid_y = fy + step_y * (float)(SCREEN_W / 2);
        int light = (int)((row_dist - 0.5f) * 3.0f) - sample_light(mid_x, mid_y);
        if (light < 0) light = 0;
        if (light > 50) light = 50;

        int32_t s0    = (int32_t)(fx     * 65536.0f);
        int32_t t0    = (int32_t)(fy     * 65536.0f);
        int32_t sstep = (int32_t)(step_x * 65536.0f);
        int32_t tstep = (int32_t)(step_y * 65536.0f);

        of_gpu_span_t fs = {
            .fb_addr   = fb_addr + y * SCREEN_W,
            .tex_addr  = floor_addr,
            .s         = s0,
            .t         = t0,
            .sstep     = sstep,
            .tstep     = tstep,
            .count     = SCREEN_W,
            .light     = light,
            .flags     = OF_GPU_SPAN_COLORMAP,
            .fb_stride = 1,
            .tex_width = 64,
        };
        of_gpu_draw_span(&fs);

        /* Mirror into the ceiling row — same row_dist, different tex. */
        int cy = (SCREEN_H - 1) - y;
        if (cy >= 0 && cy < horizon) {
            of_gpu_span_t cs = fs;
            cs.fb_addr  = fb_addr + cy * SCREEN_W;
            cs.tex_addr = wall_addr;
            cs.light    = (light + 6 > 60) ? 60 : light + 6;
            of_gpu_draw_span(&cs);
        }
    }

    /* --- Walls --- */
    for (int x = 0; x < SCREEN_W; x++) {
        float camX = 2.0f * (float)x / (float)SCREEN_W - 1.0f;
        float rdx = ca + planeX * camX;
        float rdy = sa + planeY * camX;

        int mapX = (int)player_x;
        int mapY = (int)player_y;
        float ddx = (rdx == 0.0f) ? 1e30f : fabsf(1.0f / rdx);
        float ddy = (rdy == 0.0f) ? 1e30f : fabsf(1.0f / rdy);
        int stepX, stepY;
        float side_x, side_y;
        if (rdx < 0) { stepX = -1; side_x = (player_x - mapX) * ddx; }
        else         { stepX =  1; side_x = (mapX + 1.0f - player_x) * ddx; }
        if (rdy < 0) { stepY = -1; side_y = (player_y - mapY) * ddy; }
        else         { stepY =  1; side_y = (mapY + 1.0f - player_y) * ddy; }

        int hit = 0, side = 0;
        for (int safety = 0; safety < 64 && !hit; safety++) {
            if (side_x < side_y) { side_x += ddx; mapX += stepX; side = 0; }
            else                 { side_y += ddy; mapY += stepY; side = 1; }
            if (map_solid(mapX, mapY)) hit = 1;
        }
        if (!hit) continue;

        float perp = (side == 0) ? (side_x - ddx) : (side_y - ddy);
        if (perp < 0.05f) perp = 0.05f;

        int line_h = (int)((float)SCREEN_H / perp);
        int draw_start = -line_h / 2 + SCREEN_H / 2;
        int draw_end   =  line_h / 2 + SCREEN_H / 2;
        int top_clip = 0;
        if (draw_start < 0) { top_clip = -draw_start; draw_start = 0; }
        if (draw_end > SCREEN_H) draw_end = SCREEN_H;
        int span_count = draw_end - draw_start;
        if (span_count <= 0) continue;

        /* Texture U from the exact hit position along the wall face. */
        float wallU;
        if (side == 0) wallU = player_y + perp * rdy;
        else           wallU = player_x + perp * rdx;
        wallU -= floorf(wallU);
        int texX = (int)(wallU * 64.0f);
        if (texX < 0)  texX = 0;
        if (texX > 63) texX = 63;
        if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0))
            texX = 63 - texX;

        int32_t tstep = ((int32_t)64 << 16) / line_h;
        int32_t t0    = (int32_t)top_clip * tstep;

        /* Distance fog + side shading + lightgrid sample. */
        float wx, wy;
        if (side == 0) {
            wx = (float)mapX + (stepX > 0 ? 0.0f : 1.0f);
            wy = player_y + perp * rdy;
        } else {
            wx = player_x + perp * rdx;
            wy = (float)mapY + (stepY > 0 ? 0.0f : 1.0f);
        }
        int light = (int)((perp - 0.5f) * 4.0f) - sample_light(wx, wy);
        if (side == 1) light += 6;
        if (light < 0)  light = 0;
        if (light > 63) light = 63;

        of_gpu_span_t col = {
            .fb_addr   = fb_addr + draw_start * SCREEN_W + x,
            .tex_addr  = wall_addr,
            .s         = (int32_t)texX << 16,
            .t         = t0,
            .sstep     = 0,
            .tstep     = tstep,
            .count     = span_count,
            .light     = light,
            .flags     = OF_GPU_SPAN_COLORMAP,
            .fb_stride = SCREEN_W,
            .tex_width = 64,
        };
        of_gpu_draw_span(&col);
    }

    unsigned int _t1 = of_time_us();
    of_gpu_finish();
    unsigned int _t2 = of_time_us();
    _stat_cpu_us += _t1 - _t0;
    _stat_gpu_us += _t2 - _t1;
}

/* ================================================================
 * Triangle Demo: Rotating 3D cube
 * ================================================================ */

/* Cube: 8 vertices, 12 triangles (2 per face) */
static const int8_t cube_verts[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const uint8_t cube_faces[12][3] = {
    {0,1,2},{0,2,3}, {4,6,5},{4,7,6},  /* front, back */
    {0,4,5},{0,5,1}, {2,6,7},{2,7,3},  /* bottom, top */
    {0,3,7},{0,7,4}, {1,5,6},{1,6,2},  /* left, right */
};
static const uint8_t face_colors[6] = { 0xC0, 0xA0, 0x80, 0xE0, 0x60, 0xB0 };

/* Per-face 1-texel textures — avoids race between CPU write and GPU read.
 * Heap-allocated (in SDRAM) at startup, just like the other textures. */
static uint8_t *face_tex;

static void draw_triangle_demo(int frame) {
    uint8_t *fb = of_video_surface();

    unsigned int _t0 = of_time_us();
    prepare_fb(fb, 0x08);

    /* Initialise per-face texels and flush to SDRAM before GPU draws */
    for (int i = 0; i < 6; i++)
        face_tex[i] = face_colors[i];
    OF_SVC->cache_clean_range(face_tex, 6);

    int angle_y = frame * 2;
    int angle_x = frame;
    int sy = sin_lut[angle_y & 255], cy = cos_lut[angle_y & 255];
    int sx = sin_lut[angle_x & 255], cx = cos_lut[angle_x & 255];

    /* Transform and project vertices */
    int16_t proj_x[8], proj_y[8];
    for (int i = 0; i < 8; i++) {
        int x = cube_verts[i][0] * 80;
        int y = cube_verts[i][1] * 80;
        int z = cube_verts[i][2] * 80;
        /* Rotate Y */
        int rx = (x * cy - z * sy) >> 8;
        int rz = (x * sy + z * cy) >> 8;
        /* Rotate X */
        int ry = (y * cx - rz * sx) >> 8;
        rz = (y * sx + rz * cx) >> 8;
        /* Perspective projection */
        int d = 300 + rz;
        if (d < 50) d = 50;
        proj_x[i] = (int16_t)(SCREEN_W / 2 + (rx * 200) / d);
        proj_y[i] = (int16_t)(SCREEN_H / 2 + (ry * 200) / d);
    }

    /* Draw each face as 2 triangles */
    for (int f = 0; f < 12; f++) {
        int i0 = cube_faces[f][0];
        int i1 = cube_faces[f][1];
        int i2 = cube_faces[f][2];

        /* Simple backface cull via cross product */
        int dx1 = proj_x[i1] - proj_x[i0], dy1 = proj_y[i1] - proj_y[i0];
        int dx2 = proj_x[i2] - proj_x[i0], dy2 = proj_y[i2] - proj_y[i0];
        if (dx1 * dy2 - dx2 * dy1 <= 0) continue;

        /* Bind per-face 1-texel texture (no race: each face has its own) */
        of_gpu_texture_t solid_tex = {
            .addr = (uint32_t)(uintptr_t)&face_tex[f / 2],
            .width = 1, .height = 1,
        };
        of_gpu_bind_texture(&solid_tex);

        of_gpu_vertex_t tri[3] = {
            { .x = proj_x[i0] * 16, .y = proj_y[i0] * 16, .r = 0 },
            { .x = proj_x[i1] * 16, .y = proj_y[i1] * 16, .r = 0 },
            { .x = proj_x[i2] * 16, .y = proj_y[i2] * 16, .r = 0 },
        };
        of_gpu_draw_triangles(tri, 3);
    }

    unsigned int _t1 = of_time_us();
    of_gpu_finish();
    unsigned int _t2 = of_time_us();
    _stat_cpu_us += _t1 - _t0;
    _stat_gpu_us += _t2 - _t1;
}

/* ================================================================
 * Multi-Triangle Demo: rotating fan of textured triangles drawn in a
 * single batched DRAW_TRIANGLES command via of_gpu_draw_triangles_batch().
 *
 * The fan has FAN_SLICES wedges sharing the same bound texture, so the
 * whole frame's geometry rides on one cmd-decode pass instead of N.
 * Per-vertex `r` (flat light) varies sinusoidally so adjacent slices
 * pulse at different phases — easy visual confirmation that each
 * triangle in the batch is rendered with its own attributes.
 * ================================================================ */

#define FAN_SLICES 32

static void draw_multitri_demo(int frame) {
    uint8_t *fb = of_video_surface();

    unsigned int _t0 = of_time_us();
    prepare_fb(fb, 0x10);

    of_gpu_texture_t tex = {
        .addr = (uint32_t)(uintptr_t)wall_tex,
        .width = 64, .height = 64,
    };
    of_gpu_bind_texture(&tex);

    static of_gpu_vertex_t verts[FAN_SLICES * 3];

    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2;
    int radius = 90;
    int rot = (frame * 2) & 255;

    /* Center vertex shared by every slice */
    of_gpu_vertex_t center = {
        .x = (int16_t)(cx * 16), .y = (int16_t)(cy * 16),
        .s = (int32_t)32 << 16,  .t = (int32_t)32 << 16,
        .w = 0x10000,
    };

    for (int i = 0; i < FAN_SLICES; i++) {
        int a0 = ((i       * 256) / FAN_SLICES + rot) & 255;
        int a1 = (((i + 1) * 256) / FAN_SLICES + rot) & 255;

        int x0 = cx + (cos_lut[a0] * radius) / 256;
        int y0 = cy + (sin_lut[a0] * radius) / 256;
        int x1 = cx + (cos_lut[a1] * radius) / 256;
        int y1 = cy + (sin_lut[a1] * radius) / 256;

        /* Texture sweeps a circle: rim points sit on a 28-px radius
         * around (32,32) so the rotating wheel kaleidoscopes through
         * the brick texture. */
        int32_t s0 = (int32_t)(32 + (cos_lut[a0] * 28) / 256) << 16;
        int32_t t0 = (int32_t)(32 + (sin_lut[a0] * 28) / 256) << 16;
        int32_t s1 = (int32_t)(32 + (cos_lut[a1] * 28) / 256) << 16;
        int32_t t1 = (int32_t)(32 + (sin_lut[a1] * 28) / 256) << 16;

        /* Flat light per triangle, phased by slice index + frame. */
        int s = sin_lut[(i * 8 + frame * 4) & 255];
        uint8_t light = (uint8_t)(24 - (s >> 4));   /* 24 ± 16 */

        of_gpu_vertex_t v0 = center;            v0.r = light;
        of_gpu_vertex_t v1 = {
            .x = (int16_t)(x0 * 16), .y = (int16_t)(y0 * 16),
            .s = s0, .t = t0, .w = 0x10000, .r = light,
        };
        of_gpu_vertex_t v2 = {
            .x = (int16_t)(x1 * 16), .y = (int16_t)(y1 * 16),
            .s = s1, .t = t1, .w = 0x10000, .r = light,
        };
        verts[i * 3 + 0] = v0;
        verts[i * 3 + 1] = v1;
        verts[i * 3 + 2] = v2;
    }

    /* All FAN_SLICES triangles in ONE command — the whole point of
     * the batched form. */
    of_gpu_draw_triangles_batch(verts, FAN_SLICES * 3);

    unsigned int _t1 = of_time_us();
    of_gpu_finish();
    unsigned int _t2 = of_time_us();
    _stat_cpu_us += _t1 - _t0;
    _stat_gpu_us += _t2 - _t1;
}

/* ================================================================
 * Perspective Span Demo: rotating textured triangle, software rasterised
 * with hardware perspective-correct spans (SPAN_PERSP)
 * ================================================================
 *
 * The CPU walks the triangle scanline-by-scanline; for each scanline it
 * computes (s/z, t/z, 1/z) at the left and right edges in 16.16 fixed
 * point and feeds them to the GPU as a SPAN_PERSP. The GPU then runs the
 * recip + multiply per 16-pixel sub-segment in hardware, so the resulting
 * texturing is perspective-correct (no affine warping inside polygons).
 */

typedef struct { int32_t x, y, z; int32_t s, t; } persp_vert_t;

static int32_t fdiv16(int32_t num, int32_t den) {
    /* signed 16.16 division: (num << 16) / den, clamped */
    if (den == 0) return 0;
    int64_t n = ((int64_t)num) << 16;
    return (int32_t)(n / den);
}

static void draw_persp_demo(int frame) {
    uint8_t *fb = of_video_surface();
    uint32_t fb_addr = (uint32_t)(uintptr_t)fb;

    unsigned int _t0 = of_time_us();
    prepare_fb(fb, 0x10);

    /* Three vertices of a triangle in world space, rotating about the
     * world Y axis. Texture coords (s, t) are attached at vertex setup. */
    int ang = frame & 255;
    int s_a = sin_lut[ang], c_a = cos_lut[ang];

    persp_vert_t verts[3];
    static const int16_t base[3][3] = {
        { -80, -60, 0 },
        {  80, -60, 0 },
        {   0,  80, 0 },
    };
    static const int16_t tex[3][2] = {
        {   0,   0 },
        {  64,   0 },
        {  32,  64 },
    };
    for (int i = 0; i < 3; i++) {
        /* Rotate base[i] about Y so the triangle tilts in/out of the screen */
        int x0 = base[i][0], y0 = base[i][1], z0 = base[i][2];
        int rx = (x0 * c_a - z0 * s_a) >> 8;
        int rz = (x0 * s_a + z0 * c_a) >> 8;
        verts[i].x = rx;
        verts[i].y = y0;
        verts[i].z = 200 + rz;        /* push triangle in front of camera */
        verts[i].s = (int32_t)tex[i][0] << 16;
        verts[i].t = (int32_t)tex[i][1] << 16;
    }

    /* Project to screen + compute per-vertex (s/z, t/z, 1/z) */
    int32_t sx[3], sy[3];
    int32_t sZ[3], tZ[3], oZ[3];   /* projection-space attributes (16.16) */
    for (int i = 0; i < 3; i++) {
        if (verts[i].z < 16) verts[i].z = 16;
        int32_t zi = verts[i].z;
        sx[i] = (verts[i].x * 200) / zi + (SCREEN_W / 2);
        sy[i] = (verts[i].y * 200) / zi + (SCREEN_H / 2);
        sZ[i] = fdiv16(verts[i].s, zi << 0);  /* s/z, 16.16 */
        tZ[i] = fdiv16(verts[i].t, zi << 0);
        oZ[i] = fdiv16(1 << 16,    zi);       /* 1/z, 16.16 */
    }

    /* Sort vertices by Y (top, mid, bot) */
    int top = 0, mid = 1, bot = 2;
    if (sy[mid] < sy[top]) { int t = top; top = mid; mid = t; }
    if (sy[bot] < sy[top]) { int t = top; top = bot; bot = t; }
    if (sy[bot] < sy[mid]) { int t = mid; mid = bot; bot = t; }

    int y_top = sy[top], y_mid = sy[mid], y_bot = sy[bot];
    if (y_bot <= y_top) {
        unsigned int _t1 = of_time_us();
        of_gpu_finish();
        unsigned int _t2 = of_time_us();
        _stat_cpu_us += _t1 - _t0;
        _stat_gpu_us += _t2 - _t1;
        return;
    }

    /* Walk each scanline from y_top..y_bot. For each scanline, the left
     * and right edges are interpolated linearly in screen y between the
     * appropriate pair of vertices. (s/z, t/z, 1/z) interpolate linearly
     * the same way — that's the whole point of perspective division. */
    for (int y = y_top; y <= y_bot; y++) {
        if (y < 0 || y >= SCREEN_H) continue;

        int e1_a = top, e1_b = bot;                          /* long edge */
        int e2_a, e2_b;
        if (y < y_mid) { e2_a = top; e2_b = mid; }
        else           { e2_a = mid; e2_b = bot; }

        int dy1 = sy[e1_b] - sy[e1_a];
        int dy2 = sy[e2_b] - sy[e2_a];
        if (dy1 == 0) dy1 = 1;
        if (dy2 == 0) dy2 = 1;
        int t1 = ((y - sy[e1_a]) << 16) / dy1;  /* 16.16 fraction */
        int t2 = ((y - sy[e2_a]) << 16) / dy2;

        /* Linearly interpolate edge x and projection-space attribs */
        int32_t x1 = sx[e1_a] + (((int64_t)(sx[e1_b] - sx[e1_a]) * t1) >> 16);
        int32_t x2 = sx[e2_a] + (((int64_t)(sx[e2_b] - sx[e2_a]) * t2) >> 16);
        int32_t sZ1 = sZ[e1_a] + (int32_t)(((int64_t)(sZ[e1_b] - sZ[e1_a]) * t1) >> 16);
        int32_t sZ2 = sZ[e2_a] + (int32_t)(((int64_t)(sZ[e2_b] - sZ[e2_a]) * t2) >> 16);
        int32_t tZ1 = tZ[e1_a] + (int32_t)(((int64_t)(tZ[e1_b] - tZ[e1_a]) * t1) >> 16);
        int32_t tZ2 = tZ[e2_a] + (int32_t)(((int64_t)(tZ[e2_b] - tZ[e2_a]) * t2) >> 16);
        int32_t oZ1 = oZ[e1_a] + (int32_t)(((int64_t)(oZ[e1_b] - oZ[e1_a]) * t1) >> 16);
        int32_t oZ2 = oZ[e2_a] + (int32_t)(((int64_t)(oZ[e2_b] - oZ[e2_a]) * t2) >> 16);

        int32_t xl = x1 < x2 ? x1 : x2;
        int32_t xr = x1 < x2 ? x2 : x1;
        if (x2 < x1) {
            int32_t tmp;
            tmp = sZ1; sZ1 = sZ2; sZ2 = tmp;
            tmp = tZ1; tZ1 = tZ2; tZ2 = tmp;
            tmp = oZ1; oZ1 = oZ2; oZ2 = tmp;
        }

        if (xl < 0) xl = 0;
        if (xr >= SCREEN_W) xr = SCREEN_W - 1;
        int count = xr - xl + 1;
        if (count <= 0) continue;

        int32_t inv_count = (1 << 16) / count;  /* 16.16 of 1/count */
        of_gpu_span_t span = {
            .fb_addr     = fb_addr + y * SCREEN_W + xl,
            .tex_addr    = (uint32_t)(uintptr_t)persp_tex,
            .s           = 0,                              /* unused (PERSP) */
            .t           = 0,
            .sstep       = 0,
            .tstep       = 0,
            .count       = count,
            .light       = 0,
            .flags       = OF_GPU_SPAN_COLORMAP | OF_GPU_SPAN_PERSP,
            .fb_stride   = 1,
            .tex_width   = 64,
            .sdivz       = sZ1,
            .tdivz       = tZ1,
            .zi_persp    = oZ1,
            .sdivz_step  = (int32_t)(((int64_t)(sZ2 - sZ1) * inv_count) >> 16),
            .tdivz_step  = (int32_t)(((int64_t)(tZ2 - tZ1) * inv_count) >> 16),
            .zi_step     = (int32_t)(((int64_t)(oZ2 - oZ1) * inv_count) >> 16),
        };
        of_gpu_draw_span(&span);
    }

    unsigned int _t1 = of_time_us();
    of_gpu_finish();
    unsigned int _t2 = of_time_us();
    _stat_cpu_us += _t1 - _t0;
    _stat_gpu_us += _t2 - _t1;
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void) {
    of_video_init();
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    set_palette();
    printf("[gpudemo] start\n");

    /* Allocate textures from the heap (SDRAM, reachable by both CPU and GPU). */
    checkerboard_tex = malloc(64 * 64);
    wall_tex         = malloc(64 * 64);
    sprite_tex       = malloc(16 * 16);
    persp_tex        = malloc(64 * 64);
    face_tex         = malloc(8);  /* 6 bytes rounded up for alignment */
    printf("[gpudemo] malloc: cb=%p wall=%p sprite=%p persp=%p face=%p\n",
           checkerboard_tex, wall_tex, sprite_tex, persp_tex, face_tex);
    if (!checkerboard_tex || !wall_tex || !sprite_tex || !persp_tex || !face_tex) {
        printf("[gpudemo] FATAL: out of heap for textures\n");
        return 1;
    }

    build_sin_table();
    build_colormap();
    build_checkerboard();
    build_wall_texture();
    build_sprite_texture();
    build_persp_texture();
    build_light_grid();
    printf("[gpudemo] textures + lightgrid built\n");

    /* Flush texture data from CPU D-cache to SDRAM so the GPU can read it */
    OF_SVC->cache_clean_range(checkerboard_tex, 64 * 64);
    OF_SVC->cache_clean_range(wall_tex,         64 * 64);
    OF_SVC->cache_clean_range(sprite_tex,       16 * 16);
    OF_SVC->cache_clean_range(persp_tex,        64 * 64);
    OF_SVC->cache_clean_range(face_tex,         8);
    printf("[gpudemo] cache_clean done\n");

    /* The stock of_gpu_init() only pulses ring_reset (GPU_CTRL=4). On real
     * hardware the GPU FSM may have been processing garbage from boot-time
     * ring BRAM, so we also pulse soft_reset (bit1) here to force the FSM
     * back to S_IDLE before init clears the ring. */
    GPU_CTRL = 6;  /* soft_reset | ring_reset */
    {
        volatile int i;
        for (i = 0; i < 100; i++) ;
    }
    of_gpu_init();
    printf("[gpudemo] gpu_init ok\n");

    of_gpu_palookup_upload(0, colormap, sizeof(colormap));
    printf("[gpudemo] colormap uploaded\n");

    {
        uint8_t *fb0 = of_video_surface();
        printf("[gpudemo] surface[0] = %p\n", fb0);
    }

    printf("[gpudemo] entering main loop — A = cycle mode\n");

    /* Mode 0 — Auto-walking raycaster maze (textured spans + colormap)
     * Mode 1 — Perspective-correct textured triangle (SPAN_PERSP)
     * Mode 2 — Rotating 3D cube (per-triangle DRAW_TRIANGLES)
     * Mode 3 — Pinwheel of 32 triangles in one batched DRAW_TRIANGLES
     *          command (of_gpu_draw_triangles_batch) */
    int mode = 0;
    int frame = 0;
    int auto_switch_at = 600;  /* swap modes every ~10s */

    /* CPU% vs GPU% is computed from _stat_cpu_us / _stat_gpu_us, which
     * the draw functions update internally. No timing or finish calls
     * here — the original pipeline ordering (finish inside draw, then
     * flip) is preserved, which is what the GPU was happy with. */
    unsigned int fps_last_ms = of_time_ms();
    int fps_frames = 0;

    while (1) {
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) {
            mode = (mode + 1) % 4;
            printf("[gpudemo] mode -> %d\n", mode);
        }

        switch (mode) {
            case 0: draw_maze_demo(frame);     break;
            case 1: draw_persp_demo(frame);    break;
            case 2: draw_triangle_demo(frame); break;
            case 3: draw_multitri_demo(frame); break;
        }
        of_video_flip();
        fps_frames++;

        unsigned int now_ms = of_time_ms();
        unsigned int dt_ms  = now_ms - fps_last_ms;
        if (dt_ms >= 1000) {
            unsigned int fps_x10 = (fps_frames * 10000u) / dt_ms;
            unsigned int total   = _stat_cpu_us + _stat_gpu_us;
            if (total == 0) total = 1;
            unsigned int cpu_pct = (_stat_cpu_us * 100u) / total;
            unsigned int gpu_pct = 100u - cpu_pct;
            printf("[gpudemo] fps=%u.%u cpu=%u%% gpu=%u%% mode=%d\n",
                   fps_x10 / 10, fps_x10 % 10, cpu_pct, gpu_pct, mode);
            fps_last_ms  = now_ms;
            fps_frames   = 0;
            _stat_cpu_us = 0;
            _stat_gpu_us = 0;
        }

        frame++;
    }
}
