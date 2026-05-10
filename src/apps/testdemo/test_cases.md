# openfpgaOS Test Cases

Comprehensive positive, negative, and edge case tests for all SDK functions.

---

## Video (`of_video_*`)

### of_video_init()
- **Positive**: Call once → display mode switches to framebuffer, no crash
- **Positive**: Get surface after init → returns non-NULL pointer in SDRAM
- **Edge**: Call twice → should be idempotent, no double-init crash

### of_video_flip()
- **Positive**: Write pixels to surface, flip → pixels visible on next frame
- **Positive**: Flip without writing → displays previous buffer (double-buffered)
- **Edge**: Flip immediately after flip → waits for vsync, no corruption
- **Edge**: Flip 1000 times in tight loop → no hang, maintains ~60fps

### of_video_palette(index, r, g, b)
- **Positive**: Set palette 0 to red (0xFF0000) → pixel 0 shows red
- **Positive**: Set all 256 entries → no crash, all colors correct
- **Negative**: Index 255 → valid (last entry)
- **Edge**: Set same index twice → second value wins

### of_video_palette_bulk(palette, count)
- **Positive**: Load 256-entry palette array → all colors set
- **Edge**: count=0 → no-op
- **Edge**: count=1 → only first entry set

### of_video_clear(color)
- **Positive**: Clear to index 0 → entire framebuffer filled
- **Positive**: Clear to index 255 → entire framebuffer filled with 255

### of_video_get_surface()
- **Positive**: Returns pointer to current draw buffer
- **Positive**: After flip, returns different buffer than before
- **Edge**: Call before init → undefined (should not crash)

### of_video_set_display_mode(mode)
- **Positive**: Mode 0 (terminal) → terminal overlay visible
- **Positive**: Mode 1 (framebuffer) → framebuffer visible
- **Negative**: Mode 99 → should not crash

---

## Input (`of_input_*`)

### of_input_poll()
- **Positive**: Call once → updates button state
- **Positive**: Call in loop → tracks button changes frame-by-frame
- **Edge**: Call before any button pressed → all zeros

### of_btn(mask)
- **Positive**: Press A → of_btn(OF_BTN_A) returns non-zero
- **Positive**: Release A → of_btn(OF_BTN_A) returns 0
- **Negative**: Mask 0 → returns 0
- **Edge**: Multiple buttons pressed → OR mask works

### of_btn_pressed(mask)
- **Positive**: Press A this frame → returns non-zero (edge detect)
- **Positive**: Hold A next frame → returns 0 (only on press edge)
- **Edge**: Press and release within one poll → may miss the press

### of_btn_released(mask)
- **Positive**: Release A this frame → returns non-zero
- **Positive**: A already released → returns 0

### of_input_state(player, state)
- **Positive**: Player 0 → fills state with P1 data
- **Positive**: Player 1 → fills state with P2 data
- **Negative**: Player 2 → should not crash
- **Positive**: state->joy_lx/ly → analog stick values (-32768 to 32767)

### of_input_set_deadzone(deadzone)
- **Positive**: Set to 4000 → small stick movements ignored
- **Edge**: Set to 0 → no deadzone, raw values
- **Edge**: Set to 32767 → everything filtered

---

## Audio (`of_audio_*`)

### of_audio_write(samples, count)
- **Positive**: Write 512 stereo pairs → audio output
- **Positive**: Write 1 pair → single sample
- **Negative**: count=0 → returns 0
- **Edge**: Write more than FIFO space → returns partial count

### of_audio_get_free()
- **Positive**: After init, returns full FIFO capacity
- **Positive**: After writing, returns reduced capacity
- **Edge**: FIFO full → returns 0

### of_audio_init()
- **Positive**: Initializes audio subsystem
- **Edge**: Call twice → idempotent

---

## Timer (`of_time_*`, `of_delay_*`)

### of_time_us()
- **Positive**: Returns increasing values on successive calls
- **Positive**: Difference between calls ≈ actual elapsed time
- **Edge**: Wraps at ~4295 seconds (uint32_t)

### of_time_ms()
- **Positive**: Returns increasing values
- **Positive**: 1000× slower than of_time_us()
- **Edge**: Wraps at ~49 days

### of_delay_us(us)
- **Positive**: of_delay_us(1000) → ~1ms elapsed
- **Positive**: of_delay_us(0) → returns immediately
- **Edge**: of_delay_us(1) → minimum delay (~10ns resolution)

### of_delay_ms(ms)
- **Positive**: of_delay_ms(100) → ~100ms elapsed
- **Positive**: of_delay_ms(0) → returns immediately
- **Edge**: of_delay_ms(1) → ~1ms elapsed

---

## Save (`of_save_*`, `fopen("save_N")`)

### fopen("save_0", "wb") / fwrite / fclose
- **Positive**: Write 32 bytes → fclose flushes to SD
- **Positive**: Write 256KB → fills entire slot
- **Edge**: Write 0 bytes then close → no flush (write_max=0)
- **Edge**: Write 1 byte → flushes 1 byte to SD

