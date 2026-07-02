# Design Document: GPU-Composited Translucency for the SCI32 Inventory Window (LSL7)

Scope: LSL7 (SCI2.1, 640×480 CLUT8) translucent inventory. Design only. Grounded against the actual sources (line refs verified this session).

---

## PROBE RESULTS (live, as phases run)

**Phase 1 — GPU translucency hardware probe: PASSED / GO (2026-07-01).**
On-device probe (`openfpga_gpu_transluc_probe`, backend) drew bg=0x40+x, blitted constant texel 0xE8 through a hand-built LUT with `OF_GPU_SPAN_TRANSLUC`, read back `out=40,41..4f`. Since the output is the *background passed through the LUT* (not the opaque texel 0xE8), the GPU **did** consume the translucency table → `INCLUDE_TRANSLUC` is present in this bitstream. **Hardware gate passed.**
Resolves open question #2 (the axis): the HW indexes `translucency[(source<<8) | dest]` — i.e. **SOURCE (overlay/cel index) = high byte = the ÷2-DECIMATED axis; DEST (background) = low byte = full res.** This is INVERTED from gpudemo's build orientation (their symmetric blend hid it). Consequences for the design:
- Build SCI's LUT as `table[(source<<8)|target] = remapColor(source,target)` (was assumed `(target<<8)|source` in §2 — **§2's "opaque chrome rides free" note is now WRONG**).
- Decimation hits the OVERLAY source index, so opaque window chrome (per-source passthrough, needs distinct rows for src and src^1) **cannot** share the transluc pass. The flat translucent PANEL (a single remap index — pick it even) rides the transluc pass; opaque chrome takes a SEPARATE normal skip-keyed blit. Not a blocker.

**Phase −1 — does the win exist: PASSED, with a corrected mechanism (2026-07-01).**
On-device split of per-plane cel draws (scene vs transparent). Inventory open, steady: `cels/f≈45` = `scene/f≈12` + `trans/f≈33`; normal play `scene/f≈5`, `trans/f=0`. So the recomposition cost is NOT force-redrawn SCENE cels (only ~7 extra) — it is the TRANSPARENT plane's OWN ~33 cels being re-blended EVERY frame (filterUpDrawRects re-adds them because the ~7 animating scene cels under the window change). This is still exactly what the peel fixes: draw the overlay content ONCE into buffer B (persistent), HW-composite over the fresh scene each frame → the ~33 redraws/frame collapse to a one-time draw. **Corrected payoff estimate: cels/frame ~45 → ~12, ≈2–2.5× (foMs ~150 → ~70-90ms, ~5 → ~11-14 fps); the ~27ms/frame audio-decode pump is a hard floor.** Design impact: the peel targets the transparent plane (as designed); neutering the scene filter (§3) is now a minor part — the big win is making the overlay's own content persistent.

---

## 1. VERDICT — CONDITIONAL GO

Feasible and worth prototyping, but **do not commit to the engine refactor until one on-device probe passes.** The primitive fit is real and exact: SCI's `remapColor(source, target)` (remap32.h:317-338) is byte-for-byte a BUILD `transluc[src][dst]` table, which is precisely what `of_gpu_translucency_upload` + `OF_GPU_SPAN_TRANSLUC` consume. gpudemo already ships a working instance of that path on a Pocket bitstream (main.c:179-185, 505-507), so the hardware very likely has it.

**The single biggest risk is not the GPU — it is whether peeling the overlay actually removes the cost.** The measured ~40ms of cel redraws only disappears if those cels are the *translucent window panel being re-blended* (pulled back into the draw list every frame by `filterUpDrawRects`, frameout.cpp:1001). If instead they are the *LSL7 talkie animation genuinely repainting the scene behind the window every frame*, then the scene planes must redraw those cels regardless of how we composite the window, and the win collapses from ~3× to maybe ~1.3×. This is measurable off the existing `g_ofMapPixels` / `g_ofMapCalls` counters (celobj32.cpp:920-921) **before** any refactor — and it should be the very first thing checked, ahead of even the GPU probe.

Honest payoff math: frame is ~167ms (~6fps). Best realistic case removes the ~40ms panel re-blend plus part of the ~65ms erase/palette overhead the transparent plane forces → ~90-100ms (~10fps). The advertised "3×" (~55ms) additionally requires that most of that 65ms overhead is *caused by* the transparent-plane recomposition and vanishes when it's peeled. That is an assumption, not a measurement.

---

## 2. ARCHITECTURE — target data flow

