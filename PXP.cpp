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
 * value.  AS_CTRL[FORMAT] (RM 52.6.22) is a third namespace with its own
 * translator, pxpAsFormat(), below (Phase 3).
 * Verified against RM rev.5 52.6.3 (OUT_CTRL), 52.6.12 (PS_CTRL) and
 * 52.6.22 (AS_CTRL). */
uint8_t pxpPsFormat(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888:   return 0x04;   /* RGB888_ARGB8888 - PS has no 0x00 */
    case PXP_XRGB8888:   return 0x04;   /* same PS encoding as ARGB8888     */
    case PXP_RGB565:     return 0x0E;
    case PXP_RGBA8888:   return 0x24;   /* RM 52.6.12: alpha at the low byte */
    case PXP_UYVY1P422:  return 0x12;   /* RM 52.6.12 UYVY1P422 (1-plane)   */
    case PXP_YUV1P444:   return 0x10;   /* RM 52.6.12 YUV1P444 (32-bit XYUV)*/
    }
    return PXP_FMT_NA;
}

uint8_t pxpOutFormat(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888:   return 0x00;
    case PXP_XRGB8888:   return 0x04;   /* RGB888, unpacked 24-bit in 32 bits */
    case PXP_RGB565:     return 0x0E;
    case PXP_RGBA8888:   return PXP_FMT_NA;  /* no OUT encoding (RM 52.6.3)  */
    case PXP_UYVY1P422:  return PXP_FMT_NA;  /* YUV is a PS source only, never OUT */
    case PXP_YUV1P444:   return PXP_FMT_NA;  /* YUV is a PS source only, never OUT */
    }
    return PXP_FMT_NA;
}

/* AS_CTRL[FORMAT] (RM 52.6.22, [7:4]): the four AS formats Phase 3 supports.
 * The remaining six encodings of the namespace (ARGB1555, ARGB4444, RGBA5551,
 * RGBA4444, RGB555, RGB444) have no PXPFormat token yet and are deferred. */
uint8_t pxpAsFormat(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888:   return 0x0;   /* alpha in the high byte             */
    case PXP_RGBA8888:   return 0x1;   /* alpha in the low byte              */
    case PXP_XRGB8888:   return 0x4;   /* RGB888 unpacked; alpha := 0xFF     */
    case PXP_RGB565:     return 0xE;   /* alpha := 0xFF                      */
    case PXP_UYVY1P422:  return PXP_FMT_NA;  /* AS has no YUV datapath       */
    case PXP_YUV1P444:   return PXP_FMT_NA;  /* AS has no YUV datapath       */
    }
    return PXP_FMT_NA;
}

