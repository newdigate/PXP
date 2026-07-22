/* PXP.h - i.MX RT1176 Pixel Pipeline (PXP) 2D accelerator
 *
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Clean-room implementation from the i.MX RT1170 Reference Manual rev.5,
 * chapter 52.  No vendor driver source is derived from.
 */
#ifndef PXP_h_
#define PXP_h_

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#if !defined(__IMXRT1176__)
#error "The PXP library targets the i.MX RT1176 (__IMXRT1176__)"
#endif

class EventResponder;

/* Enumerator values ARE the hardware FORMAT encodings (PS_CTRL/OUT_CTRL,
 * field mask 0x3F).  Phase 1 uses the RGB subset; Phases 2/5 extend this. */
enum PXPFormat : uint8_t {
    PXP_ARGB8888 = 0x00,
    PXP_XRGB8888 = 0x04,
    PXP_RGB565   = 0x0E,
};

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
};

struct PXPSurface {
    void      *data;
    uint16_t   width;
    uint16_t   height;
    uint16_t   pitch;      /* bytes per row; 0 in ctor => width*bpp */
    PXPFormat  format;

    PXPSurface(void *d, uint16_t w, uint16_t h, PXPFormat f,
               uint16_t pitchBytes = 0)
        : data(d), width(w), height(h),
          pitch(pitchBytes ? pitchBytes
                           : (uint16_t)(w * _bpp(f))), format(f) {}

    uint8_t bytesPerPixel() const { return _bpp(format); }
    size_t  sizeBytes()     const { return (size_t)pitch * height; }
    bool    reachable()     const;   /* bus-master address-range check */

    static uint8_t _bpp(PXPFormat f) { return (f == PXP_RGB565) ? 2 : 4; }
};

class PXPClass;

class PXPOp {
public:
    PXPOp &source(const PXPSurface &s)  { _src = &s; return *this; }
    PXPOp &output(const PXPSurface &o)  { _dst = &o; return *this; }
    PXPOp &at(uint16_t x, uint16_t y)   { _x = x; _y = y; return *this; }
    PXPOp &background(uint32_t argb)    { _bg = argb; return *this; }
    PXPOp &rotate(PXPRotation r)        { _rot = r; return *this; }
    PXPOp &flip(bool h, bool v)         { _hflip = h; _vflip = v; return *this; }

    PXPError run(uint32_t timeout_ms = 100);
    PXPError runAsync(EventResponder *onComplete = nullptr);

private:
    friend class PXPClass;
    PXPError _program();      /* validate + write registers; no ENABLE */

    const PXPSurface *_src = nullptr;
    const PXPSurface *_dst = nullptr;
    uint16_t     _x = 0, _y = 0;
    uint32_t     _bg = 0;
    PXPRotation  _rot = PXP_ROT_0;
    bool         _hflip = false, _vflip = false;
    bool         _fillOnly = false;   /* PS positioned outside the window */
};

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
    friend void pxp_isr(void);
    static void _isr();

    bool     _begun = false;
    volatile PXPError _lastError = PXP_OK;
    EventResponder *_responder = nullptr;
};

extern PXPClass PXP;

#endif