### Today (flat, one buffer)
```
SCI planes (priority order) --draw--> GfxFrameout::_currentBuffer (one CLUT8 640x480, persistent)
   translucent panel cel: *dst = remapColor(cel_px, *dst)   [celobj32.cpp:941, reads dst]
      -> forces recompose of scene under window every frame
   -> showBits() copies dirty rects -> copyRectToScreen -> _screenBuf
   -> updateScreen(): memcpy dirty box into 1 of 3 GPU FBs, CPU cursor, flip
GPU used ONLY for flip + border clears.
```

### Target (two layers, GPU blend)
```
LAYER A (scene/background): SCI renders NON-transparent planes into _currentBuffer as today.
                            Its own small dirty rects only (Plane::calcLists already computes them).
LAYER B (overlay):          The peeled transparent plane's screen items rendered into a SEPARATE
                            CLUT8 buffer, with remap cels kept as RAW remap-index pixels
                            (do NOT call remapColor). Plus the union window rect.
PER FRAME (backend, gated):
   1. establish A in the acquired GPU FB (dirty-box memcpy of _screenBuf, as today)
   2. of_gpu_set_framebuffer(FB)              [openfpga_osystem.cpp:329 uses this already]
   3. of_gpu_bind_texture(overlay B)
   4. emit window rect, flags |= OF_GPU_SPAN_TRANSLUC (+SKIP_ZERO), s/t 1:1
        -> per pixel: out = transluc[(fb_pixel<<8) | overlay_texel]   (see LUT below)
   5. CPU cursor composite -> of_gpu_flip_to -> present   [existing presentFrame :370-386]
LUT uploaded ONCE (rebuilt only when remap tables / palette change, gated on _remapOccurred).
```

### The LUT — exact construction and the resolved axis order

`remapColor(source, target)` (remap32.h:317-338):
```
index = _remapEndColor - source
if (target >= _remapStartColor) return 0     // RAMA/skip-target rule, must be reproduced
return _remaps[index]._remapColors[target]
```
`_remapColors` is `[237]` (remap32.h:107); `_remapStartColor` is the first remap index (≈237).

**Axis order (verified, load-bearing).** `of_gpu_translucency_upload` (of_gpu.h:803-815) keeps only rows whose **high byte is even**: `row = &table[(s7<<1)<<8]`, 128 rows × 64 words. gpudemo builds `translucency[(dst<<8)|src]` (main.c:183) and it renders correctly, with `src` = the constant texel `0xe8` (main.c:214) and `dst` = the already-drawn background. Therefore:

- **low byte = the texel** (overlay pixel) → **full 256 resolution**
- **high byte = the framebuffer pixel** (background) → **decimated ÷2 (low bit dropped)**

Build the SCI table in the **same orientation gpudemo proved**:
```c
// texel = SCI source (overlay/cel index) -> LOW byte (full res)
// fb    = SCI target (scene index)        -> HIGH byte (decimated /2)
for (int fb = 0; fb < 256; ++fb)          // = SCI targetColor
  for (int texel = 0; texel < 256; ++texel) {  // = SCI sourceColor
    uint8 out;
    if (texel == skipColor)                 out = texel; // transparent, see SKIP_ZERO note
    else if (texel <  remapStart)           out = texel; // opaque passthrough
    else if (remap->remapEnabled(texel))    out = remap->remapColor(texel, fb);
    else                                    out = texel;
    table[(fb << 8) | texel] = out;
  }
of_gpu_translucency_upload(table, 65536);
```

**This corrects a claim in two of the three reports.** They assumed the *source* axis is decimated and therefore that opaque window content (icons/text/frame) "cannot ride the same TRANSLUC pass" and that overlay remap indices must be spaced even. Under the *proven* orientation the opposite holds:

- **Opaque source rows are `out = source`, target-independent → decimation is irrelevant to them.** So a **single** TRANSLUC pass can carry BOTH the flat translucent panel AND the opaque window chrome. That is a meaningful simplification.
- The ÷2 quantization lands on the **background being dimmed** (`fb`/`target`), not the overlay. All overlay remap indices 236..254 survive at full resolution. The cost is that two adjacent *scene* palette indices (2k, 2k+1) dim to the same result — visible only if LSL7's palette places very different colors on adjacent even/odd indices under the window.

**Caveat (must probe):** gpudemo's blend is a ~symmetric 50% average, so gpudemo alone cannot fully distinguish "texel→low / fb→high" from the inverse. `remapColor` is strongly asymmetric, so a Phase-0 asymmetric fixture will confirm it. If the probe shows the inverse, flip one line to `table[(texel<<8)|fb]` and the reports' original concern returns (opaque content then needs even source indices or a second skip-keyed pass).

