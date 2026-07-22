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
};

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
    bool         _hflip = false, _vflip = false;
    bool         _fillOnly = false;   /* PS positioned outside the window */
};

void pxp_isr_trampoline(void);

class PXPClass {
public:
    bool     begin();
    void     end();
    PXPOp    op() { return PXPOp(); }

    PXPError fill(const PXPSurface &dst, uint32_t argb);
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
