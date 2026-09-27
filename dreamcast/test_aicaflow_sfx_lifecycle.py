#!/usr/bin/env python3
"""Stops survive a busy activation; preempted instances keep their slot."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('audio_aicaflow.c').read_text()
def function(signature):
    start = source.index(signature + ' {')
    return source[start:source.index('\n}', start) + 2]

program = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#define DKR_SFX_SLOTS 2
#define AFX_RUNNING 1
#define AFX_PARKED 2
#define AFX_DONE 3
#define AFX_ERROR 4
#define AFX_OK 0
#define AFX_BAD_COMMAND 5
#define AFX_NO_EXEC_BUDGET 6
#define AFX_NO_CHANNELS 7
#define AFX_BUSY 8
#define DKR_SFX_DIRTY_VOLUME 1
#define DKR_SFX_DIRTY_PITCH 2
#define DKR_SFX_DIRTY_PAN 4
#define DKR_SFX_DIRTY_FX 8
typedef struct { int state; } afx_instance_status_t;
typedef struct { int *flows, *sounds; } afx_sfx_bank_t;
typedef struct {
    void *owner;
    int instance, initializing, stopping, dirty, volume, pitch, pan, fx, priority;
    const afx_sfx_bank_t *bank;
    const int *sound;
} dkr_sfx_slot_t;
static dkr_sfx_slot_t sSfxSlots[2];
static int sReady = 1, stops, completes, controls, activations;
static int state = AFX_RUNNING, voice_available = 1;
static int sndp_aicaflow_voice_available(void *owner) { return voice_available; }
static int afx_instance_stop(int id) { return ++stops < 3 ? -1 : 0; }
static int afx_instance_status(int id, afx_instance_status_t *out) { out->state = state; return 0; }
static int afx_instance_recycle(int id) { return 0; }
static void apply_sfx_controls(dkr_sfx_slot_t *slot) { ++controls; }
static void sndp_aicaflow_complete(void *owner) { ++completes; }
static const afx_sfx_bank_t *sfx_bank_for(uint16_t id, uint32_t *index) {
    static int data;
    static afx_sfx_bank_t bank = {&data, &data};
    *index = 0;
    return &bank;
}
static int fallback_request(uint16_t id) { return -AFX_BAD_COMMAND; }
static void *preempted;
static void sndp_aicaflow_preempt(void *owner) { preempted = owner; }
static int afx_instance_activate(int flow, int *out) { ++activations; *out = 3; return 0; }
'''
for signature in ('static int sfx_preempt(uint8_t priority)',
                  'static dkr_sfx_slot_t *sfx_slot(void *owner)',
                  'void dkr_afx_sfx_stop(void *owner)',
                  'static void update_sfx(void)',
                  'int dkr_afx_sfx_play(uint16_t id, void *owner, uint8_t priority)'):
    program += function(signature)
program += r'''
int main(void) {
    int owner;
    sSfxSlots[0].instance = 1;
    sSfxSlots[0].owner = &owner;
    dkr_afx_sfx_stop(&owner);
    assert(stops == 1 && sSfxSlots[0].stopping);
    update_sfx(); update_sfx();
    assert(stops == 3 && controls == 0);
    state = AFX_DONE;
    update_sfx();
    assert(completes == 1 && !sSfxSlots[0].instance);
    /* A preempted owner is gone before its hardware instance is recycled. */
    sSfxSlots[0].instance = 1;
    sSfxSlots[1].instance = 2;
    assert(dkr_afx_sfx_play(1, &owner, 0) == -AFX_NO_EXEC_BUDGET);
    assert(activations == 0 && sSfxSlots[0].instance == 1);
    int lower, higher;
    sSfxSlots[0].owner = &higher; sSfxSlots[0].priority = 10;
    sSfxSlots[1].owner = &lower; sSfxSlots[1].priority = 2;
    assert(dkr_afx_sfx_play(1, &owner, 1) == -AFX_NO_EXEC_BUDGET);
    assert(!preempted && activations == 0);
    assert(dkr_afx_sfx_play(1, &owner, 5) == -AFX_BUSY);
    assert(preempted == &lower && !sSfxSlots[1].owner);
    assert(sSfxSlots[0].owner == &higher && sSfxSlots[1].instance == 2);
    assert(activations == 0);
    update_sfx();
    assert(dkr_afx_sfx_play(1, &owner, 0) == 0 && activations == 1);
    voice_available = 0;
    preempted = NULL;
    assert(dkr_afx_sfx_play(1, &higher, 5) == -AFX_BUSY);
    assert(preempted == &owner && activations == 1);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'check.c').write_text(program)
    subprocess.run(['cc', '-std=c11', str(path / 'check.c'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
print('AICAFLOW SFX lifecycle: stop retries, deferred reuse and priority at both limits passed')
