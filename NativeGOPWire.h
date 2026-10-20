// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual firmware scanout transfer, not a compositor or hardware vblank ABI.
#pragma once
#include <stdint.h>
enum { NGP_BGRX8888=1,NGP_VERSION=1 };
struct NGPMode {uint32_t version,format;uint64_t width,height,stride,bytes;};
struct NGPPresent {uint64_t address,bytes;uint32_t width,height,stride,format;};
struct NGPTransfer {uint64_t sequence,bytes;};
static_assert(sizeof(NGPMode)==40 && sizeof(NGPPresent)==32 && sizeof(NGPTransfer)==16,"GOP transfer ABI");