### fopen("save_0", "rb") / fread
- **Positive**: Read back written data → matches
- **Positive**: Read 4 bytes → returns 4
- **Edge**: Read from empty/new save → returns whatever is in CRAM1

### fopen("save_9", "wb")
- **Positive**: Last save slot works
- **Negative**: fopen("save_10") → returns NULL (only 0-9)
- **Negative**: fopen("save_-1") → returns NULL

### of_save_read(slot, buf, offset, len)
- **Positive**: Read 32 bytes at offset 0 → returns 32
- **Positive**: Read at offset 1000 → returns data from that offset
- **Negative**: slot=-1 → returns -1
- **Negative**: slot=10 → returns -1
- **Edge**: offset=0x3FFFC, len=4 → reads last 4 bytes of 256KB slot
- **Edge**: offset=0x40000, len=1 → out of bounds, returns -1

### of_save_write(slot, buf, offset, len)
- **Positive**: Write 32 bytes → returns 32
- **Negative**: slot=10 → returns -1
- **Edge**: Write at offset 0x40000-4, len=4 → writes last 4 bytes
- **Edge**: Write at offset 0x40000 → out of bounds, returns -1

### of_save_flush(slot)
- **Positive**: Flush after write → data persists across reboot
- **Negative**: slot=-1 → no crash

### of_save_flush_size(slot, size)
- **Positive**: Flush with size=32 → only 32 bytes written to SD
- **Positive**: Flush with size=0x40000 → full slot
- **Edge**: size=0 → no flush

### of_save_erase(slot)
- **Positive**: Erase → all bytes become 0xFF
- **Positive**: Read after erase → 0xFF
- **Negative**: slot=10 → no crash

---

## File I/O (`fopen`, `fread`, `fseek`, `ftell`, `fclose`)

### fopen("slot:N", "rb")
- **Positive**: fopen("slot:1") → returns valid FILE*
- **Positive**: fopen("slot:2") → returns valid FILE*
- **Negative**: fopen("slot:99") → returns FILE* but reads fail
- **Edge**: Open same slot twice → two independent FILE* with separate offsets

### fopen("filename", "rb")
- **Positive**: After of_file_slot_register(3, "game.dat"), fopen("game.dat") works
- **Negative**: fopen("nonexistent.xyz") → returns NULL
- **Negative**: fopen("") → returns NULL
- **Negative**: fopen("/path/file") → returns NULL (no filesystem paths)
- **Edge**: Case-insensitive: register "Game.DAT", open "game.dat" → works

### fread(buf, size, count, f)
- **Positive**: fread 4 bytes → returns 4, buffer has file data
- **Positive**: fread 64KB → returns 64KB
- **Positive**: fread 1 byte at a time in loop → sequential data
- **Edge**: fread after EOF → returns 0
- **Edge**: size=0 → returns 0
- **Edge**: count=0 → returns 0
- **Negative**: fread on NULL file → undefined (crash OK)

### fseek(f, offset, whence)
- **Positive**: SEEK_SET 0 → back to start
- **Positive**: SEEK_CUR +10 → advance 10 bytes
- **Positive**: SEEK_END 0 → go to end (if size known)
- **Edge**: SEEK_SET to same position → no-op
- **Negative**: SEEK_SET -1 → returns -1

### ftell(f)
- **Positive**: After fopen → returns 0
- **Positive**: After fread 64 bytes → returns 64
- **Positive**: After fseek(f, 0, SEEK_SET) → returns 0
- **Edge**: After seek past file size → returns that position

### fclose(f)
- **Positive**: Close after read → fd freed, no leak
- **Positive**: Close save file after write → auto-flushes
- **Edge**: Close then fopen → reuses fd
- **Negative**: fclose(NULL) → should not crash

### of_file_slot_register(slot_id, filename)
- **Positive**: Register slot 3 as "game.dat" → fopen("game.dat") works
- **Positive**: Register multiple slots → all accessible
- **Edge**: Register same slot twice with different name → last wins
- **Edge**: Register 16 slots (MAX_FILE_SLOTS) → all work
- **Edge**: Register 17th slot → silently ignored

### of_file_read(slot_id, offset, dest, length)
- **Positive**: Read 64 bytes from slot 1 → returns 0, data valid
- **Positive**: Read 512KB → returns 0
- **Negative**: dest outside SDRAM → returns OF_ERR_PARAM
- **Edge**: length=0 → returns 0 (no DMA)
- **Edge**: Read at large offset → returns bridge error if past EOF

### of_file_size(slot_id)
- **Positive**: Slot 1 (os.bin) → returns ~119KB
- **Positive**: Slot 2 (app.elf) → returns app size
- **Negative**: Slot 99 → returns negative (timeout)

---

## POSIX Save I/O (`fopen("save_N")`)

### fopen("save_0", "wb")
- **Positive**: Open for write → returns FILE*
- **Positive**: fwrite data → write_max tracks high water mark
- **Positive**: fclose → calls of_save_flush_size with write_max

