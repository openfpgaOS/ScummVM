#include "test.h"

void test_accel(void) {
    section_start("Accel Mem");

    /* memset: small */
    {
        uint8_t buf[64];
        for (int i = 0; i < 64; i++) buf[i] = 0;
        memset(buf, 0xAB, 32);
        int ok = 1;
        for (int i = 0; i < 32; i++) if (buf[i] != 0xAB) ok = 0;
        for (int i = 32; i < 64; i++) if (buf[i] != 0x00) ok = 0;
        ASSERT("memset 32B", ok);
    }

    /* memset: large (uses heap to avoid .bss issues) */
    {
        uint8_t *big = malloc(4096);
        ASSERT("memset alloc", big != NULL);
        if (big) {
            memset(big, 0x55, 4096);
            ASSERT("memset 4K", big[0] == 0x55 && big[2047] == 0x55 && big[4095] == 0x55);
            memset(big, 0, 4096);
            ASSERT("memset zero", big[0] == 0 && big[4095] == 0);
            free(big);
        }
    }

    /* memset: unaligned */
    {
        uint8_t buf[80];
        for (int i = 0; i < 80; i++) buf[i] = 0;
        memset(buf + 1, 0xCC, 63);
        ASSERT("memset unalign", buf[0] == 0 && buf[1] == 0xCC && buf[63] == 0xCC && buf[64] == 0);
    }

    /* memcpy: basic */
    {
        uint8_t src[64], dst[64];
        for (int i = 0; i < 64; i++) src[i] = (uint8_t)i;
        for (int i = 0; i < 64; i++) dst[i] = 0;
        memcpy(dst, src, 64);
        ASSERT("memcpy 64B", memcmp(src, dst, 64) == 0);
    }

    /* memcpy: large */
    {
        uint8_t *src = malloc(4096);
        uint8_t *dst = malloc(4096);
        if (src && dst) {
            for (int i = 0; i < 4096; i++) src[i] = (uint8_t)(i ^ (i >> 8));
            memcpy(dst, src, 4096);
            ASSERT("memcpy 4K", memcmp(src, dst, 4096) == 0);
        }
        if (src) free(src);
        if (dst) free(dst);
    }

    /* memcpy: unaligned src and dst */
    {
        uint8_t buf[80], dst[80];
        for (int i = 0; i < 80; i++) buf[i] = (uint8_t)i;
        for (int i = 0; i < 80; i++) dst[i] = 0;
        memcpy(dst + 3, buf + 1, 50);
        ASSERT("memcpy unalign", dst[3] == 1 && dst[52] == 50 && dst[2] == 0 && dst[53] == 0);
    }

    /* memmove: non-overlapping */
    {
        uint8_t buf[64];
        for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i + 1);
        memmove(buf + 32, buf, 32);
        ASSERT("memmove fwd", buf[32] == 1 && buf[63] == 32);
    }

    /* memmove: overlapping forward (dst > src) */
    {
        uint8_t buf[64];
        for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
        memmove(buf + 4, buf, 32);
        ASSERT("memmove olap+", buf[4] == 0 && buf[35] == 31);
    }

    /* memmove: overlapping backward (dst < src) */
    {
        uint8_t buf[64];
        for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
        memmove(buf, buf + 4, 32);
        ASSERT("memmove olap-", buf[0] == 4 && buf[31] == 35);
    }

    /* memcmp */
    {
        uint8_t a[64], b[64];
        for (int i = 0; i < 64; i++) a[i] = b[i] = (uint8_t)i;
        ASSERT("memcmp eq", memcmp(a, b, 64) == 0);
        b[63] = 0xFF;
        ASSERT("memcmp tail", memcmp(a, b, 64) < 0);
        b[63] = a[63]; b[0] = 0xFF;
        ASSERT("memcmp head", memcmp(a, b, 64) < 0);
        ASSERT("memcmp 0", memcmp(a, b, 0) == 0);
    }

    section_end();
}

void test_memset_stack(void) {
    section_start("Memset Stack");

    char buf[2048];

    memset(buf, 0, sizeof(buf));
    ASSERT("zero[0]", buf[0] == 0);
    ASSERT("zero[2047]", buf[2047] == 0);

    memset(buf, 0xAA, sizeof(buf));
    ASSERT("fill[0]", (unsigned char)buf[0] == 0xAA);
    ASSERT("fill[1023]", (unsigned char)buf[1023] == 0xAA);
    ASSERT("fill[2047]", (unsigned char)buf[2047] == 0xAA);

    section_end();
}
