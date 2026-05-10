/*
 * of_smp_bank.c -- .ofsf runtime bank reader
 *
 * The kernel auto-loads the first .ofsf it finds in a data slot at boot
 * and hands the buffer to apps via OF_SVC->smp_bank_preload_base. This
 * file copies the metadata (header + presets + zones) into SDRAM so the
 * CPU never reads CRAM1 during playback — only the mixer DMA touches
 * CRAM1.  Without this copy, CPU zone-reads and mixer sample-DMA
 * contend on the CRAM1 bus arbiter and eventually deadlock.
 */

#include "include/of_smp_bank.h"
#include "include/of_services.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static ofsf_header_t        hdr_copy;
static ofsf_preset_t       *preset_copy;
static ofsf_zone_t         *zone_copy;
static const ofsf_header_t *loaded_header;
static const ofsf_preset_t *loaded_presets;
static const ofsf_zone_t   *loaded_zones;
static const void           *sample_base;

__attribute__((constructor(102)))
static void bank_autobind(void)
{
    const void *buf = OF_SVC->smp_bank_preload_base;
    if (!buf) return;

    const ofsf_header_t *hdr = (const ofsf_header_t *)buf;
    if (hdr->magic != OFSF_MAGIC || hdr->version != OFSF_VERSION)
        return;

    const uint8_t *base = (const uint8_t *)buf;

    memcpy(&hdr_copy, hdr, sizeof(hdr_copy));

    uint32_t preset_bytes = OFSF_PRESET_COUNT * sizeof(ofsf_preset_t);
    preset_copy = (ofsf_preset_t *)malloc(preset_bytes);
    if (preset_copy)
        memcpy(preset_copy, base + sizeof(ofsf_header_t), preset_bytes);

    uint32_t zone_bytes = hdr->zone_count * sizeof(ofsf_zone_t);
    zone_copy = (ofsf_zone_t *)malloc(zone_bytes);
    if (zone_copy)
        memcpy(zone_copy, base + sizeof(ofsf_header_t) + preset_bytes, zone_bytes);

    loaded_header  = &hdr_copy;
    loaded_presets = preset_copy;
    loaded_zones   = zone_copy;
    sample_base    = base + hdr->sample_data_offset;
}

const ofsf_header_t *of_smp_bank_get(void)
{
    return loaded_header;
}

const void *of_smp_bank_sample_base(void)
{
    return sample_base;
}

int of_smp_zone_lookup(int bank, int program, int key, int velocity,
                       const ofsf_zone_t **zones_out, int max_zones)
{
    if (!loaded_header || !loaded_presets || !loaded_zones)
        return 0;
    if (program < 0 || program > 127)
        return 0;

    /* Preset index: bank 0 = slots 0..127, bank 128 (drums) = slots 128..255 */
    int idx;
    if (bank == 128)
        idx = 128 + program;
    else
        idx = program; /* bank 0 only; other banks not yet supported */

    if (idx < 0 || idx >= OFSF_PRESET_COUNT)
        return 0;

    const ofsf_preset_t *pr = &loaded_presets[idx];
    if (pr->zone_count == 0)
        return 0;

    int found = 0;
    for (int i = 0; i < pr->zone_count && found < max_zones; i++) {
        int zi = pr->zone_start + i;
        if ((uint32_t)zi >= loaded_header->zone_count)
            break;
        const ofsf_zone_t *z = &loaded_zones[zi];
        if (key >= z->key_lo && key <= z->key_hi &&
            velocity >= z->vel_lo && velocity <= z->vel_hi) {
            zones_out[found++] = z;
        }
    }
    return found;
}