---

## 3. INTERCEPTION POINTS (file:line)

**SCI engine** (`engines/sci/graphics/`):

- **frameout.cpp:1020 `drawScreenItemList`** and the plane loop around **:1008 `drawEraseList`** — the split point. For the peeled transparent plane, redirect its screen items into overlay buffer B and, for its remap cel, **emit the raw cel index** instead of the `remapColor` blend.
- **celobj32.cpp:911-945 `drawUncompNoFlipMap`** (the openfpga fast path; blend at :941 `*dst = remap->remapColor(p, *dst)`, skip at :936, opaque at :938-939) and **:499-511 `MAPPER_Map`** — the exact semantics to (a) replicate in the LUT and (b) branch to "write raw `p` into overlay B" when the cel belongs to the peeled plane. Keep skip handling identical (`p==skip → leave transparent`).
- **frameout.cpp:981-1005 (the `foundTransparentPlane` block)** — the recomposition driver. `filterUpEraseRects` (:985), `filterDownEraseRects` (:991), `filterUpDrawRects` (:1001) are what re-add the window cel to the draw list whenever any lower rect intersects it. For the peeled plane, **neuter these three calls** so the scene planes redraw only their own dirty rects. Note the hard invariant at :994-996 (`error("Transparent plane's erase list not absorbed")`) — the peel must still absorb/clear `eraseLists[planeIndex]` or this fires.
- **frameout.cpp:1095 `showBits` / :1144 `copyRectToScreen`** — after copying scene dirty rects, call a NEW backend hook `openfpga_composite_*` with (overlay B, window rect, lutId). Non-LSL7 frames never arm it → inert.
- **frameout.cpp:503, 523 (`_remapOccurred = _palette->updateForFrame()`)** — the rebuild gate. Re-upload the LUT only when this is true (rare while a static-percent panel is open).
- **remap32.h:317-338 `remapColor`, :301 `remapEnabled`, :241-242 `getStartColor/getEndColor`, :107 `_remapColors[237]`** — source data for the table build.

**Backend** (`backend/openfpga_osystem.cpp`, I own this, single TU that includes of_gpu.h per of_gpu.h:18-22):

- New `extern "C"` hooks next to `updateScreen` (:396): `openfpga_composite_upload_remap(...)`, `openfpga_composite_present(overlay, pitch, rect)`. SCI must NOT include of_gpu.h — call through these.
- Reuse `acquireFrameBuffer` (:278), `of_gpu_set_framebuffer` (:329), `presentFrame`/`of_gpu_flip_to` (:370-386), the `_gpuStalled` bounded-wait guard (:315-321), `of_cache_flush_range` (:519), and extend the `s_fbDrawn[]` per-buffer tracking (:188, 469-481) to the composite path.

**How the plane gets peeled:** at the frameout.cpp:981-1005 gate, when exactly one `kPlaneTypeTransparent` plane is active AND it matches the LSL7 signature (single flat active-remap backing cel + opaque chrome — confirm via the printPlaneItemList dump), route that plane's `drawScreenItemList` output to buffer B (raw indices), skip the three filter calls, and let the scene planes composite normally into `_currentBuffer`.

---

## 4. PHASED PLAN (each phase independently deployable + verifiable; fallback at each)

**Phase −1 — Off-device: prove the win even exists (no GPU, no refactor).**
Add UART logging of `g_ofMapCalls`/`g_ofMapPixels` (celobj32.cpp:920-921) and the per-plane draw-list sizes in calcLists while the inventory is open. Determine: is the ~40ms the ONE big panel re-blend, or N animating scene cels? Deploy once, user opens inventory, reads the log.
*Gate:* if the scene genuinely animates every frame under the window, STOP — redesign target is "cut the panel re-blend + erase overhead only" (~1.3-1.5×), not 3×. Fallback: none needed, this is pure instrumentation.

**Phase 0 — Off-device: LUT correctness in software.**
Build `table[65536]` from `GfxRemap32` and assert in a host unit test that `table[(target<<8)|source]` reproduces `remapColor(source,target)` byte-for-byte for every (source in remap range, target) pair, including the `target>=_remapStartColor → 0` rule (remap32.h:334). No device needed.
*Fallback:* if it doesn't match, the whole idea is wrong — cheap to discover.

