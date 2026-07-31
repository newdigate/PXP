# PXP

Arduino/Teensyduino-style driver for the **PXP (Pixel Pipeline)**, the 2D
graphics accelerator on the NXP i.MX RT1176 (`__IMXRT1176__` only).

Clean-room implementation from the i.MX RT1170 Reference Manual rev.5,
chapter 52 — no NXP vendor driver source (e.g. `fsl_pxp.c`) is used or
derived from. MIT licensed; see `LICENSE`.

## Status

**Phases 1–3 implemented; Phases 1–2 HW-verified on the MIMXRT1170-EVKB.**
Phase 1: `fill`, `blit`, placement, all four rotations, both flips, and
blocking + async completion all pass with identical checksums in the QEMU
gate and on real silicon (`PXP_ALL=PASS`). Hardware verification found and
fixed four issues the emulator could not see — the memory and colour notes
below record what actually held on the board, not the original design
assumption. See `rt1176-evkb/examples/display/pxp_blit_test` for the gate,
and `rt1176-evkb/docs/superpowers/specs/2026-07-22-rt1176-pxp-design.md`
(Amendment 2) for the full writeup. Phase 2 added PS pre-decimation
(`pxp_decimate_test`) and YUV sources through CSC1 (`pxp_yuv_test`).
Phase 3 (below) adds alpha-surface compositing; its contested blend/key
semantics are being pinned by silicon measurement in
`rt1176-evkb/examples/display/pxp_composite_test`.

Consumed as a manifest library via `import_evkb_library(PXP)` in
`newdigate/rt1176-evkb`.

## Capabilities (Phases 1–2)

- **`fill`** — every destination pixel becomes a solid RGB888 colour,
  converted to the destination's format by hardware (`PS_BACKGROUND`,
  RM §52.3.1.23).
- **`blit`** — same-format rectangle copy. Source and destination must share
  a `PXPFormat`; cross-format conversion is deferred to Phase 5.
- **Placement** — `.outputAt(x, y)` retargets the destination window inside
  a larger surface (e.g. compositing a sprite into a framebuffer); pixels
  outside the window are provably untouched.
- **Rotation** — 0°/90°/180°/270° (`PXPRotation`), HW-verified including
  non-square surfaces (the 90°/270° axis swap).
- **Flips** — independent horizontal and vertical, composable with rotation.
- **Completion** — blocking `run()` (polls `STAT` with a timeout) and async
  `runAsync()` (IRQ 57 + `EventResponder`).
- **Decimation** — `.decimate(x, y)` pixel-drop shrink by 1/2, 1/4 or 1/8
  per axis (`PXPDecim`), ahead of any filtering. Not combinable with
  rotation (RM §52.3.4.1) nor with a YUV source in one pass.
- **YUV sources** — `PXP_UYVY1P422` and `PXP_YUV1P444` as PS sources, colour
  converted to the RGB output format by CSC1 (real coefficients, not the
  bypass). YUV is source-only; it can never be an output format.
- **Formats** — `PXP_RGB565`, `PXP_ARGB8888`, `PXP_XRGB8888`,
  `PXP_RGBA8888` (AS/PS roles only — `OUT_CTRL` has no RGBA8888 encoding),
  plus the two YUV source formats. These are abstract tokens, not raw
  register values: the PXP encodes the same format differently depending on
  its role — source (`PS_CTRL[FORMAT]`), destination (`OUT_CTRL[FORMAT]`),
  or overlay (`AS_CTRL[FORMAT]`); `pxpPsFormat()` / `pxpOutFormat()` /
  `pxpAsFormat()` translate per role.

## Phase 3: compositing

One-pass hardware composition of an overlay (the PXP's **Alpha Surface**,
AS) onto the source (PS), programmed with six fluent calls. `.overlay()`
arms the AS; calling any other overlay setter (or `.rop()`) without it is
treated as a forgotten `.overlay(sprite)` and fails with `PXP_ERR_CONFIG`
rather than silently degrading to a plain blit
(`.sourceColorKey()` is PS-side and independent):

```cpp
PXP.op().source(bg).output(screen)
        .overlay(sprite)                       // AS surface (ARGB8888 here)
        .overlayAt(40, 30)                     // position in the output window
        .overlayAlpha(PXP_ALPHA_EMBEDDED)      // per-pixel alpha blend
        .run();

// Green-screen: RGB565 overlay, keyed range shows the PS through.
PXP.op().source(bg).output(screen)
        .overlay(sprite565)
        .overlayColorKey(0x00FF00, 0x00FF00)
        .run();
```

- **Alpha modes** (`PXPAlphaMode`): `PXP_ALPHA_EMBEDDED` (per-pixel),
  `PXP_ALPHA_OVERRIDE` (global value), `PXP_ALPHA_MULTIPLY` (global scales
  per-pixel), `PXP_ALPHA_ROPS` (the raster-op ALU) — each with optional
  `invert`.
- **ROPs** (`PXPRop`): the full 12-op table (AND/OR/XOR/NOT combinations of
  AS and PS). `.rop()` is legal exactly when the mode is `PXP_ALPHA_ROPS`
  (`PXP_ERR_CONFIG` otherwise, in both directions).
- **Colorkeys**: `.overlayColorKey()` (AS key) and `.sourceColorKey()`
  (PS key), 24-bit `low..high` ranges.
