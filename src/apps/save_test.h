/*
 * save_test.h -- shared save cross-pollution test logic
 *
 * Used by savea/ and saveb/. Each app defines APP_ID, APP_NAME, STEP and
 * (optionally) VSAVE_SIZE_EVEN, then #includes this header and calls
 * save_test_main() from main(). Running the apps alternately exercises
 * save isolation between distinct app IDs.
 */

#ifndef OF_SAVE_TEST_H
#define OF_SAVE_TEST_H

#include "of.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#ifndef APP_ID
#error "APP_ID must be defined before including save_test.h"
#endif
#ifndef APP_NAME
#error "APP_NAME must be defined before including save_test.h"
#endif
#ifndef STEP
#error "STEP must be defined before including save_test.h"
#endif

#ifndef NUM_VSAVES
#define NUM_VSAVES 10
#endif
#ifndef VSAVE_SIZE_EVEN
#define VSAVE_SIZE_EVEN 16384
#endif
#ifndef VSAVE_SIZE_ODD
#define VSAVE_SIZE_ODD 8192
#endif

#define VSAVE_SIZE      VSAVE_SIZE_EVEN  /* max, for buffer sizing */
#define SLOT_SIZE       VSAVE_SIZE_EVEN  /* one vsave per slot */
#define MAGIC           0x5356           /* "SV" */
#define PAYLOAD_OFFSET  16

/* Each virtual save layout:
 *   [0..1]   magic (0x5356)
 *   [2]      app_id
 *   [3]      slot_index
 *   [4..7]   iteration counter (increments each rotation)
 *   [8..11]  CRC32 of bytes [PAYLOAD_OFFSET..size-1]
 *   [12..15] reserved (0)
 *   [16..size-1]  random data seeded from (app_id, slot_index, iteration)
 */
typedef struct {
    uint16_t magic;
    uint8_t  app_id;
    uint8_t  slot_index;
    uint32_t iteration;
    uint32_t crc;
    uint32_t reserved;
} vsave_header_t;

