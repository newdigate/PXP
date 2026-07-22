# PXP

Arduino/Teensyduino-style driver for the **PXP (Pixel Pipeline)**, the 2D
graphics accelerator on the NXP i.MX RT1176 (`__IMXRT1176__` only).

Clean-room implementation from the i.MX RT1170 Reference Manual rev.5,
chapter 52 — no NXP vendor driver source (e.g. `fsl_pxp.c`) is used or
derived from. MIT licensed; see `LICENSE`.

## Status

**Phase 1 skeleton — not yet functional.** This checkout currently holds the
public API surface (`PXP.h`) and a stub driver (`PXP.cpp`) whose every entry
point fails deterministically (`begin()` returns `false`, all operations
return `PXP_ERR_UNIMPLEMENTED`). It exists to prove the build/import plumbing
(`import_evkb_library(PXP)` in `newdigate/rt1176-evkb`) before any real
register-level driver is written.

Note on `PXPFormat`: the enumerators are abstract tokens, not hardware
register encodings. The PXP programs the same logical format with different
bit patterns depending on which register it lands in (`PS_CTRL[FORMAT]` is a
6-bit field with no `0x00` encoding; `OUT_CTRL[FORMAT]` is a 5-bit field with
no `0x24`; `AS_CTRL[FORMAT]`, unused until Phase 3, is only 4 bits wide) — so
translation is done per-role by `pxpPsFormat()`/`pxpOutFormat()`, verified
against RM rev.5 §52.6.3 and §52.6.12.

Consumed as a manifest library by the `pxp_blit_test` gate in
`rt1176-evkb/examples/display/pxp_blit_test`, which is expected to build and
link against this stub and then fail at runtime with `PXP_BEGIN=FAIL` — the
intentional "red" of a red/green bring-up.

## Planned scope (Phase 1)

A headless memory-to-memory 2D engine: solid fill, blit, 90/180/270 rotation
and horizontal/vertical flips, with both blocking (`run()`) and async
(`runAsync()` via IRQ 57 + `EventResponder`) completion. Scaling, colour-space
conversion, alpha-surface compositing, Porter-Duff blending and the `NEXT`
operation queue are out of scope for Phase 1.

Usage documentation, the memory-placement rule for `PXPSurface` (which bus
masters can reach which RAM), and the D-cache dependency will be written up
here once the driver is implemented and hardware-verified.
