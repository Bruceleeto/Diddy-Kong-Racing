#!/usr/bin/env python3
"""A waiting bank must not block ready sounds or follow preempted list entries."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parent.parent/'src/audiosfx.c').read_text()
a=s.index('void sndp_aicaflow_retry_pending(void) {')
body=s[a:s.index('\n}',a)+2]
program=r'''
#include <assert.h>
#define OS_IM_NONE 0
#define SOUND_STATE_WAIT_VOICE 1
#define SOUND_FLAG_LOOPED 1
#define SOUND_FLAG_RETRIGGER 2
#define AFX_BUSY 1
#define AFX_NO_EXEC_BUDGET 2
typedef int OSIntMask;
typedef struct ALSoundState {
    struct ALSoundState *next;
    int state, aicaflowId, priority, flags, retries;
} ALSoundState;
static struct { ALSoundState *allocHead; } gSoundStateLists;
static int gNumActiveSounds;
static struct { int maxActiveSounds; } player = {10}, *gSoundPlayerPtr = &player;
static ALSoundState first, victim, last;
static int seen, preempt;
static OSIntMask osSetIntMask(int mask) { return 0; }
static void sndp_aicaflow_started(ALSoundState *state) { state->state=2; }
static void sndp_deallocate(ALSoundState *state) { assert(0); }
static int dkr_afx_sfx_play(int id, void *owner, int priority) {
    assert(owner!=&victim); ++seen;
    if(owner==&first) {
        if(preempt) first.next=&last;
        return -AFX_BUSY;
    }
    return 0;
}
'''+s[s.index('int sndp_aicaflow_voice_available(void *owner) {'):s.index('\n}',s.index('int sndp_aicaflow_voice_available(void *owner) {'))+2]+body+r'''
int main(void) {
    assert(sndp_aicaflow_voice_available(&first));
    gNumActiveSounds = 10;
    assert(!sndp_aicaflow_voice_available(&first));
    first.flags = SOUND_FLAG_RETRIGGER;
    assert(sndp_aicaflow_voice_available(&first));
    first=(ALSoundState){.next=&last,.state=1};
    last=(ALSoundState){.state=1}; gSoundStateLists.allocHead=&first;
    sndp_aicaflow_retry_pending();
    assert(seen==2 && last.state==2 && first.state==1);
    seen=0; preempt=1; last.state=1;
    first.next=&victim; victim.next=&last; victim.state=1;
    sndp_aicaflow_retry_pending();
    assert(seen==2 && last.state==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)
    (p/'test.c').write_text(program)
    subprocess.run(['cc',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('Pending sounds: independent progress and preempted list entry passed')
