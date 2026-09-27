#!/usr/bin/env python3
"""Music must finish while the SFX worker waits on the game audio mutex."""
from pathlib import Path
import subprocess
import tempfile
s=Path(__file__).with_name('audio_aicaflow.c').read_text()
def function(name):
    a=s.index('static void *'+name+'(void *unused) {')
    return s[a:s.index('\n}',a)+2]
program=r'''
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#define AFX_ASSET_INVALID 0
#define DKR_SEQUENCE_NONE 0
typedef int afx_asset_t;
static uint8_t sPreparedSequence, sLoadPreparedSequence, sLoadPrefetchSequence;
static uint8_t sLoadingSequence, sLoadedSequence, sLoadedMusicReady;
static afx_asset_t sLoadedMusicFlow;
static int sLoadedMusicResult, entered, music_idle;
static pthread_mutex_t audio = PTHREAD_MUTEX_INITIALIZER;
static void thd_sleep(int ms) { __atomic_store_n(&music_idle,1,__ATOMIC_RELEASE); usleep(ms*1000); }
static void fallback_load(void) {
    __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
    pthread_mutex_lock(&audio); pthread_mutex_unlock(&audio);
}
static int music_flow_load(uint8_t sequence, afx_asset_t *flow) { *flow=42; return 0; }
'''+function('music_loader')+function('sfx_loader')+r'''
int main(void) {
    pthread_t music,sfx;
    pthread_mutex_lock(&audio);
    assert(!pthread_create(&sfx,0,sfx_loader,0));
    for(int i=0;i<500 && !__atomic_load_n(&entered,__ATOMIC_ACQUIRE);++i) usleep(1000);
    assert(__atomic_load_n(&entered,__ATOMIC_ACQUIRE));
    assert(!pthread_create(&music,0,music_loader,0));
    for(int i=0;i<500 && !__atomic_load_n(&music_idle,__ATOMIC_ACQUIRE);++i) usleep(1000);
    assert(__atomic_load_n(&music_idle,__ATOMIC_ACQUIRE));
    __atomic_store_n(&sPreparedSequence,31,__ATOMIC_RELEASE);
    __atomic_store_n(&sLoadPreparedSequence,31,__ATOMIC_RELEASE);
    for(int i=0;i<500 && !__atomic_load_n(&sLoadedMusicReady,__ATOMIC_ACQUIRE);++i) usleep(1000);
    assert(__atomic_load_n(&sLoadedMusicReady,__ATOMIC_ACQUIRE));
    assert(sLoadedMusicFlow==42 && sLoadedSequence==31 && !sLoadedMusicResult);
    pthread_mutex_unlock(&audio);
    pthread_cancel(music); pthread_cancel(sfx);
    pthread_join(music,0); pthread_join(sfx,0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory); (p/'test.c').write_text(program)
    subprocess.run(['cc','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=5)
print('Loader progress: music completes while SFX waits for game lock')
