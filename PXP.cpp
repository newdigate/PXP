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
    PXP_STAT_CLR = PXP_STAT_IRQ;
    if (stat & PXP_STAT_AXI_READ_ERROR)  return PXP_ERR_AXI_READ;
    if (stat & PXP_STAT_AXI_WRITE_ERROR) return PXP_ERR_AXI_WRITE;
    return PXP_OK;
}
PXPError PXPClass::fill(const PXPSurface &, uint32_t)           { return PXP_ERR_UNIMPLEMENTED; }
PXPError PXPClass::blit(const PXPSurface &, const PXPSurface &) { return PXP_ERR_UNIMPLEMENTED; }
bool     PXPSurface::reachable() const     { return false; }
PXPError PXPOp::_program()                 { return PXP_ERR_UNIMPLEMENTED; }
PXPError PXPOp::run(uint32_t)              { return PXP_ERR_UNIMPLEMENTED; }
PXPError PXPOp::runAsync(EventResponder *) { return PXP_ERR_UNIMPLEMENTED; }

#endif /* __IMXRT1176__ */