### fopen("save_0", "rb")
- **Positive**: Open for read → returns FILE*
- **Positive**: fread → reads from CRAM1

### fopen("save_0", "r+b")
- **Positive**: Open for read+write → returns FILE*
- **Edge**: Read then write at same offset → both work

### Sequential save workflow
- **Positive**: Open wb → write 100 bytes → close → open rb → read 100 bytes → matches
- **Positive**: Open wb → write 1000 bytes → seek to 0 → write 4 bytes → close → flush size = 1000 (high water mark)

---

## Mixer (`of_mixer_*`)

### of_mixer_init(sample_rate, channels)
- **Positive**: Init with 11025, 8 → mixer ready
- **Edge**: Init twice → reinitializes

### of_mixer_play(data, len, rate, loop, vol)
- **Positive**: Play WAV data → audio output
- **Positive**: loop=1 → loops continuously
- **Positive**: vol=128 → half volume
- **Negative**: data=NULL → returns -1
- **Edge**: len=0 → returns -1

### of_mixer_stop(voice)
- **Positive**: Stop playing voice → silence
- **Negative**: voice=-1 → no crash

### of_mixer_stop_all()
- **Positive**: Stop all → all voices silent
- **Edge**: Call when nothing playing → no-op

### of_mixer_set_volume(voice, vol)
- **Positive**: Set vol=255 → full volume
- **Positive**: Set vol=0 → silence
- **Edge**: Set vol=128 → half volume

---

## Link Cable (`of_link_*`)

### of_link_send(data)
- **Positive**: Send 32-bit value → returns 0
- **Edge**: Send 0x00000000 → valid
- **Edge**: Send 0xFFFFFFFF → valid

### of_link_recv(data)
- **Positive**: Receive when data available → returns 0, *data filled
- **Negative**: Receive when no data → returns -1

### of_link_get_status()
- **Positive**: Returns status word
- **Edge**: No cable connected → returns disconnected status

---

## Terminal (`of_term_*`, `printf`)

### printf(fmt, ...)
- **Positive**: printf("hello") → text on terminal
- **Positive**: printf("%d", 42) → "42" on terminal
- **Positive**: printf("%08X", 0xDEAD) → "0000DEAD"
- **Edge**: printf("") → no output
- **Edge**: Long string (>40 chars) → wraps to next line

### of_term_clear()
- **Positive**: Clear → all characters removed
- **Edge**: Call twice → no crash

### of_term_putchar(c)
- **Positive**: 'A' → A on screen
- **Edge**: '\n' → moves to next line
- **Edge**: '\r' → moves to start of line

---

## Cache (`of_cache_*`)

### of_cache_flush()
- **Positive**: Flush before DMA → DMA sees latest data
- **Edge**: Call with no dirty lines → no-op (fast)

---

## Interact (`of_interact_get`)

### of_interact_get(index)
- **Positive**: Index 0 → returns interact option value
- **Edge**: Index out of range → returns 0 or garbage (no crash)

---

## LZW (`of_lzw_*`)

### of_lzw_compress / of_lzw_uncompress
- **Positive**: Compress then uncompress → matches original
- **Positive**: Compress repetitive data → output smaller
- **Edge**: Compress 0 bytes → returns 0
- **Edge**: Compress random data → output may be larger

---

## System

### of_exit()
- **Positive**: Halts execution → never returns
- **Edge**: Called from main → clean exit

---

## BRAM (`OF_FASTTEXT`)

### OF_FASTTEXT function
- **Positive**: Function at BRAM address (< 0x10000)
- **Positive**: Function executes correctly from BRAM
- **Positive**: Performance: BRAM function faster than SDRAM equivalent
- **Edge**: Multiple OF_FASTTEXT functions → all at BRAM addresses
- **Edge**: 55KB of OF_FASTTEXT code → fills available BRAM

---

## Memory (`malloc`, `free`)

### malloc(size)
- **Positive**: malloc(1024) → returns non-NULL aligned pointer
- **Positive**: malloc(1) → returns non-NULL
- **Positive**: malloc(48*1024*1024) → returns non-NULL (large heap)
- **Negative**: malloc(0) → implementation-defined (NULL or small block)
- **Negative**: malloc(SIZE_MAX) → returns NULL
- **Edge**: 1000 small mallocs → all succeed, no fragmentation crash

### free(ptr)
- **Positive**: free(malloc(100)) → no crash
- **Positive**: free(NULL) → no-op
- **Edge**: Double free → undefined (crash OK)

### calloc(nmemb, size)
- **Positive**: calloc(10, 100) → 1000 zero-filled bytes
- **Edge**: calloc(0, 100) → returns NULL or small block

### realloc(ptr, size)
- **Positive**: Grow allocation → data preserved
- **Positive**: Shrink allocation → data preserved (truncated)
- **Positive**: realloc(NULL, 100) → same as malloc(100)
- **Positive**: realloc(ptr, 0) → same as free(ptr)