static int save_read(int slot, void *buf, uint32_t offset, uint32_t len) {
    char path[32];
    snprintf(path, sizeof(path), "%s_%d.sav", APP_NAME, slot);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (offset && fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
    int n = (int)fread(buf, 1, len, f);
    fclose(f);
    return n;
}

static int save_write(int slot, const void *buf, uint32_t offset, uint32_t len) {
    char path[32];
    snprintf(path, sizeof(path), "%s_%d.sav", APP_NAME, slot);
    FILE *f = fopen(path, "r+b");
    if (!f) f = fopen(path, "wb");
    if (!f) return -1;
    if (offset && fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
    int n = (int)fwrite(buf, 1, len, f);
    fclose(f);
    return n;
}

static uint32_t vsave_data_size(int idx) {
    return (idx & 1) ? VSAVE_SIZE_ODD : VSAVE_SIZE_EVEN;
}
#define PAYLOAD_SIZE(sz) ((sz) - PAYLOAD_OFFSET)

/* Simple CRC32 (no table, small code) */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/* Deterministic PRNG seeded from app_id + slot + iteration */
static uint32_t xorshift_state;
static void seed_rng(uint8_t app_id, uint8_t slot, uint32_t iter) {
    xorshift_state = ((uint32_t)app_id << 24) ^ ((uint32_t)slot << 16) ^ iter ^ 0x12345678;
    if (xorshift_state == 0) xorshift_state = 1;
}
static uint32_t xorshift32(void) {
    uint32_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift_state = x;
    return x;
}

/* Map virtual save index to (slot, offset). One vsave per slot in current
 * configuration; the math generalizes if SLOT_SIZE > VSAVE_SIZE. */
static void vsave_location(int vsave_idx, int *slot, uint32_t *offset) {
    int vsaves_per_slot = SLOT_SIZE / VSAVE_SIZE;
    if (vsaves_per_slot < 1) vsaves_per_slot = 1;
    *slot = vsave_idx / vsaves_per_slot;
    *offset = (vsave_idx % vsaves_per_slot) * VSAVE_SIZE;
}

static uint8_t buf[VSAVE_SIZE];

static void generate_payload(uint8_t app_id, uint8_t slot_index,
                             uint32_t iteration, uint32_t payload_size) {
    seed_rng(app_id, slot_index, iteration);
    for (uint32_t i = 0; i < payload_size; i += 4) {
        uint32_t r = xorshift32();
        uint32_t remaining = payload_size - i;
        uint32_t take = (remaining >= 4) ? 4 : remaining;
        for (uint32_t j = 0; j < take; j++)
            buf[PAYLOAD_OFFSET + i + j] = (uint8_t)(r >> (j * 8));
    }
}

static int write_vsave(int vsave_idx, uint32_t iteration) {
    int slot;
    uint32_t offset;
    vsave_location(vsave_idx, &slot, &offset);

    uint32_t sz  = vsave_data_size(vsave_idx);
    uint32_t psz = PAYLOAD_SIZE(sz);

    vsave_header_t *hdr = (vsave_header_t *)buf;
    hdr->magic      = MAGIC;
    hdr->app_id     = APP_ID;
    hdr->slot_index = (uint8_t)vsave_idx;
    hdr->iteration  = iteration;
    hdr->reserved   = 0;

    generate_payload(APP_ID, (uint8_t)vsave_idx, iteration, psz);
    hdr->crc = crc32(&buf[PAYLOAD_OFFSET], psz);

    int rc = save_write(slot, buf, offset, sz);
    return (rc == (int)sz) ? 0 : -1;
}

static int verify_vsave(int vsave_idx, uint32_t *out_iteration) {
    int slot;
    uint32_t offset;
    vsave_location(vsave_idx, &slot, &offset);

    uint32_t sz  = vsave_data_size(vsave_idx);
    uint32_t psz = PAYLOAD_SIZE(sz);

    int rc = save_read(slot, buf, offset, sz);
    if (rc != (int)sz) return -1;

    vsave_header_t *hdr = (vsave_header_t *)buf;
    if (hdr->magic != MAGIC) return -2;
    if (hdr->app_id != APP_ID) return -3;
    if (hdr->slot_index != (uint8_t)vsave_idx) return -4;

    uint32_t stored_crc = hdr->crc;
    uint32_t actual_crc = crc32(&buf[PAYLOAD_OFFSET], psz);
    if (actual_crc != stored_crc) return -5;

    uint32_t iter = hdr->iteration;
    seed_rng(APP_ID, (uint8_t)vsave_idx, iter);
    for (uint32_t i = 0; i < psz; i += 4) {
        uint32_t r = xorshift32();
        uint32_t remaining = psz - i;
        uint32_t check = (remaining >= 4) ? 4 : remaining;
        for (uint32_t j = 0; j < check; j++) {
            if (buf[PAYLOAD_OFFSET + i + j] != (uint8_t)(r >> (j * 8)))
                return -6;
        }
    }

    if (out_iteration) *out_iteration = iter;
    return 0;
}

static int is_virgin(void) {
    uint8_t hdr[4];
    save_read(0, hdr, 0, 4);
    uint16_t magic = hdr[0] | (hdr[1] << 8);
    uint8_t app_id = hdr[2];
    return (magic != MAGIC || app_id != APP_ID);
}

static int save_test_main(void) {
    printf("\033[2J\033[H");
    printf("\033[93m  %s Save Test (step=%d)\033[0m\n\n", APP_NAME, STEP);
    printf("  Save sizes: even=%dB odd=%dB\n",
           (int)VSAVE_SIZE_EVEN, (int)VSAVE_SIZE_ODD);
    printf("  virgin=%d\n\n", is_virgin());

    if (is_virgin()) {
        printf("  First run -- initializing %d virtual saves...\n", NUM_VSAVES);
        for (int i = 0; i < NUM_VSAVES; i++) {
            int rc = write_vsave(i, 0);
            if (rc < 0) {
                printf("  \033[91mFAIL\033[0m write vsave %d (rc=%d)\n", i, rc);
                goto done;
            }
            uint32_t iter;
            int vrc = verify_vsave(i, &iter);
            uint32_t sz = vsave_data_size(i);
            if (vrc < 0)
                printf("  [%d] %dB write ok, \033[91mreadback FAIL rc=%d\033[0m\n",
                       i, (int)sz, vrc);
            else
                printf("  [%d] %dB ok\n", i, (int)sz);
        }
        printf("\n  \033[92mInitialized. Run again to test rotation.\033[0m\n");
        goto done;
    }

    printf("  Verifying all %d virtual saves...\n", NUM_VSAVES);

    uint32_t iterations[NUM_VSAVES];
    int all_ok = 1;
    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = verify_vsave(i, &iterations[i]);
        if (rc < 0) {
            all_ok = 0;
            const char *reason;
            switch (rc) {
                case -2: reason = "bad magic"; break;
                case -3: reason = "WRONG APP_ID (cross-pollution!)"; break;
                case -4: reason = "wrong slot_index"; break;
                case -5: reason = "CRC MISMATCH (data corruption!)"; break;
                case -6: reason = "PAYLOAD MISMATCH (bit rot!)"; break;
                default: reason = "read error"; break;
            }
            printf("  \033[91mFAIL\033[0m vsave %d (%dB): %s\n",
                   i, (int)vsave_data_size(i), reason);
        } else {
            printf("  [%d] %dB ok (iter=%d)\n",
                   i, (int)vsave_data_size(i), (int)iterations[i]);
        }
    }

    if (!all_ok) {
        printf("\n  \033[91mVERIFICATION FAILED\033[0m\n");
        goto done;
    }

    printf("\n  Rotating by step=%d...\n", STEP);

    uint32_t new_iters[NUM_VSAVES];
    for (int i = 0; i < NUM_VSAVES; i++) {
        int src = ((i - STEP) % NUM_VSAVES + NUM_VSAVES) % NUM_VSAVES;
        new_iters[i] = iterations[src] + 1;
    }

    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = write_vsave(i, new_iters[i]);
        if (rc < 0) {
            printf("  \033[91mFAIL\033[0m write vsave %d\n", i);
            goto done;
        }
    }

    int verify_ok = 1;
    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = verify_vsave(i, NULL);
        if (rc < 0) {
            verify_ok = 0;
            printf("  \033[91mFAIL\033[0m post-rotate verify vsave %d (rc=%d)\n", i, rc);
        }
    }

    if (verify_ok)
        printf("\n  \033[92mPASS\033[0m -- all %d vsaves rotated and verified\n", NUM_VSAVES);
    else
        printf("\n  \033[91mFAIL\033[0m -- post-rotate verification failed\n");

done:
    while (1)
        usleep(100 * 1000);
    return 0;
}

#endif /* OF_SAVE_TEST_H */
