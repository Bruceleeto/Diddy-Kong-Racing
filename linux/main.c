#include <stdio.h>
#include <ultra64.h>

// The game's main-thread entry (src/thread3_main.c): init_game() + the
// main_game_loop() forever-loop. Called directly on the host thread, same as
// the OoT DC port calling Main()/Graph_ThreadEntry directly — the N64 boot
// chain (mainproc -> thread1 -> thread3) is skipped entirely, so the libultra
// thread machinery is never needed.
void thread3_main(void *unused);

// libultra code asks about "the current thread" (osGetThreadPri(NULL) etc.).
// There is no thread system on PC, so the host thread poses as one: thread 3
// at its real priority.
extern OSThread *__osRunningThread;
static OSThread sHostThread;

// ---------------------------------------------------------------------------
// Graphics task — entry point of the F3DDKR HLE renderer (this file will grow
// it, like the OoT DC port's src/dreamcast/main.c). Called per frame from
// gfxtask_run_xbus (src/rcp_dkr.c) with the display list, instead of the RSP.
// For now: count frames and prove the game loop is alive.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Frame pacing — stands in for the VI retrace interrupt. Sleeps to the next
// 60Hz boundary and returns how many retrace periods passed since last call
// (the game uses that count as its update rate / frame-skip signal).
// ---------------------------------------------------------------------------
#include <time.h>

#define PC_RETRACE_NSEC (1000000000ll / 60)

s32 pc_retrace_wait(void) {
    static long long sLastNs = 0;
    struct timespec ts;
    long long nowNs;
    s32 periods;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    nowNs = (long long) ts.tv_sec * 1000000000ll + ts.tv_nsec;

    if (sLastNs == 0) {
        sLastNs = nowNs;
        return 1;
    }

    // Sleep until the next retrace boundary if we're early.
    if (nowNs - sLastNs < PC_RETRACE_NSEC) {
        long long targetNs = sLastNs + PC_RETRACE_NSEC;
        struct timespec req;
        req.tv_sec = (targetNs - nowNs) / 1000000000ll;
        req.tv_nsec = (targetNs - nowNs) % 1000000000ll;
        nanosleep(&req, NULL);
        nowNs = targetNs;
    }

    periods = (s32) ((nowNs - sLastNs) / PC_RETRACE_NSEC);
    if (periods < 1) {
        periods = 1;
    }
    if (periods > 6) { // don't let a debugger pause turn into a huge skip
        periods = 6;
    }
    sLastNs = nowNs;
    return periods;
}

static u32 sGfxFrameCount = 0;

void pc_gfx_task_submit(void *dlBegin, void *dlEnd) {
    sGfxFrameCount++;
    if ((sGfxFrameCount % 60) == 1) {
        printf("GFX: frame %u, display list %u bytes\n", sGfxFrameCount,
               (u32) ((u8 *) dlEnd - (u8 *) dlBegin));
    }
}

int main(int argc, char **argv) {
    printf("=== DKR PC ===\n");
    sHostThread.id = 3;
    sHostThread.priority = 10;
    __osRunningThread = &sHostThread;
    thread3_main(0);
    return 0;
}
