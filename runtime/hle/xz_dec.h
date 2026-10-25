#ifndef AC3_XZ_DEC_H
#define AC3_XZ_DEC_H

#include "ac3_runtime.h"
#include <stddef.h>

int ac3_xz_decode(const u8 *in, size_t in_len, u8 *out, size_t out_len, size_t *produced);

#endif
