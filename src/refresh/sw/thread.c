/*
Copyright (C) 2026 Q2RTX rerelease port

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// thread.c -- the worker pool for the per-pixel stages
//
// The renderer stays one thread from the BSP to the edge scan, which costs
// the same at any resolution; what grows with the pixels is handed out here.
// R_ParallelFor runs func(arg, index, thread) for index 0..count-1 across the
// main thread and its workers, each taking the next index as it comes free,
// and returns when all are done. thread is 0 on the main thread and 1.. on
// the workers, for scratch memory of their own.
//
// sw_threads: 0 is one thread per logical processor, 1 is the original
// renderer on the main thread alone, more is that many.

#include "sw.h"
#include <SDL.h>

cvar_t      *sw_threads;
int         r_numthreads = 1;

static SDL_Thread   *r_workers[MAX_SW_THREADS];
static SDL_sem      *r_startsem;    // a post wakes a worker for the job at hand
static SDL_sem      *r_donesem;     // the last worker out posts it
static SDL_atomic_t r_nextjob;
static SDL_atomic_t r_busy;
static volatile bool r_quit;

static void (*r_jobfunc)(void *arg, int index, int thread);
static void *r_jobarg;
static int  r_jobcount;

static blocklight_t *r_threadblocklights[MAX_SW_THREADS];

static void R_RunJobs(int thread)
{
    int i;

    while ((i = SDL_AtomicAdd(&r_nextjob, 1)) < r_jobcount)
        r_jobfunc(r_jobarg, i, thread);
}

static int SDLCALL R_WorkerMain(void *data)
{
    int thread = (int)(intptr_t)data;

    while (1) {
        SDL_SemWait(r_startsem);
        if (r_quit)
            break;
        // A worker that finds the jobs gone may take a second wake-up meant
        // for one still asleep; it only adds a pass that finds nothing, and
        // every wake-up is still counted off before the main thread goes on.
        R_RunJobs(thread);
        if (SDL_AtomicAdd(&r_busy, -1) == 1)
            SDL_SemPost(r_donesem);
    }

    return 0;
}

static int R_WantThreads(void)
{
    int n = sw_threads->integer;

    if (n <= 0)
        n = SDL_GetCPUCount();

    return Q_clip(n, 1, MAX_SW_THREADS);
}

void R_InitThreads(void)
{
    int i, n;

    n = R_WantThreads();

    for (i = 0; i < n; i++)
        r_threadblocklights[i] = R_Malloc(sizeof(blocklight_t) * MAX_BLOCKLIGHTS * LIGHTMAP_BYTES);

    r_numthreads = 1;
    sw_threads->modified = false;

    if (n == 1)
        return;

    r_startsem = SDL_CreateSemaphore(0);
    r_donesem = SDL_CreateSemaphore(0);
    if (!r_startsem || !r_donesem) {
        Com_WPrintf("Software renderer: no worker threads: %s\n", SDL_GetError());
        return;
    }

    r_quit = false;
    for (i = 1; i < n; i++) {
        r_workers[i] = SDL_CreateThread(R_WorkerMain, "sw_render", (void *)(intptr_t)i);
        if (!r_workers[i]) {
            Com_WPrintf("Software renderer: worker thread %d failed: %s\n", i, SDL_GetError());
            break;
        }
        r_numthreads = i + 1;
    }

    Com_DPrintf("Software renderer: %d threads\n", r_numthreads);
}

void R_ShutdownThreads(void)
{
    int i;

    if (r_numthreads > 1) {
        r_quit = true;
        for (i = 1; i < r_numthreads; i++)
            SDL_SemPost(r_startsem);
        for (i = 1; i < r_numthreads; i++) {
            SDL_WaitThread(r_workers[i], NULL);
            r_workers[i] = NULL;
        }
    }

    if (r_startsem) {
        SDL_DestroySemaphore(r_startsem);
        r_startsem = NULL;
    }
    if (r_donesem) {
        SDL_DestroySemaphore(r_donesem);
        r_donesem = NULL;
    }

    for (i = 0; i < MAX_SW_THREADS; i++)
        Z_Freep((void **)&r_threadblocklights[i]);

    r_numthreads = 1;
}

// before a frame: sw_threads changed
void R_CheckThreads(void)
{
    if (sw_threads->modified) {
        R_ShutdownThreads();
        R_InitThreads();
    }
}

blocklight_t *R_ThreadBlocklights(int thread)
{
    return r_threadblocklights[thread];
}

// how many bands to cut rows into: a few for each thread, so that one that
// draws a busy band doesn't hold up the rest
int R_RowJobs(int rows)
{
    if (r_numthreads == 1)
        return 1;
    return Q_clip(r_numthreads * 4, 1, max(rows / 4, 1));
}

void R_ParallelFor(int count, void (*func)(void *arg, int index, int thread), void *arg)
{
    int i, wake;

    if (count <= 0)
        return;

    wake = min(r_numthreads - 1, count - 1);
    if (wake <= 0) {
        for (i = 0; i < count; i++)
            func(arg, i, 0);
        return;
    }

    // the semaphores order these stores before any worker reads them
    r_jobfunc = func;
    r_jobarg = arg;
    r_jobcount = count;
    SDL_AtomicSet(&r_nextjob, 0);
    SDL_AtomicSet(&r_busy, wake);

    for (i = 0; i < wake; i++)
        SDL_SemPost(r_startsem);

    R_RunJobs(0);

    SDL_SemWait(r_donesem);
}
