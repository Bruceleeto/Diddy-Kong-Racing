#!/usr/bin/env python3
"""Exercise the actual transition helpers with delayed AICA acknowledgements."""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path(__file__).with_name('audio_aicaflow.c').read_text()


def function(name):
    start = re.search(r'^(?:int|void) ' + name + r'\(', source, re.M).start()
    end = source.index('\n}', start) + 2
    return source[start:end]


program = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define DKR_SFX_SLOTS 2
#define DKR_SEQUENCE_NONE 0
#define DKR_SEQUENCE_NONE2 1
#define DKR_SEQUENCE_COUNT 66
#define AFX_OK 0
#define AFX_BUSY 1
#define AFX_UNSUPPORTED 2
#define AFX_BAD_FORMAT 3
static struct { int instance; } sSfxSlots[2];
static int attempts[3];
static int afx_update(void) { return 0; }
static void update_sfx(void) {}
static void thd_sleep(int ms);
static int afx_instance_stop(int id) {
    if (++attempts[id] < 3) return -AFX_BUSY;
    sSfxSlots[id - 1].instance = 0;
    return 0;
}
static int sReady = 1, sMusicFlow = 1, sPrefetchedMusicFlow = 2;
static uint8_t sMusicFlowSequence = 35, sPrefetchedMusicSequence = 31;
static uint8_t sPreparedSequence, sFailedSequence, queued;
static int short_music_flow(uint8_t seq) { return seq == 30; }
static void music_load_enqueue(uint8_t seq, int prepared) { queued = seq; }
'''
program += function('dkr_afx_sfx_stop_all')
program += function('dkr_afx_music_prepare')
program += r'''
static uint8_t sWantedSequence, sLoadPreparedSequence, sLoadingSequence;
static uint8_t sLoadedMusicReady, sLoadedSequence;
static int sleeps, promote_on_sleep, locked;
static void pc_audio_lock(void) { assert(!locked); locked = 1; }
static void pc_audio_unlock(void) { assert(locked); locked = 0; }
static void thd_sleep(int ms) {
    (void)ms;
    assert(!locked);
    assert(++sleeps < 10);
    if (promote_on_sleep) {
        sMusicFlow = sPrefetchedMusicFlow;
        sMusicFlowSequence = sPrefetchedMusicSequence;
        sPrefetchedMusicFlow = 0;
        sPrefetchedMusicSequence = 0;
        sPreparedSequence = 0;
    }
}
static int music_flow_for(uint8_t seq) {
    assert(locked);
    if (sMusicFlow && sMusicFlowSequence == seq) return sMusicFlow;
    if (sPrefetchedMusicFlow && sPrefetchedMusicSequence == seq) return sPrefetchedMusicFlow;
    return short_music_flow(seq);
}
static void collect_loaded_music(void) {
    assert(locked);
    if (!sPreparedSequence) return;
    sPrefetchedMusicFlow = 3;
    sPrefetchedMusicSequence = sPreparedSequence;
}
'''
program += function('dkr_afx_music_play')
program += r'''
int main(void) {
    sSfxSlots[0].instance = 1;
    sSfxSlots[1].instance = 2;
    assert(dkr_afx_sfx_stop_all() == 0);
    assert(!sSfxSlots[0].instance && !sSfxSlots[1].instance);
    assert(attempts[1] == 3 && attempts[2] == 3);
    assert(dkr_afx_music_prepare(31) == 0);
    assert(sPreparedSequence == 31 && !queued);
    assert(dkr_afx_music_prepare(35) == 0 && !sPreparedSequence);
    assert(dkr_afx_music_prepare(11) == 0 && queued == 11);
    assert(dkr_afx_music_prepare(30) == 0 && !sPreparedSequence);
    assert(dkr_afx_music_prepare(66) == -AFX_BAD_FORMAT);
    sleeps = 0;
    promote_on_sleep = 1;
    dkr_afx_music_play(7);
    assert(sWantedSequence == 7 && sMusicFlowSequence == 7);
    assert(!sPrefetchedMusicFlow && sleeps == 1 && !locked);
    dkr_afx_music_play(66);
    assert(sWantedSequence == 7 && !locked);
    dkr_afx_music_play(0);
    assert(!sWantedSequence && !locked);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'check.c').write_text(program)
    subprocess.run(['cc', '-std=c11', str(path / 'check.c'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
print('AICAFLOW transitions: pending stops and prepared music passed')
