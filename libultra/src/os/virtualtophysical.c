#include "PR/os_internal.h"
#include "PR/R4300.h"
#include "PRinternal/osint.h"

u32 osVirtualToPhysical(void* addr) {
#ifdef TARGET_PC
    /* No MMU segments and no RCP on the host: "physical" addresses are handed to
     * the audio command-list interpreter and the gfx backend, which just
     * dereference them. Identity is the only mapping that works.
     * NB: without this the KSEG0/KSEG1 tests below fall through to __osProbeTLB,
     * which is a stub returning 0 - i.e. every audio buffer pointer becomes NULL. */
    return (u32) addr;
#else
    if (IS_KSEG0(addr)) {
        return K0_TO_PHYS(addr);
    } else if (IS_KSEG1(addr)) {
        return K1_TO_PHYS(addr);
    } else {
        return __osProbeTLB(addr);
    }
#endif
}
