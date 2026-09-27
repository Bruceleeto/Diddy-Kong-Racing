#!/usr/bin/env python3
"""Local scene preload must preserve fallback room and retain resident music."""
from pathlib import Path
import subprocess
import tempfile
s=Path(__file__).with_name('audio_aicaflow.c').read_text()
a=s.index('int dkr_afx_sfx_scene_prepare(uint16_t level) {')
body=s[a:s.index('\n}',a)+2]
program=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define AFX_OK 0
#define AFX_BUSY 1
#define AFX_NO_AICA_RAM 2
#define AFX_BAD_FORMAT 3
static int sReady=1, sVehicleMask;
static uint16_t sSceneLevel=UINT16_MAX;
static struct bank { int header; unsigned bytes; } sSceneSfx, sVehicleSfx;
static const unsigned dkr_afx_scene_bytes[]={40000,40000,0};
typedef struct { unsigned largest_free_block; } afx_mem_stats_t;
static unsigned free_bytes, local_loads, vehicle_loads, stops;
static int load_error;
static const char *sfx_scene_path(uint16_t level, char *path) { return level<2 ? "scene" : 0; }
static const char *sfx_vehicle_path(uint16_t level) { return level<2 ? "vehicle" : 0; }
static int dkr_afx_sfx_stop_all(void) { ++stops; return 0; }
static int fallback_clear(void) { return 0; }
static int afx_sfx_bank_release(struct bank *bank) { bank->header=0; return 0; }
static int afx_mem_stats(afx_mem_stats_t *out) { out->largest_free_block=free_bytes; return 0; }
static int afx_sfx_bank_load_file(struct bank *bank, const char *path) {
    if(bank==&sSceneSfx) {
        assert(sVehicleMask); ++local_loads;
        if(load_error) return load_error;
    }
    else { assert(bank==&sVehicleSfx); ++vehicle_loads; }
    bank->header=1; return 0;
}
'''+body+r'''
int main(void) {
    free_bytes=40000+65536;
    assert(!dkr_afx_sfx_scene_prepare(0));
    assert(local_loads==1 && vehicle_loads==1 && sSceneLevel==0);
    assert(!dkr_afx_sfx_scene_prepare(0) && stops==1);
    free_bytes--;
    assert(!dkr_afx_sfx_scene_prepare(1));
    assert(!sSceneSfx.header && sVehicleSfx.header && local_loads==1);
    assert(vehicle_loads==1 && sSceneLevel==1);
    assert(!dkr_afx_sfx_scene_prepare(2));
    assert(!sVehicleSfx.header && !sVehicleMask && sSceneLevel==2);
    /* Music can claim the measured space before the scene allocation. */
    free_bytes=40000+65536;
    load_error=-AFX_NO_AICA_RAM;
    assert(!dkr_afx_sfx_scene_prepare(0));
    assert(sSceneLevel==0 && !sSceneSfx.header && sVehicleSfx.header);
    load_error=-AFX_BAD_FORMAT;
    assert(dkr_afx_sfx_scene_prepare(1)==-AFX_BAD_FORMAT);
    assert(sSceneLevel!=1);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.c').write_text(program)
    subprocess.run(['cc',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Scene budget: reserve, allocation race, format errors and vehicle lifecycle passed')
