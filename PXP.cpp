/* PXP.cpp - i.MX RT1176 Pixel Pipeline driver
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */
#include "PXP.h"
#include <EventResponder.h>

#if defined(__IMXRT1176__)

/* Promote -Wswitch to a hard error for this file.  The toolchain does not use
 * -Werror and the cores build already emits unrelated warnings, so a bare
 * warning here would scroll past unnoticed - which would defeat the whole
 * point of the no-default: switches below. */
#pragma GCC diagnostic error "-Wswitch"

PXPClass PXP;

static PXPClass *pxp_instance = nullptr;

/* Format translation.  Each switch covers every enumerator and has NO
 * default:, so adding a PXPFormat without teaching all three tables is a
 * compile error (see the pragma above) instead of a silently wrong register
 * value.  AS_CTRL[FORMAT] (RM 52.6.22) is a third namespace, unused until
 * Phase 3, that will need its own translator and its own coverage here then.
 * Verified against RM rev.5 52.6.3 (OUT_CTRL) and 52.6.12 (PS_CTRL). */
uint8_t pxpPsFormat(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888: return 0x04;   /* RGB888_ARGB8888 - PS has no 0x00 */
    case PXP_XRGB8888: return 0x04;   /* same PS encoding as ARGB8888     */
    case PXP_RGB565:   return 0x0E;
    }
    return PXP_FMT_NA;
}

uint8_t pxpOutFormat(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888: return 0x00;
    case PXP_XRGB8888: return 0x04;   /* RGB888, unpacked 24-bit in 32 bits */
    case PXP_RGB565:   return 0x0E;
    }
    return PXP_FMT_NA;
}

uint16_t pxpBitsPerPixel(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888: return 32;
    case PXP_XRGB8888: return 32;
    case PXP_RGB565:   return 16;
    }
    return 0;
}

bool PXPClass::begin()
{
    if (_begun) return true;

    /* 1. Ungate the peripheral clock (LPCG127, BUS_CLK_ROOT). */
    CCM_LPCG127_DIRECT = 1;

    /* 2. RM 52.5: set SFTRST, then clear SFTRST and CLKGATE.
     *    CTRL comes out of reset as 0xC000_0000 with both bits set, so this
     *    sequence is mandatory - without it every operation is a no-op. */
    PXP_CTRL_SET = PXP_CTRL_SFTRST;
    for (volatile int i = 0; i < 100; i++) { }        /* reset settle */
    PXP_CTRL_CLR = PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE;

    /* 3. Confirm the block actually came alive. */
    if (PXP_CTRL & (PXP_CTRL_SFTRST | PXP_CTRL_CLKGATE)) {
        _lastError = PXP_ERR_TIMEOUT;
        return false;
    }

    PXP_STAT_CLR = PXP_STAT_IRQ;
    pxp_instance = this;
    _begun = true;
    _lastError = PXP_OK;
    return true;
}

void PXPClass::end()
{
    if (!_begun) return;
    /* Disable IRQ 57 in the NVIC before gating the block: a torn-down PXP must
     * not be able to take a completion interrupt. */
    NVIC_DISABLE_IRQ(IRQ_PXP);
    PXP_CTRL_SET = PXP_CTRL_CLKGATE;
    CCM_LPCG127_DIRECT = 0;
    _begun = false;
}

bool PXPClass::busy() const
{
    return _begun && (PXP_CTRL & PXP_CTRL_ENABLE);
}

PXPError PXPClass::wait(uint32_t timeout_ms)
{
    uint32_t start = millis();
    while (!(PXP_STAT & PXP_STAT_IRQ)) {
        if ((millis() - start) > timeout_ms) return PXP_ERR_TIMEOUT;
    }
    uint32_t stat = PXP_STAT;
    PXP_STAT_CLR = PXP_STAT_IRQ | PXP_STAT_AXI_READ_ERROR | PXP_STAT_AXI_WRITE_ERROR;
    if (stat & PXP_STAT_AXI_READ_ERROR)  return PXP_ERR_AXI_READ;
    if (stat & PXP_STAT_AXI_WRITE_ERROR) return PXP_ERR_AXI_WRITE;
    return PXP_OK;
}