- **AS formats**: `PXP_ARGB8888`, `PXP_RGBA8888`, `PXP_XRGB8888`,
  `PXP_RGB565` (`pxpAsFormat()`; anything else is `PXP_ERR_FORMAT`).
- **Validation**: the overlay rect must sit fully inside the output extent,
  and compositing does not combine with rotation or decimation in this
  phase (`PXP_ERR_CONFIG`).
- **Idle-AS discipline**: when `.overlay()` is *not* called, `_program()`
  still writes the **full** AS register set to its idle state every op — a
  degenerate `OUT_AS_ULC > OUT_AS_LRC` window, `AS_CTRL = 0`, and both
  colorkeys at the RM's never-true encoding (`low = 0xFFFFFF`,
  `high = 0x000000`). A previous composite can never leave the AS
  half-armed for a later plain blit.

The RM contradicts itself on the blend direction and the AS-key result, so
the blend/key semantics here are **silicon-measured, not transcribed** —
measured in `pxp_composite_test`'s transcript
(`rt1176-evkb/examples/display/pxp_composite_test/transcript_hw_evkb.txt`).

## Example

```cpp
#include <PXP.h>

// Surfaces must live where the PXP (an AXI bus master) can reach them -
// see "Memory rules" below. DMAMEM places a buffer in OCRAM.
DMAMEM static uint16_t fb[240 * 320];
DMAMEM static uint16_t icon[32 * 32];

PXPSurface screen(fb,    240, 320, PXP_RGB565);
PXPSurface sprite(icon,   32,  32, PXP_RGB565);

void setup() {
    PXP.begin();

    // Solid fill: every screen pixel becomes this RGB888 colour.
    PXP.fill(screen, 0x001133);

    // Same-format copy, full surface.
    PXP.blit(sprite, screen);

    // Fluent form: placed + rotated blit, blocking.
    PXP.op().source(sprite).output(screen)
            .outputAt(100, 60)
            .rotate(PXP_ROT_90)
            .run();

    // Async: returns immediately, EventResponder fires from the IRQ 57 ISR.
    static EventResponder done;
    done.attachImmediate([](EventResponderRef) { /* blit complete */ });
    PXP.op().source(sprite).output(screen).outputAt(0, 0).runAsync(&done);
}

void loop() {}
```

`PXP.op()` always returns a fresh, zero-initialised operation — there is no
persistent builder state on the `PXP` singleton to leak stale configuration
from one blit into the next.

## Memory rules (bus-master reachability)

The PXP is an AXI bus master: a surface's `data` pointer must live somewhere
it can actually reach, or the operation fails fast with
`PXP_ERR_UNREACHABLE` (`PXPSurface::reachable()` is checked internally
before every operation). HW-verified reachable ranges:

- **OCRAM** (`DMAMEM` buffers) — reachable, the default choice.
- **SDRAM** (`extmem_malloc`) — reachable.
- **CM7 DTCM** — reachable. This is a hardware finding, not the original
  design assumption: DTCM was expected to be invisible to bus masters other
  than the CM7 itself, but the PXP reads it cleanly with no AXI error on
  this FlexRAM SoC. Only DTCM-as-**source** was HW-probed; DTCM-as-
  destination is permitted by `reachable()` but unverified — a bad write
  there would surface as `PXP_ERR_AXI_WRITE` via `wait()`, not silent
  corruption.
- **FLASH** — reachable (source only, in practice — it's read-only memory).
- **ITCM** is *not* accepted — unprobed, and it's code space.

**Alignment:** rotate combined with a flip on a destination that isn't
64-byte aligned returns `PXP_ERR_ALIGN` — the one hazard RM §52.3.4.1(4)
documents. Rotate alone or flip alone is HW-confirmed to work at any byte
alignment (RM §52.6.4: "any byte alignment is valid for OUT_BUF"); the guard
fires only for the combined case.

## D-cache dependency

Correctness depends on the CM7 D-cache being **off** — and it is, in this
core (`cores/imxrt1176/imxrt1176.h:849-854`, where `arm_dcache_delete` and
`arm_dcache_flush_delete` are deliberate no-ops), so PXP surfaces need no
cache maintenance today. If a future build enables the D-cache, every PXP
surface will need real cache maintenance around each operation — this
library does not perform any.

## The CSC1 gotcha (for future YUV work)

The PXP's CSC1 block applies a YUV→RGB colour matrix to the source by
default — it resets **not bypassed**, with YUV coefficients loaded. Every
Phase-1 format is RGB, so the driver unconditionally sets
`CSC1_COEF0[BYPASS]` before each operation. This was a hardware-only bug
during bring-up: QEMU doesn't model CSC1 at all, so the emulator gate
passed while every RGB blit came back colour-transformed on silicon. Phase 5
(YUV/CSC) will make the bypass conditional and load real coefficients for
YUV sources.

## Deferred to later phases

Not yet implemented:

- The Porter-Duff blend engine (`ALPHA_B_CTRL`; Phase 3 uses the legacy
  `AS_CTRL` blend path only)
- The six remaining `AS_CTRL[FORMAT]` encodings (ARGB1555, ARGB4444,
  RGBA5551, RGBA4444, RGB555, RGB444)
- The `NEXT` hardware command queue
- Cross-format conversion (`blit` currently requires
  `src.format == dst.format` except YUV→RGB via CSC1, enforced as
  `PXP_ERR_FORMAT`)
- A CM4-owned PXP (IRQ 57 is wired to both the CM7 and CM4 NVICs, so it's
  plausible later, but out of scope here)
