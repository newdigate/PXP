/* PXP.h - i.MX RT1176 Pixel Pipeline (PXP) 2D accelerator
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Clean-room implementation written from the i.MX RT1170 Reference Manual
 * rev.5, chapter 52.  Not derived from NXP's fsl_pxp.c, nor from any other
 * vendor or GPL-licensed driver source.
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
 *   AS_CTRL[FORMAT]  bits [7:4]  - only 4 bits wide (Phase 3)
 * Translate with pxpPsFormat()/pxpOutFormat(), which return PXP_FMT_NA when
 * the format cannot be expressed in that role. */
enum PXPFormat : uint8_t {
    PXP_ARGB8888 = 0,   /* 32 bpp, alpha in the high byte          */
    PXP_XRGB8888,       /* 32 bpp, unpacked 24-bit, alpha ignored  */
    PXP_RGB565,         /* 16 bpp                                  */
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
    case PXP_RGB565:      return false;
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
