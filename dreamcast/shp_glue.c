// sh4-hotlayout's PC sampler and PMU windows, compiled in by SHP=1 (Makefile.dc).
// enDjinn compiles every .c under dreamcast/, so the sampler's sources come in
// through this file from SHP_ROOT/sampler, which SHP=1 adds to the include path.
#if DKR_SHP
#include "shp_sampler.c"
#include "shp_pmu.c"
#endif
