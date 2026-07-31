/* PXP.h - i.MX RT1176 Pixel Pipeline (PXP) 2D accelerator
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Clean-room implementation written from the i.MX RT1170 Reference Manual
 * rev.5, chapter 52.  Not derived from NXP's fsl_pxp.c, nor from any other
 * vendor or GPL-licensed driver source.
 *
 * Phases 1-2: fill/blit, placement, rotation, flips, pre-decimation, YUV
 * sources through CSC1, blocking + async completion.
 * Phase 3: alpha-surface (AS) compositing - overlay(), the four alpha modes
 * with invert, the 12-op ROP table, AS + PS colorkeys.  The contested blend
 * and colorkey semantics are silicon-measured, not RM-transcribed - see the
 * README's Phase 3 section and pxp_composite_test's transcript.
 */
#ifndef PXP_h_
#define PXP_h_

#if defined(__IMXRT1176__)

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

class EventResponder;

/* Abstract pixel-format tokens.  These are deliberately NOT hardware
 * encodings: the PXP programs the SAME format with DIFFERENT values depending
 * on which register it lands in, and not every format is legal in every role.
 *   PS_CTRL[FORMAT]  bits [5:0]  - has no 0x00 encoding
 *   OUT_CTRL[FORMAT] bits [4:0]  - has no 0x24 encoding
 *   AS_CTRL[FORMAT]  bits [7:4]  - only 4 bits wide
 * Translate with pxpPsFormat()/pxpOutFormat()/pxpAsFormat(), which return
 * PXP_FMT_NA when the format cannot be expressed in that role. */
enum PXPFormat : uint8_t {
    PXP_ARGB8888 = 0,   /* 32 bpp, alpha in the high byte          */
    PXP_XRGB8888,       /* 32 bpp, unpacked 24-bit, alpha ignored  */
    PXP_RGB565,         /* 16 bpp                                  */
    PXP_RGBA8888,       /* 32 bpp, alpha in the LOW byte.  AS/PS roles only:
                         * OUT_CTRL has no RGBA8888 encoding (RM 52.6.3), so
                         * pxpOutFormat() returns PXP_FMT_NA for it. */
    PXP_UYVY1P422,      /* 16 bpp YUV422, one plane, U Y V Y byte order.
                         * PS-only (a YUV surface is a valid source but never
                         * an output); the PS datapath's CSC1 converts it to the
                         * RGB output format.  Matches the OV5640's UYVY mode. */
    PXP_YUV1P444,       /* 32 bpp YUV444, one plane, unpacked XYUV in a 32-bit
                         * word (X:31-24, Y:23-16, U:15-8, V:7-0).  PS-only, runs
                         * through CSC1 like UYVY.  This is what the CSI stores
                         * when receiving MIPI YUV422 (24-bit bus -> XYUV8888). */
};

/* True for the YUV/YCbCr source formats, whose PS datapath must run through
 * CSC1 (colour convert) rather than bypass it.  Kept as a free function with
 * NO default: so a new YUV format added to PXPFormat is a compile error here
 * until classified. */
inline bool pxpIsYuv(PXPFormat f)
{
    switch (f) {
    case PXP_ARGB8888:
    case PXP_XRGB8888:
    case PXP_RGB565:
    case PXP_RGBA8888:    return false;
    case PXP_UYVY1P422:
    case PXP_YUV1P444:    return true;
    }
    return false;
}

/* Sentinel for "no encoding in this role".  Note it fails OPEN under masking
 * (0xFF & 0x3F == 0x3F, a reserved PS value), so callers must compare against
 * it BEFORE masking - which _program() does. */
static constexpr uint8_t PXP_FMT_NA = 0xFFu;

uint8_t  pxpPsFormat(PXPFormat f);    /* -> PS_CTRL[FORMAT]  */
uint8_t  pxpOutFormat(PXPFormat f);   /* -> OUT_CTRL[FORMAT] */
uint8_t  pxpAsFormat(PXPFormat f);    /* -> AS_CTRL[FORMAT]  */
uint16_t pxpBitsPerPixel(PXPFormat f);

enum PXPRotation : uint8_t {
    PXP_ROT_0 = 0, PXP_ROT_90 = 1, PXP_ROT_180 = 2, PXP_ROT_270 = 3,
};

/* PS pre-decimation (RM 52.3.1.3): drops source pixels to shrink the processed
 * surface by an integer factor per axis.  The values ARE the PS_CTRL DECX/DECY
 * encodings (0..3), and the factor is 1<<value.  Decimation cannot combine with
 * rotation (RM 52.3.4.1); scaling >2x uses this ahead of the bilinear filter. */
enum PXPDecim : uint8_t {
    PXP_DEC_1 = 0,   /* no decimation */
    PXP_DEC_2 = 1,
    PXP_DEC_4 = 2,
    PXP_DEC_8 = 3,
};