**Phase 1 — On-device: GPU transluc probe (the critical hardware gate).**
Standalone test path in the backend (behind an os.ini flag): draw a known asymmetric 2-color background, upload a hand-built table, blit a known constant texel with `OF_GPU_SPAN_TRANSLUC`, read back the FB. Confirms three things at once: (a) `INCLUDE_TRANSLUC` is actually in this bitstream (no caps bit exists — of_caps.h:74-76 — so this is the ONLY way to know); (b) which physical input is the decimated high byte (texel vs fb); (c) that the 0x48 affine path honors bit 6. Also dump `of_get_caps()->hw_features` to check `OF_HW_GPU_SPAN_GROUP` (bit 23).
*Fallback:* if transluc is absent or inverted-and-unfixable → NO-GO for GPU; stop here, keep the CPU path. Cost sunk is only the probe.

**Phase 2 — On-device: software peel (prove the split is visually identical, still no GPU blend).**
Implement the layer split (Sections 3 peel points) but composite B over A in **software** using the same LUT (or just call the existing CPU `remapColor`). This validates that peeling the plane and neutering the three filters produces a pixel-identical frame, and that non-LSL7 games are untouched.
*Fallback:* if visuals diverge, the filter-neutering is wrong; revert the peel, keep everything else. This phase is where the SCI regression surface is actually exercised, deliberately BEFORE any GPU dependency.

**Phase 3 — On-device: swap the software overlay blend for the GPU TRANSLUC span.**
Replace the Phase-2 CPU composite with `of_gpu_bind_texture(B)` + `OF_GPU_SPAN_TRANSLUC` emit, gated on the Phase-1 result. Extend `s_fbDrawn[]` so the window rect is re-composited into all 3 rotating buffers. Handle `_gpuStalled` → fall back to Phase-2 CPU blend.
*Fallback:* per-frame, if `!of_has_feature(OF_HW_GPU_SPAN_GROUP)` use `of_gpu_draw_param_span_list` (of_gpu.h:1425, "decodes on every core"); if that's slow or transluc misbehaves, drop to the Phase-2 CPU blend at runtime. The CPU path stays compiled in as the always-available floor.

**Phase 4 — Optional: also render Layer A on the GPU / reduce per-buffer bg restores** to shave the memcpy. Only if Phase 3 measurements justify it.

---

## 5. EFFORT + RISK

**Rough size:**
- Phase −1: ~0.5 day (instrumentation).
- Phase 0: ~1 day (LUT build + host test).
- Phase 1: ~1-2 days (self-contained backend probe; owned code).
- Phase 2: ~3-5 days (the real engine surgery: peel + filter neutering + arming gate; highest-touch).
- Phase 3: ~2-3 days (backend GPU emit + triple-buffer handling).
- Total to first GPU frame: ~1.5-2 weeks, front-loaded with cheap kill-gates.

**Top 3 risks:**
1. **The background animates anyway (payoff risk).** If the scene under the window repaints every frame from the talkie, peeling the overlay removes only the panel re-blend, not the actor redraws — sub-2× and possibly not worth the regression surface. *Mitigated by Phase −1 measuring this before any refactor.*
2. **Transluc hardware unverifiable at build time (all-or-nothing).** `OF_HW_GPU_ALPHA` is defined but "not yet advertised" (of_caps.h:74-76); there is no runtime feature bit for the LUT path. If the bitstream lacks `INCLUDE_TRANSLUC`, the span draws opaque/garbage with no error. *Mitigated by Phase 1 probe as a hard gate; CPU fallback always present.*
3. **SCI regression across ~30 games.** frameout.cpp:981-1005 and the filter functions are load-bearing for *every* transparent/transparent-picture plane (menus, subtitles, control panels) in KQ7/Phant/QFG/LSL7 and SCI16/SCUMM/AGI go through frameOut too. Neutering the filters for the wrong plane corrupts. *Mitigated by a tight arming gate (Section 6) and by Phase 2 exercising the split in software first.*

**What makes it not pay off:** animating background (risk 1); the RMW TRANSLUC pass being SDRAM-bandwidth-bound and contending with CPU + audio DMA on the 16-bit PHY so the GPU blend isn't actually cheaper than the CPU one it replaces (probe `GPU_CHANUTIL` via `of_gpu_debug_snapshot`); the per-rotating-buffer background restore memcpy eating the savings; and LSL7 palette cycling forcing frequent 8192-word LUT re-uploads (each polling `_gpu_wait_transluc_idle`, of_gpu.h:810) — measure `_remapOccurred` frequency while the panel is open.

---

## 6. NON-REGRESSION (the other ~30 games stay on the current path)

