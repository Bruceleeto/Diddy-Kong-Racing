#ifndef _ASSERT_H_
#define _ASSERT_H_

#include "types.h"
#include <ultra64.h>
#include "macros.h"

// Renamed on PC targets: the host libc declares its own __assert with a
// different signature, and letting the two collide means a host assert()
// dispatches into this one with mismatched arguments.
#ifdef TARGET_PC
#define __assert __n64_assert
#endif

void __assert(const char* exp, const char* filename, int line);

#endif
