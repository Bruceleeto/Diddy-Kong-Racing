#ifndef PC_SWAP_H
#define PC_SWAP_H

#include <stddef.h>
#include "types.h"

// In-place big-endian -> host byteswap helpers for loaded asset data.
void pc_swap16_buf(void *buf, u32 numBytes);
void pc_swap32_buf(void *buf, u32 numBytes);

// Swap a run of fields starting at `field`, addressed from the struct base.
//
// The obvious spelling, pc_swapN_buf(&p->field, nbytes), makes the pointed-to
// object just that one field as far as the compiler is concerned, so every
// span covering more than one field trips -Wstringop-overflow under LTO.
// Going through the base pointer plus an offset describes the same bytes
// against an object that is actually big enough.
#define PC_SWAP16_AT(p, field, nbytes) \
    pc_swap16_buf((u8 *) (p) + offsetof(typeof(*(p)), field), (nbytes))
#define PC_SWAP32_AT(p, field, nbytes) \
    pc_swap32_buf((u8 *) (p) + offsetof(typeof(*(p)), field), (nbytes))

#endif
