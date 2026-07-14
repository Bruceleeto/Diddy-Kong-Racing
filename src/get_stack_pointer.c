#include "memory.h"

#ifdef __sgi
/**
 * Uses IDO specific -dollar compiler flag to get the current stack pointer.
 * Official Name: diCpuTraceCurrentStack
 */
StackInfo *stack_pointer(void) {
    return (StackInfo *) __$sp;
}
#else
/**
 * Uses a GCC builtin to get the current stack pointer, portable to any target.
 * Official Name: diCpuTraceCurrentStack
 */
StackInfo *stack_pointer(void) {
    return (StackInfo *) __builtin_frame_address(0);
}
#endif
