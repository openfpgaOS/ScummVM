# Resolved: `fread` silently returned 0 bytes on unaligned destinations

## Status

**Fixed** in openfpgaOS commit `64b35bc` (2026-04-21,
"io v2 phase 2g — route io_cache_fill through CRAM0 bounce").
Verified on hardware 2026-04-22 with `src/apps/freadalign/` reporting
PASS on both an aligned and a stack-local destination.

The Quake shim workaround in `Repos/Quake/src/quake/shim/sys_of.c`
(`Sys_FileRead` bouncing through a 64 KB aligned staging buffer) has
been retired alongside this note.

## Original symptom

`fread(ptr, 1, n, f)` on an SDK-opened `FILE *` delivered **0 bytes**
when `ptr` was not 512-byte aligned. The call returned 0, `errno` was
not set, no kernel log was emitted, and the handle remained valid.
Aligned pointers read correctly. The Quake port spent multiple
debugging iterations tracking this down as apparent PAK-file corruption
before identifying the alignment dependency.

Reproducer remains at `src/apps/freadalign/` as a regression test.

## Root cause (pre-fix)

Before v2, the file I/O cache DMA'd via `of_file_read_raw` directly
from the bridge into SDRAM at `IO_CACHE_BASE = 0x10380000` using the
`bridge_to_sdram` fabric path. That path required 512-byte alignment
(inherited from the SD transport), and the previous layout happened to
keep the I/O cache 512-aligned — but v2 retired the `bridge_to_sdram`
path entirely while leaving `io_cache_fill` still pointed at an
address the bridge could no longer reach. Writes went nowhere, and
reads came back as whatever stale SDRAM bytes lived there. That
presented user-side as "fread returns 0 for unaligned dst" in some
configurations and "fread returns garbage" in others (see the
`64b35bc` commit message for the moddemo/mididemo failure mode).

The underlying alignment requirement on the bridge DMA remains — it's
a hardware constraint of the SD transport — but user code never sees
it, because all user-visible reads now go through a kernel-owned bounce.

## The fix

`io_cache_fill` in `src/firmware/os/kernel/syscall.c` now calls
`of_file_read`, which routes through the `of_disk_bridge` backend:

```
bridge DMA → CRAM0_SCRATCH (known aligned, kernel-owned)
          → memcpy        → SDRAM io_cache (CPU-side copy, no alignment req.)
          → memcpy        → user dst (byte-wise, os_string.h memcpy)
```

Both memcpys use the byte-wise fallback in `src/firmware/os/os_string.h`,
which has no alignment requirement. User-dst alignment is therefore
no longer observable from the read path.

## Regression guard

`src/apps/freadalign/` opens the first file ≥512 B on the card and
reads 16 bytes into both an `__attribute__((aligned(512)))` buffer and
a 4/8-byte-aligned stack local. Historically the unaligned read
returned 0; a correctly-behaved kernel delivers identical bytes to
both. Run after any change to the file read path, the I/O cache, or
the bridge DMA plumbing.