- **New API is additive and port-private.** Do NOT touch the virtual `OSystem`/`GraphicsManager` interface. Add `extern "C"` hooks in openfpga_osystem.cpp only. Unarmed, they are no-ops → SCUMM/AGI/SCI16 and all non-inventory SCI32 frames flow through the untouched `_screenBuf` memcpy + flip (openfpga_osystem.cpp:208, 396-576).
- **Arming gate is triple-conditioned:** (engine == SCI32) AND (a `kPlaneTypeTransparent`/`TransparentPicture` plane is active this frame) AND (that plane matches the LSL7 single-flat-active-remap signature). Anything else → the frameout.cpp:981-1005 block runs unmodified. Concretely: guard the peel/filter-neuter with a runtime predicate; when false, the original filterUp/Down calls execute exactly as today.
- **CPU path stays compiled in as the runtime floor**, selected whenever the gate is false, `_gpuStalled` is set, or the Phase-1 probe flag is off. No bitstream-dependent build fork.
- **Phase ordering protects this:** Phase 2 proves the split is pixel-identical (and other games unaffected) in software before the GPU is a dependency; Phase 1 confirms hardware in isolation. The high-regression engine change and the unverified hardware are never introduced in the same deploy.

---

## 7. OPEN QUESTIONS — probe before committing

1. **[Blocking] Is the background actually animating under the open inventory?** Dump `g_ofMapCalls`/`g_ofMapPixels` (celobj32.cpp:920-921) and per-plane draw-list sizes in calcLists. Decides whether the win is ~3× or ~1.3×. Do this FIRST (Phase −1).
2. **[Blocking] Does this bitstream have `INCLUDE_TRANSLUC`, and which physical input is the decimated high byte (texel vs fb)?** No caps bit exists (of_caps.h:74-76). Phase-1 asymmetric fixture. Determines whether the LUT is `table[(fb<<8)|texel]` (proven-gpudemo orientation, opaque chrome rides free) or the inverse (opaque needs even source indices / second pass).
3. **Does the 0x48 affine-span path honor bit 6 (TRANSLUC), or is transluc only wired to the tri/param-span path?** Same Phase-1 probe.
4. **`OF_HW_GPU_SPAN_GROUP` (bit 23) present on this core?** `of_get_caps()->hw_features` dump. If clear, the compact emitter is a silent no-op (of_gpu.h:1122) → must use `of_gpu_draw_param_span_list`.
5. **Exact LSL7 inventory plane graph** — is the dimmed backing a SINGLE flat active-remap cel, or multiple remap colors / remap+opaque interleaved? `printPlaneItemList` (frameout.cpp) runtime dump. Confirms the arming signature and that a single TRANSLUC pass suffices.
6. **How many distinct remap source indices, and does LSL7's palette place divergent colors on adjacent even/odd *scene* indices?** With the proven orientation the ÷2 quantization hits the background; need to confirm it's visually acceptable. `getRemapCount` + a palette inspection.
7. **LUT re-upload frequency:** how often is `_remapOccurred` true (frameout.cpp:523) while the panel is open? If palette cycling makes it frequent, the 8192-word upload may dominate.
8. **SDRAM-bound cost of the RMW pass over the inventory sub-rect under CPU+audio contention** — `GPU_CHANUTIL` via `of_gpu_debug_snapshot` (of_gpu.h). Confirms the GPU blend is actually cheaper than the CPU blend it replaces.
9. **Cursor ordering:** the CPU composites the cursor into the FB after the blit (openfpga_osystem.cpp:510). Confirm the GPU composite lands before the cursor draw so cursor-over-window ordering is preserved, and that the `of_gpu_finish()` fence this implies (one wait/frame) doesn't cost more than it saves on the in-order core.

**Investigator uncertainties I could not resolve from source, flagged honestly:** the texel-vs-fb physical axis assignment (gpudemo's symmetric blend can't disambiguate — needs the asymmetric probe); whether transluc is compiled into *this* specific bitstream at all; and the true redraw cause behind the window (counters exist but were not read on-device). All three are device probes, and all three are cheap kill-gates placed in Phases −1 and 1 ahead of the expensive engine work.

Relevant files: `/home/alberto/Repos/ScummVM/src/scummvm/scummvm/engines/sci/graphics/frameout.cpp`, `.../celobj32.cpp`, `.../remap32.h`, `/home/alberto/Repos/ScummVM/src/scummvm/backend/openfpga_osystem.cpp`, `/home/alberto/Repos/ScummVM/src/sdk/include/of_gpu.h`, `/home/alberto/Repos/ScummVM/src/sdk/include/of_caps.h`, `/home/alberto/Repos/ScummVM/src/apps/gpudemo/main.c`.