uint16_t pxpBitsPerPixel(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888:   return 32;
    case PXP_XRGB8888:   return 32;
    case PXP_RGB565:     return 16;
    case PXP_RGBA8888:   return 32;
    case PXP_UYVY1P422:  return 16;   /* 2 bytes/pixel packed (U Y / V Y)   */
    case PXP_YUV1P444:   return 32;   /* 4 bytes/pixel unpacked XYUV         */
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

    bool srcIsYuv = !_fillOnly && pxpIsYuv(_src->format);

    /* Pre-decimation (RM 52.3.1.3): pixel-drop shrink by 1<<DEC per axis.
     * Constraints: RM 52.3.4.1 forbids rotate+decimate; the YUV422 chroma
     * adjustment is not modelled here, so a YUV source must be CSC'd first then
     * decimated as RGB (two passes).  Require a whole-multiple frame so the last
     * output pixel maps to a real source pixel. */
    uint16_t fx = (uint16_t)(1u << (uint8_t)_decx);
    uint16_t fy = (uint16_t)(1u << (uint8_t)_decy);
    bool decimating = (_decx != PXP_DEC_1 || _decy != PXP_DEC_1);
    if (decimating && _rot != PXP_ROT_0) return PXP_ERR_CONFIG;
    if (decimating && srcIsYuv)          return PXP_ERR_CONFIG;
    if (ps_w % fx || ps_h % fy)          return PXP_ERR_CONFIG;

    /* Content dims the output stage lays out: the decimated frame (no rotation)
     * or the rotated source extent (no decimation - they never coexist). */
    uint16_t cw = ps_w / fx, ch = ps_h / fy;
    uint16_t out_w = cw, out_h = ch;
    if (_rot == PXP_ROT_90 || _rot == PXP_ROT_270) {
        uint16_t t = out_w; out_w = out_h; out_h = t;
    }
    if ((uint32_t)_x + out_w > _dst->width ||
        (uint32_t)_y + out_h > _dst->height) return PXP_ERR_CONFIG;

    /* Same-format copies are always legal.  The one cross-format case allowed
     * is a YUV source into an RGB output: CSC1 does the colour convert in the
     * PS datapath (armed below).  Any other format mismatch would copy raw
     * bytes the destination then misreads, so it stays rejected.  (A YUV
     * OUTPUT is impossible anyway - pxpOutFormat() returns NA for it.) */
    if (!_fillOnly && _src->format != _dst->format) {
        if (!(srcIsYuv && !pxpIsYuv(_dst->format))) return PXP_ERR_FORMAT;
    }

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
    /* OUT_LRC is the laid-out content frame: the decimated dims (cw,ch), which
     * equal the source dims when not decimating.  For rotation the model reads
     * OUT_LRC as the pre-rotation source frame (decimation excluded), so cw,ch
     * are still right there. */
    PXP_OUT_CTRL   = (uint32_t)out_fmt & PXP_OUT_FORMAT_MASK;
    PXP_OUT_BUF    = out_buf;
    PXP_OUT_PITCH  = _dst->pitch;
    PXP_OUT_LRC    = PXP_COORD(cw - 1, ch - 1);

    PXP_PS_BACKGROUND = _bg;

    if (_fillOnly) {
        /* Degenerate PS rectangle => every output pixel is PS_BACKGROUND. */
        PXP_OUT_PS_ULC = PXP_COORD(1, 1);
        PXP_OUT_PS_LRC = PXP_COORD(0, 0);
    } else {
        /* PS_CTRL[11:10]=DECX, [9:8]=DECY, [5:0]=FORMAT. */
        PXP_PS_CTRL  = ((uint32_t)ps_fmt & PXP_PS_FORMAT_MASK)
                     | ((uint32_t)_decx << 10) | ((uint32_t)_decy << 8);
        PXP_PS_BUF   = (uint32_t)_src->data;
        PXP_PS_PITCH = _src->pitch;
        PXP_OUT_PS_ULC = PXP_COORD(0, 0);
        PXP_OUT_PS_LRC = PXP_COORD(cw - 1, ch - 1);
    }

    /* === AS (Phase 3) - written EVERY op, armed or idle =================== */
    if (_as) {
        uint8_t as_fmt = pxpAsFormat(_as->format);
        if (as_fmt == PXP_FMT_NA)           return PXP_ERR_FORMAT;
        if (!_as->data || _as->pitch == 0 || _as->bytesPerPixel() == 0 ||
            _as->width == 0 || _as->height == 0)
            return PXP_ERR_CONFIG;
        if (!_as->reachable())              return PXP_ERR_UNREACHABLE;
        if (_rot != PXP_ROT_0 || decimating)
            return PXP_ERR_CONFIG;          /* v8 measures ROT_0 compositing only */
        if (_rop_set != (_alpha_mode == PXP_ALPHA_ROPS))
            return PXP_ERR_CONFIG;          /* rop() iff Rops mode */
        /* AS rect must sit inside the output extent (shared coordinate space
         * with OUT_PS_ULC/LRC -- outputAt has already retargeted OUT_BUF). */
        if ((uint32_t)_as_x + _as->width  > out_w ||
            (uint32_t)_as_y + _as->height > out_h)
            return PXP_ERR_CONFIG;
        PXP_AS_BUF    = (uint32_t)_as->data;
        PXP_AS_PITCH  = _as->pitch;
        PXP_AS_CTRL   = (((uint32_t)as_fmt & PXP_AS_FORMAT_MASK) << PXP_AS_FORMAT_SHIFT)
                      | ((uint32_t)_alpha_mode << 1)
                      | ((uint32_t)_alpha_value << 8)
                      | (_as_key ? (1u << 3) : 0)
                      | (_rop_set ? ((uint32_t)_rop << 16) : 0)
                      | (_alpha_invert ? (1u << 20) : 0);
        PXP_OUT_AS_ULC = PXP_COORD(_as_x, _as_y);
        PXP_OUT_AS_LRC = PXP_COORD(_as_x + _as->width - 1,
                                   _as_y + _as->height - 1);
        PXP_AS_CLRKEYLOW  = _as_key ? _as_key_low  : 0x00FFFFFFu;
        PXP_AS_CLRKEYHIGH = _as_key ? _as_key_high : 0x00000000u;
    } else {
        if (_rop_set)                       return PXP_ERR_CONFIG;
        PXP_AS_CTRL       = 0;
        PXP_OUT_AS_ULC    = 0xFFFFFFFFu;    /* degenerate: ULC > LRC = disarmed */
        PXP_OUT_AS_LRC    = 0x00000000u;
        PXP_AS_CLRKEYLOW  = 0x00FFFFFFu;    /* never-true key range (RM 52.3.1.13) */
        PXP_AS_CLRKEYHIGH = 0x00000000u;
    }
    PXP_PS_CLRKEYLOW  = _ps_key ? _ps_key_low  : 0x00FFFFFFu;
    PXP_PS_CLRKEYHIGH = _ps_key ? _ps_key_high : 0x00000000u;

    /* CSC1: for a YUV source, run the PS datapath through the YUV->RGB matrix;
     * for an RGB source (or fill), bypass it.  The block resets NOT-bypassed
     * with the YUV->RGB coefficients loaded, so an RGB op MUST set BYPASS or
     * the copy is colour-mangled (silicon only - QEMU does not model CSC1).
     * A prior RGB op clobbered COEF0 with the BYPASS bit, so the YUV path
     * restores all three coefficients, not just clears BYPASS. */
    if (srcIsYuv) {
        PXP_CSC1_COEF0 = PXP_CSC1_COEF0_YUV2RGB;   /* BYPASS clear, C0 loaded */
        PXP_CSC1_COEF1 = PXP_CSC1_COEF1_YUV2RGB;
        PXP_CSC1_COEF2 = PXP_CSC1_COEF2_YUV2RGB;
    } else {
        PXP_CSC1_COEF0 = PXP_CSC1_BYPASS;
    }

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