enum PXPAlphaMode : uint8_t {        /* AS_CTRL[ALPHA_CTRL], RM 52.6.22 */
    PXP_ALPHA_EMBEDDED = 0,          /* per-pixel alpha from the AS format   */
    PXP_ALPHA_OVERRIDE = 1,          /* AS_CTRL[ALPHA] replaces every pixel  */
    PXP_ALPHA_MULTIPLY = 2,          /* AS_CTRL[ALPHA] scales per-pixel      */
    PXP_ALPHA_ROPS     = 3,          /* the ROP table drives the ALU         */
};

enum PXPRop : uint8_t {              /* AS_CTRL[ROP], RM 52.6.22 */
    PXP_ROP_MASKAS = 0x0,  PXP_ROP_MASKNOTAS = 0x1, PXP_ROP_MASKASNOT = 0x2,
    PXP_ROP_MERGEAS = 0x3, PXP_ROP_MERGENOTAS = 0x4, PXP_ROP_MERGEASNOT = 0x5,
    PXP_ROP_NOTCOPYAS = 0x6, PXP_ROP_NOT = 0x7,
    PXP_ROP_NOTMASKAS = 0x8, PXP_ROP_NOTMERGEAS = 0x9,
    PXP_ROP_XORAS = 0xA,  PXP_ROP_NOTXORAS = 0xB,
};

enum PXPError : uint8_t {
    PXP_OK = 0,
    PXP_ERR_BUSY,          /* an operation is already running          */
    PXP_ERR_TIMEOUT,       /* STAT[IRQ] never asserted                 */
    PXP_ERR_CONFIG,        /* geometry does not fit / bad surface      */
    PXP_ERR_UNREACHABLE,   /* surface not visible to a bus master      */
    PXP_ERR_ALIGN,         /* rotate/flip on an unaligned window       */
    PXP_ERR_AXI_READ,      /* STAT AXI read error                      */
    PXP_ERR_AXI_WRITE,     /* STAT AXI write error                     */
    PXP_ERR_NOT_BEGUN,     /* begin() has not been called              */
    PXP_ERR_FORMAT,        /* format not valid in that PS/OUT role     */
    PXP_ERR_UNIMPLEMENTED, /* not yet implemented in this phase        */
};

struct PXPSurface {
    void      *data;
    uint16_t   width;
    uint16_t   height;
    uint16_t   pitch;      /* bytes per row of the RGB/luma plane; 0 = invalid */
    PXPFormat  format;

    PXPSurface(void *d, uint16_t w, uint16_t h, PXPFormat f,
               uint16_t pitchBytes = 0)
        : data(d), width(w), height(h), pitch(0), format(f)
    {
        /* Computed in 32 bits: a silent uint16_t truncation would turn an
         * illegal surface into a plausible-looking wrong one. */
        uint32_t minRow = ((uint32_t)w * pxpBitsPerPixel(f) + 7u) / 8u;
        uint32_t p = pitchBytes ? (uint32_t)pitchBytes : minRow;
        /* pitch 0 marks an unusable surface, which _program() rejects.  An
         * explicit pitch narrower than one row is a typo, not a stride. */
        pitch = (minRow > 0u && p >= minRow && p <= 0xFFFFu) ? (uint16_t)p : 0u;
    }

    uint16_t bitsPerPixel() const { return pxpBitsPerPixel(format); }
    /* 0 when the format is not a whole number of bytes (e.g. Y4, YUV420). */
    uint8_t  bytesPerPixel() const {
        uint16_t b = pxpBitsPerPixel(format);
        return (b != 0u && (b % 8u) == 0u) ? (uint8_t)(b / 8u) : 0u;
    }
    size_t   sizeBytes()    const { return (size_t)pitch * height; }
    bool     reachable()    const;   /* bus-master address-range check */
};

class PXPClass;

class PXPOp {
public:
    PXPOp &source(const PXPSurface &s)  { _src = &s; return *this; }
    PXPOp &output(const PXPSurface &o)  { _dst = &o; return *this; }
    /* A temporary would dangle once the full-expression ends; make it a
     * compile error rather than a silent use-after-free. */
    PXPOp &source(const PXPSurface &&) = delete;
    PXPOp &output(const PXPSurface &&) = delete;

    PXPOp &outputAt(uint16_t x, uint16_t y) { _x = x; _y = y; return *this; }
    PXPOp &background(uint32_t argb)    { _bg = argb; return *this; }
    PXPOp &rotate(PXPRotation r)        { _rot = r; return *this; }
    PXPOp &flip(bool h, bool v)         { _hflip = h; _vflip = v; return *this; }
    /* Shrink the source by 1/2, 1/4 or 1/8 per axis (pixel-drop).  The output
     * surface must be big enough for source/factor.  Not combinable with
     * rotate() (returns PXP_ERR_CONFIG), nor with a YUV source in one pass
     * (do CSC first, then decimate the RGB result). */
    PXPOp &decimate(PXPDecim x, PXPDecim y) { _decx = x; _decy = y; return *this; }

