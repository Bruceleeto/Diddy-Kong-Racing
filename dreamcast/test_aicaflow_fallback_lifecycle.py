#!/usr/bin/env python3
"""Exercise fallback ownership, deferred I/O and scene cancellation."""
from pathlib import Path
import subprocess
import tempfile
source = Path(__file__).with_name('audio_aicaflow.c').read_text()
def function(signature):
    start = source.index(signature + ' {')
    return source[start:source.index('\n}', start) + 2]
program = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define DKR_ASSET_MOUNT "/pc"
#define DKR_SFX_SLOTS 16
#define AFX_ASSET_INVALID 0
#define DKR_SEQUENCE_NONE 0
#define AFX_OK 0
#define AFX_BUSY 1
#define AFX_BAD_COMMAND 2
#define AFX_NO_EXEC_BUDGET 3
#define AFX_NO_AICA_RAM 4
typedef struct { int header; } afx_sfx_bank_t;
static struct { int instance; const afx_sfx_bank_t *bank; } sSfxSlots[DKR_SFX_SLOTS];
static int locked, loads, releases, cancel, load_result, fail_once;
static int sPrefetchedMusicFlow, freed_music;
static uint8_t sPrefetchedMusicSequence, sPreparedSequence;
static int afx_asset_free(int flow) { assert(locked); freed_music=flow; return 0; }
static uint16_t sSceneLevel=18, pending;
static int sndp_aicaflow_bank_pending(uint16_t id) { return id && id==pending; }
static int fallback_clear(void);
static int fallback_request(uint16_t id);
static void pc_audio_lock(void) { assert(!locked); locked=1; }
static void pc_audio_unlock(void) { assert(locked); locked=0; }
static int afx_sfx_bank_release(afx_sfx_bank_t *bank) {
    assert(locked);
    if (bank->header) ++releases;
    bank->header=0; return 0;
}
static int afx_sfx_bank_load_file(afx_sfx_bank_t *bank, const char *path) {
    assert(!locked); ++loads;
    if (fail_once) { fail_once=0; return -AFX_NO_AICA_RAM; }
    if (cancel) {
        pc_audio_lock(); assert(!fallback_clear());
        if (cancel == 2) assert(fallback_request(44)==-AFX_BUSY);
        pc_audio_unlock();
    }
    if (!load_result) bank->header=1;
    return load_result;
}
'''
a=source.index('#define DKR_FALLBACK_SLOTS')
b=source.index('static uint32_t sFallbackGeneration;',a)+len('static uint32_t sFallbackGeneration;')
program+=source[a:b]
for signature in ('static int fallback_bank_active(const afx_sfx_bank_t *bank)',
                  'static int fallback_clear(void)', 'static int fallback_request(uint16_t id)',
                  'static void fallback_load(void)'):
    program+=function(signature)
program+=r'''
int main(void) {
    pc_audio_lock();
    assert(fallback_request(0)==-AFX_BAD_COMMAND);
    assert(fallback_request(785)==-AFX_BAD_COMMAND);
    assert(fallback_request(7)==-AFX_BUSY && !loads);
    assert(fallback_request(7)==-AFX_BUSY);
    pc_audio_unlock(); fallback_load(); pc_audio_lock();
    assert(loads==1 && sFallback[0].bank.header);
    pending=7;
    assert(fallback_request(22)==-AFX_BUSY && sFallback[1].id==22);
    sFallback[1].id=sFallback[1].requested=0;
    pending=0;
    sSfxSlots[0].instance=1; sSfxSlots[0].bank=&sFallback[0].bank;
    assert(fallback_request(20)==-AFX_BUSY);
    assert(sFallback[0].id==7 && sFallback[1].id==20);
    pc_audio_unlock();
    cancel=1; sSfxSlots[0].instance=0;
    fallback_load();
    assert(!sFallback[1].id && !sFallback[1].bank.header && releases==2);
    cancel=0; load_result=-9;
    pc_audio_lock(); assert(fallback_request(21)==-AFX_BUSY); pc_audio_unlock();
    fallback_load(); pc_audio_lock();
    assert(fallback_request(21)==-9);
    /* Memory pressure must not blacklist a sound for the rest of the scene. */
    assert(!fallback_clear());
    assert(fallback_request(24)==-AFX_BUSY);
    pc_audio_unlock(); load_result=-AFX_NO_AICA_RAM; fallback_load(); pc_audio_lock();
    assert(fallback_request(24)==-AFX_NO_AICA_RAM);
    assert(fallback_request(24)==-AFX_BUSY);
    pc_audio_unlock(); load_result=0; fallback_load(); pc_audio_lock();
    assert(sFallback[0].id==24 && sFallback[0].bank.header);
    assert(!fallback_clear());
    assert(fallback_request(21)==-AFX_BUSY); /* New scene permits retry. */
    pc_audio_unlock();
    load_result=0; fail_once=1;
    sPrefetchedMusicFlow=99; sPrefetchedMusicSequence=34;
    sFallback[2].bank.header=1; sFallback[2].id=30;
    fallback_load();
    assert(sFallback[0].bank.header && !sFallback[2].bank.header);
    assert(freed_music==99 && !sPrefetchedMusicFlow && !sPrefetchedMusicSequence);
    pc_audio_lock(); assert(!fallback_clear());
    assert(fallback_request(22)==-AFX_BUSY); pc_audio_unlock();
    sPrefetchedMusicFlow=100; sPrefetchedMusicSequence=sPreparedSequence=34;
    freed_music=0; fail_once=1;
    fallback_load();
    assert(!freed_music && sPrefetchedMusicFlow==100);
    pc_audio_lock();
    assert(fallback_request(23)==-AFX_BUSY);
    assert(sFallback[0].id==22 && sFallback[0].bank.header);
    assert(sFallback[1].id==23); /* Fill an empty slot before evicting cached audio. */
    assert(!fallback_clear());
    assert(fallback_request(43)==-AFX_BUSY);
    pc_audio_unlock();
    /* Change scenes during the OOM retry and queue a sound for the new scene. */
    fail_once=1; cancel=2; fallback_load(); cancel=0;
    assert(!sFallback[0].id && !sFallback[0].bank.header);
    assert(sFallback[1].id==44 && sFallback[1].requested);
    fallback_load();
    assert(sFallback[1].id==44 && sFallback[1].bank.header);
    pc_audio_lock();
    assert(fallback_request(43)==-AFX_BUSY); /* Old ID may be requested afresh. */
    assert(!fallback_clear());
    /* A burst filling the load queue must wait without consuming voice retries. */
    for(unsigned i=0;i<DKR_FALLBACK_SLOTS;i++)
        assert(fallback_request(100+i)==-AFX_BUSY);
    assert(fallback_request(200)==-AFX_BUSY);
    assert(!fallback_clear());
    assert(fallback_request(200)==-AFX_BUSY && sFallback[0].id==200);
    pc_audio_unlock();
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'test.c').write_text(program)
    subprocess.run(['cc', '-std=c11', str(path/'test.c'), '-o', str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Fallback lifecycle: active ownership, unlocked I/O, scene cancellation and errors passed')