PXPError PXPClass::fill(const PXPSurface &dst, uint32_t rgb888)
{
    PXPOp o;
    o._dst = &dst;
    o._bg = rgb888 & 0x00FFFFFFu;   /* PS_BACKGROUND is 24-bit; [31:24] reserved */
    o._fillOnly = true;
    return o.run();
}

/* Which memory can the PXP (an AXI bus master) reach?  HW-verified on the EVKB:
 * the PXP reads CM7 DTCM cleanly (no AXI error), so DTCM is a reachable source
 * - the "bus masters can't see TCM" assumption is false on this FlexRAM SoC.
 * ITCM (0x0, code space) is NOT probed and stays rejected: it is an atypical
 * surface region and unverified.  Caller owns not blitting over its own stack.
 * NOTE: role-agnostic (like the FLASH entry).  Only DTCM-as-SOURCE is HW-probed;
 * DTCM-as-destination is permitted but unverified - a bad PXP write there
 * surfaces as PXP_ERR_AXI_WRITE via wait(), not silent corruption. */
bool PXPSurface::reachable() const
{
    /* Validate the WHOLE surface extent, not just the base - a surface based
     * in-range but running past a region's end is not reachable. */
    uint32_t a   = (uint32_t)data;
    uint32_t end = a + (uint32_t)sizeBytes();
    if (end < a) return false;                                  /* wrap */
    if (a >= 0x20000000u && end <= 0x20040000u) return true;    /* DTCM  256K (HW) */
    if (a >= 0x20240000u && end <= 0x202C0000u) return true;    /* OCRAM  512K */
    if (a >= 0x80000000u && end <= 0x84000000u) return true;    /* SDRAM   64M */
    if (a >= 0x30000000u && end <= 0x31000000u) return true;    /* FLASH   16M */
    return false;
}

