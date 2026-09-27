#!/usr/bin/env python3
"""A failed replacement retries after the old song finishes, without freeing live audio."""
from pathlib import Path
import subprocess
import tempfile
s=Path(__file__).with_name('audio_aicaflow.c').read_text()
a=s.index('static void collect_loaded_music(void) {')
body=s[a:s.index('\n}',a)+2]
program=r'''
#include <stdint.h>
#include <assert.h>
#define AFX_NO_AICA_RAM 6
#define AFX_ASSET_INVALID 0
#define DKR_SEQUENCE_NONE 0
typedef int afx_asset_t;
static int sLoadedMusicReady, sLoadedMusicFlow, sLoadedMusicResult;
static uint8_t sLoadedSequence, sPreparedSequence, sPrefetchSequence, sFailedSequence;
static int sMusic, sMusicFlow, sPrefetchedMusicFlow;
static uint8_t sMusicFlowSequence, sPrefetchedMusicSequence;
static int frees, busy, queued;
static int afx_asset_free(int flow) {
    assert(!sMusic);
    if(busy) return -1;
    ++frees; return 0;
}
static void music_load_enqueue(uint8_t seq, int prepared) {
    assert(prepared && !sLoadedMusicReady); queued=seq;
}
'''+body+r'''
int main(void) {
    sMusic=1; sMusicFlow=10; sMusicFlowSequence=15;
    sPreparedSequence=sLoadedSequence=34;
    sLoadedMusicReady=1; sLoadedMusicResult=-AFX_NO_AICA_RAM;
    collect_loaded_music();
    assert(!frees && !queued && !sFailedSequence && sLoadedMusicReady);
    sMusic=0; busy=1;
    collect_loaded_music();
    assert(!frees && !queued && sLoadedMusicReady);
    busy=0; collect_loaded_music();
    assert(frees==1 && queued==34 && !sMusicFlow && !sLoadedMusicReady && !sFailedSequence);
    queued=0; sLoadedMusicReady=1;
    collect_loaded_music(); /* Still too large: report failure, do not spin. */
    assert(!queued && sFailedSequence==34 && !sPreparedSequence && !sLoadedMusicReady);
    sFailedSequence=0; sPreparedSequence=34; sLoadedMusicReady=1;
    sLoadedMusicResult=0; sLoadedMusicFlow=20;
    collect_loaded_music();
    assert(sPrefetchedMusicFlow==20 && sPrefetchedMusicSequence==34 && !sFailedSequence);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory); (p/'check.c').write_text(program)
    subprocess.run(['cc','-std=c11',str(p/'check.c'),'-o',str(p/'check')],check=True)
    subprocess.run([str(p/'check')],check=True)
print('Music memory: live protection, delayed release, one retry and permanent failure passed')
