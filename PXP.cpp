/* PXP.cpp - i.MX RT1176 Pixel Pipeline driver
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 */
#include "PXP.h"

PXPClass PXP;

bool     PXPClass::begin()                 { return false; }
void     PXPClass::end()                   {}
bool     PXPClass::busy() const            { return false; }
PXPError PXPClass::wait(uint32_t)          { return PXP_ERR_TIMEOUT; }
PXPError PXPClass::fill(const PXPSurface &, uint32_t)        { return PXP_ERR_CONFIG; }
PXPError PXPClass::blit(const PXPSurface &, const PXPSurface &) { return PXP_ERR_CONFIG; }
bool     PXPSurface::reachable() const     { return false; }
PXPError PXPOp::_program()                 { return PXP_ERR_CONFIG; }
PXPError PXPOp::run(uint32_t)              { return PXP_ERR_CONFIG; }
PXPError PXPOp::runAsync(EventResponder *) { return PXP_ERR_CONFIG; }