PXPError PXPOp::_program()
{
    if (!_dst || !_dst->data)               return PXP_ERR_CONFIG;
    if (!_fillOnly && (!_src || !_src->data)) return PXP_ERR_CONFIG;
    if (!_dst->reachable())                 return PXP_ERR_UNREACHABLE;
    if (!_fillOnly && !_src->reachable())   return PXP_ERR_UNREACHABLE;

    /* PS/source frame dims (fill: the destination window).  These go straight
     * to OUT_LRC / OUT_PS_LRC: with output-stage rotation (ROT_POS=0) the
     * hardware rotates THIS source frame and lays the result out via OUT_PITCH.
     * HW-verified - a 90/270 rotation of a non-square surface needs the SOURCE
     * dims here, NOT the post-rotation dims (they differ only when W != H). */
    uint16_t ps_w = _fillOnly ? _dst->width  : _src->width;
    uint16_t ps_h = _fillOnly ? _dst->height : _src->height;

    /* Reject a degenerate window: ps-1 would underflow uint16 to 0xFFFF and
     * PXP_COORD would program a 16383-row rectangle.  QEMU clamps to 1024 and
     * tolerates it; silicon does not - it runs off the end of the buffer. */
    if (ps_w == 0 || ps_h == 0) return PXP_ERR_CONFIG;

    /* The rotated OUTPUT extent (what actually gets written) - 90/270 swap the
     * axes.  This, at (x,y), is what must fit inside the destination. */
    uint16_t out_w = ps_w, out_h = ps_h;
    if (_rot == PXP_ROT_90 || _rot == PXP_ROT_270) {
        uint16_t t = out_w; out_w = out_h; out_h = t;
    }
    if ((uint32_t)_x + out_w > _dst->width ||
        (uint32_t)_y + out_h > _dst->height) return PXP_ERR_CONFIG;

    /* Phase 1 does plain same-format copies (spec 3); a cross-format blit
     * without CSC copies raw bytes the destination then misreads.  Enforce it
     * explicitly - Phase 5 (CSC/YUV) is where this relaxes. */
    if (!_fillOnly && _src->format != _dst->format) return PXP_ERR_FORMAT;

    /* Translate to the per-role hardware encodings; a format legal in one
     * role may not exist in the other. */
    uint8_t out_fmt = pxpOutFormat(_dst->format);
    uint8_t ps_fmt  = _fillOnly ? 0u : pxpPsFormat(_src->format);
    if (out_fmt == PXP_FMT_NA || ps_fmt == PXP_FMT_NA) return PXP_ERR_FORMAT;

    uint8_t  dbpp = _dst->bytesPerPixel();
    if (dbpp == 0 || _dst->pitch == 0) return PXP_ERR_CONFIG;
    if (!_fillOnly && (_src->bytesPerPixel() == 0 || _src->pitch == 0))
        return PXP_ERR_CONFIG;

    uint32_t out_buf = (uint32_t)_dst->data + (uint32_t)_y * _dst->pitch
                                           + (uint32_t)_x * dbpp;

    /* Alignment: RM 52.6.4 says any byte alignment is valid for OUT_BUF (64B
     * recommended only for performance), and the PXP_ALIGN probe HW-confirmed a
     * 2-byte-offset rotate-alone is correct.  The one documented hazard is
     * RM 52.3.4.1(4): rotate COMBINED with flip (or scale/decimation) on an
     * unaligned buffer.  So guard only that combination, conservatively at 64B
     * (the combined threshold is not separately probed); rotate-alone and
     * flip-alone run at any alignment. */
    if (_rot != PXP_ROT_0 && (_hflip || _vflip) && (out_buf & 0x3Fu))
        return PXP_ERR_ALIGN;

    /* Output window is retargeted, NOT offset via OUT_PS_ULC: PXP writes the
     * whole OUT_LRC rectangle, so offsetting there would repaint everything
     * around the sprite with PS_BACKGROUND. */
    PXP_OUT_CTRL   = (uint32_t)out_fmt & PXP_OUT_FORMAT_MASK;
    PXP_OUT_BUF    = out_buf;
    PXP_OUT_PITCH  = _dst->pitch;
    PXP_OUT_LRC    = PXP_COORD(ps_w - 1, ps_h - 1);

    PXP_PS_BACKGROUND = _bg;

    if (_fillOnly) {
        /* Degenerate PS rectangle => every output pixel is PS_BACKGROUND. */
        PXP_OUT_PS_ULC = PXP_COORD(1, 1);
        PXP_OUT_PS_LRC = PXP_COORD(0, 0);
    } else {
        PXP_PS_CTRL  = (uint32_t)ps_fmt & PXP_PS_FORMAT_MASK;
        PXP_PS_BUF   = (uint32_t)_src->data;
        PXP_PS_PITCH = _src->pitch;
        PXP_OUT_PS_ULC = PXP_COORD(0, 0);
        PXP_OUT_PS_LRC = PXP_COORD(ps_w - 1, ps_h - 1);
    }

    /* AS is unused in Phase 1: park it outside the window. */
    PXP_OUT_AS_ULC = PXP_COORD(1, 1);
    PXP_OUT_AS_LRC = PXP_COORD(0, 0);

    /* Bypass CSC1.  It resets NOT-bypassed with YUV->RGB coefficients loaded,
     * so without this the PS datapath colour-mangles every RGB source (silicon
     * only - QEMU does not model CSC1).  All Phase-1 formats are RGB; Phase 5
     * (YUV) makes this conditional and loads real coefficients instead. */
    PXP_CSC1_COEF0 = PXP_CSC1_BYPASS;

    /* ROT_POS=0: rotate at the output stage (HW-verified for non-square). */
    uint32_t ctrl = PXP_CTRL;
    ctrl &= ~(PXP_CTRL_ROTATE_MASK | PXP_CTRL_HFLIP | PXP_CTRL_VFLIP |
              PXP_CTRL_IRQ_ENABLE | PXP_CTRL_ROT_POS);
    ctrl |= ((uint32_t)_rot << PXP_CTRL_ROTATE_SHIFT);
    if (_hflip) ctrl |= PXP_CTRL_HFLIP;
    if (_vflip) ctrl |= PXP_CTRL_VFLIP;
    PXP_CTRL = ctrl;

    return PXP_OK;
}