    /* Phase 3: composite an overlay (AS) onto the source.  Armed by
     * overlay(); an overlay* setter (or rop()) WITHOUT overlay() is a
     * forgotten .overlay(sprite) and fails with PXP_ERR_CONFIG - never a
     * silent plain blit.  When the AS is unarmed, _program() still writes the
     * full AS register set to its IDLE state (degenerate rect + colorkeys at
     * the never-true encoding) so a previous op can never leave the AS
     * half-armed.  sourceColorKey() programs the PS-side key but REQUIRES an
     * armed overlay: a PS key with no AS is RM-undefined and unmeasured, so
     * _program() rejects it (PXP_ERR_CONFIG).
     * Semantics measured on silicon (v8 transcript) -- see README Phase 3. */
    PXPOp &overlay(const PXPSurface &as)   { _as = &as; return *this; }
    PXPOp &overlay(const PXPSurface &&) = delete;   /* dangling-temporary guard */
    PXPOp &overlayAt(uint16_t x, uint16_t y)
        { _as_x = x; _as_y = y; _overlay_touched = true; return *this; }
    PXPOp &overlayAlpha(PXPAlphaMode m, uint8_t value = 0xFF, bool invert = false)
        { _alpha_mode = m; _alpha_value = value; _alpha_invert = invert;
          _overlay_touched = true; return *this; }
    PXPOp &overlayColorKey(uint32_t low, uint32_t high)
        { _as_key_low = low; _as_key_high = high; _as_key = true;
          _overlay_touched = true; return *this; }
    PXPOp &sourceColorKey(uint32_t low, uint32_t high)
        { _ps_key_low = low; _ps_key_high = high; _ps_key = true; return *this; }
    PXPOp &rop(PXPRop r)
        { _rop = r; _rop_set = true; _overlay_touched = true; return *this; }

    PXPError run(uint32_t timeout_ms = 100);
    PXPError runAsync(EventResponder *onComplete = nullptr);

private:
    friend class PXPClass;
    PXPOp() = default;        /* obtain one via PXP.op() */
    PXPError _program();      /* validate + write registers; no ENABLE */

    const PXPSurface *_src = nullptr;
    const PXPSurface *_dst = nullptr;
    uint16_t     _x = 0, _y = 0;
    uint32_t     _bg = 0;
    PXPRotation  _rot = PXP_ROT_0;
    PXPDecim     _decx = PXP_DEC_1, _decy = PXP_DEC_1;
    bool         _hflip = false, _vflip = false;
    bool         _fillOnly = false;   /* PS positioned outside the window */

    /* Phase 3 (AS) state - all inert until overlay() arms _as. */
    const PXPSurface *_as = nullptr;
    uint16_t     _as_x = 0, _as_y = 0;
    PXPAlphaMode _alpha_mode = PXP_ALPHA_EMBEDDED;
    uint8_t      _alpha_value = 0xFF;
    bool         _alpha_invert = false;
    bool         _as_key = false, _ps_key = false, _rop_set = false;
    /* Set by every overlay* setter (and rop()); _program() rejects an op
     * where these were called but overlay() was not - see the AS block. */
    bool         _overlay_touched = false;
    uint32_t     _as_key_low = 0, _as_key_high = 0;
    uint32_t     _ps_key_low = 0, _ps_key_high = 0;
    PXPRop       _rop = PXP_ROP_MASKAS;
};

void pxp_isr_trampoline(void);

class PXPClass {
public:
    bool     begin();
    void     end();
    PXPOp    op() { return PXPOp(); }

    /* Solid fill: every pixel of dst becomes rgb888, a 24-bit 0x00RRGGBB
     * colour (bits [31:24] reserved) converted to dst's format by hardware -
     * NOT an alpha format, despite the uint32_t width. */
    PXPError fill(const PXPSurface &dst, uint32_t rgb888);
    PXPError blit(const PXPSurface &src, const PXPSurface &dst);

    bool     busy() const;
    PXPError wait(uint32_t timeout_ms = 100);
    PXPError lastError() const { return _lastError; }

private:
    friend class PXPOp;
    friend void pxp_isr_trampoline(void);
    static void _isr();

    bool     _begun = false;
    volatile PXPError _lastError = PXP_OK;
    EventResponder *_responder = nullptr;
};

extern PXPClass PXP;

#endif /* __IMXRT1176__ */
#endif /* PXP_h_ */