PXPError PXPOp::run(uint32_t timeout_ms)
{
    if (!PXP._begun)  return PXP_ERR_NOT_BEGUN;
    if (PXP.busy())   return PXP_ERR_BUSY;

    PXPError e = _program();
    if (e != PXP_OK) { PXP._lastError = e; return e; }

    PXP_STAT_CLR = PXP_STAT_IRQ;
    PXP_CTRL_SET = PXP_CTRL_ENABLE;

    e = PXP.wait(timeout_ms);
    PXP._lastError = e;
    return e;
}

PXPError PXPClass::blit(const PXPSurface &src, const PXPSurface &dst)
{
    return op().source(src).output(dst).run();
}
void PXPClass::_isr()
{
    uint32_t stat = PXP_STAT;
    /* Clear IRQ AND the sticky AXI error bits (the `stat` snapshot above still
     * drives the latch below).  If only IRQ were cleared, a latched AXI error
     * would survive to the NEXT completion and be misreported on a good op -
     * the same fails-latent trap fixed in wait(). */
    PXP_STAT_CLR = PXP_STAT_IRQ | PXP_STAT_AXI_READ_ERROR | PXP_STAT_AXI_WRITE_ERROR;

    PXPClass *self = pxp_instance;
    if (!self) return;

    if (stat & PXP_STAT_AXI_READ_ERROR)       self->_lastError = PXP_ERR_AXI_READ;
    else if (stat & PXP_STAT_AXI_WRITE_ERROR) self->_lastError = PXP_ERR_AXI_WRITE;
    else                                      self->_lastError = PXP_OK;

    EventResponder *er = self->_responder;
    self->_responder = nullptr;
    if (er) er->triggerEvent((int)self->_lastError, self);
}

/* NOT static: PXP.h declares this at namespace scope and befriends that exact
 * declaration, so a static definition here would be a different function with
 * internal linkage and would not be the friend. */
void pxp_isr_trampoline(void) { PXPClass::_isr(); }

PXPError PXPOp::runAsync(EventResponder *onComplete)
{
    if (!PXP._begun) return PXP_ERR_NOT_BEGUN;
    if (PXP.busy())  return PXP_ERR_BUSY;

    PXPError e = _program();
    if (e != PXP_OK) { PXP._lastError = e; return e; }

    /* onComplete is fired from _isr() -> triggerEvent(), i.e. in the PXP IRQ 57
     * context.  An attachImmediate() callback (as the gate uses) therefore runs
     * at interrupt priority - fine for setting a flag, but a heavy callback
     * (Serial, malloc) would run in the ISR; a caller needing deferral to thread
     * context should use EventResponder::attach() instead. */
    PXP._responder = onComplete;

    attachInterruptVector(IRQ_PXP, pxp_isr_trampoline);
    NVIC_ENABLE_IRQ(IRQ_PXP);

    /* Clear a stale completion AND any latched AXI error before arming, so the
     * first ISR sees only this op's status. */
    PXP_STAT_CLR = PXP_STAT_IRQ | PXP_STAT_AXI_READ_ERROR | PXP_STAT_AXI_WRITE_ERROR;
    PXP_CTRL_SET = PXP_CTRL_IRQ_ENABLE;
    PXP_CTRL_SET = PXP_CTRL_ENABLE;
    return PXP_OK;
}

#endif /* __IMXRT1176__ */
