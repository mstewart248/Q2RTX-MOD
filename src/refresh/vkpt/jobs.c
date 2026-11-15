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
*/

/*
THE RENDERER'S CPU WORKERS (Matt, 11-14: "CPU frame time is causing the problem ...
can we thread this or optimize it"). The profiler's CPU breakdown put a blood dip's
frame at 26 ms of render, 20 of it the surface blood's tiles turned into primitives
(write_film_geometry) and 3 the copy of what changed into the upload staging - both
one thread, and both made of independent pieces: a 256-triangle chunk writes its own
region, a region's copy its own bytes.

The pool is the software renderer's (sw/thread.c) made common: workers made once and
kept, woken by a semaphore for each call, the main thread working beside them, the
last worker out posting the main thread on. d3d12_parallel_for makes its threads per
call, which is right for a map load and a few hundred microseconds too dear a frame.

pt_cpu_threads 0 picks half the logical processors, at most 8: the work is writes to
memory, which stops scaling well before the cores run out, and the client's blood film
worker, the sound and frame generation's present thread have threads of their own.
*/

#include "shared/shared.h"
#include "common/common.h"
#include "common/cvar.h"
#include "system/system.h"     // Sys_Microseconds (vkpt_jobs_last)
#include "jobs.h"

#include <SDL.h>

#define JOBS_MAX_THREADS    32

static cvar_t       *pt_cpu_threads;
static int          num_threads;        // the main thread and the workers started; 0 = not yet
static SDL_Thread   *workers[JOBS_MAX_THREADS];
static SDL_sem      *start_sem;         // a post wakes a worker for the call at hand
static SDL_sem      *done_sem;          // the last worker out posts it
static SDL_atomic_t next_job;
static SDL_atomic_t busy;
static volatile bool quit;

static vkpt_job_fn  job_fn;
static void         *job_ctx;
static int          job_count;

vkpt_jobs_stats_t   vkpt_jobs_last;
static SDL_atomic_t work_us;        // the jobs' own time this call, summed over the threads
static SDL_atomic_t wake_us;        // the slowest worker's start after the posts
static uint64_t     post_us;        // when the posts went out

// the jobs this thread ran
static int run_jobs(void)
{
    const uint64_t t0 = Sys_Microseconds();
    int i, ran = 0;
    while ((i = SDL_AtomicAdd(&next_job, 1)) < job_count) {
        job_fn(job_ctx, i);
        ran++;
    }
    SDL_AtomicAdd(&work_us, (int)(Sys_Microseconds() - t0));
    return ran;
}

static int SDLCALL worker_main(void *data)
{
    (void)data;
    while (1) {
        SDL_SemWait(start_sem);
        if (quit)
            break;
        {
            const int waited = (int)(Sys_Microseconds() - post_us);
            int was;
            while (waited > (was = SDL_AtomicGet(&wake_us)) && !SDL_AtomicCAS(&wake_us, was, waited))
                ;
        }
        // a worker that finds the jobs gone may take a wake-up meant for one still
        // asleep: a pass that finds nothing, still counted off (sw/thread.c)
        run_jobs();
        if (SDL_AtomicAdd(&busy, -1) == 1)
            SDL_SemPost(done_sem);
    }
    return 0;
}

static int wanted_threads(void)
{
    int n = pt_cpu_threads->integer;
    if (n <= 0) {
        n = SDL_GetCPUCount() / 2;
        n = n > 8 ? 8 : n;
    }
    return n < 1 ? 1 : n > JOBS_MAX_THREADS ? JOBS_MAX_THREADS : n;
}

void vkpt_jobs_shutdown(void)
{
    if (num_threads > 1) {
        quit = true;
        for (int i = 1; i < num_threads; i++)
            SDL_SemPost(start_sem);
        for (int i = 1; i < num_threads; i++) {
            SDL_WaitThread(workers[i], NULL);
            workers[i] = NULL;
        }
    }
    if (start_sem) {
        SDL_DestroySemaphore(start_sem);
        start_sem = NULL;
    }
    if (done_sem) {
        SDL_DestroySemaphore(done_sem);
        done_sem = NULL;
    }
    num_threads = 0;
    quit = false;
}

static void start_threads(void)
{
    const int n = wanted_threads();
    pt_cpu_threads->modified = false;
    num_threads = 1;
    if (n == 1)
        return;

    start_sem = SDL_CreateSemaphore(0);
    done_sem = SDL_CreateSemaphore(0);
    if (!start_sem || !done_sem) {
        Com_WPrintf("Renderer: no CPU worker threads: %s\n", SDL_GetError());
        return;
    }
    for (int i = 1; i < n; i++) {
        workers[i] = SDL_CreateThread(worker_main, "render jobs", NULL);
        if (!workers[i]) {
            Com_WPrintf("Renderer: CPU worker thread %d failed: %s\n", i, SDL_GetError());
            break;
        }
        num_threads = i + 1;
    }
    Com_DPrintf("Renderer: %d CPU threads for its jobs (pt_cpu_threads)\n", num_threads);
}

int vkpt_parallel_threads(void)
{
    if (!pt_cpu_threads)
        pt_cpu_threads = Cvar_Get("pt_cpu_threads", "0", 0);
    if (num_threads && pt_cpu_threads->modified)
        vkpt_jobs_shutdown();
    if (!num_threads)
        start_threads();
    return num_threads;
}

void vkpt_parallel_for(int count, vkpt_job_fn fn, void *ctx)
{
    if (count <= 0)
        return;

    const uint64_t t0 = Sys_Microseconds();
    const int threads = vkpt_parallel_threads();
    const int wake = min(threads - 1, count - 1);
    vkpt_jobs_last.count = count;
    vkpt_jobs_last.woken = max(wake, 0);
    if (wake <= 0) {
        for (int i = 0; i < count; i++)
            fn(ctx, i);
        vkpt_jobs_last.by_caller = count;
        vkpt_jobs_last.wall_us = vkpt_jobs_last.work_us = Sys_Microseconds() - t0;
        vkpt_jobs_last.wake_us = vkpt_jobs_last.tail_us = 0;
        return;
    }

    // the semaphores order these stores before any worker reads them
    job_fn = fn;
    job_ctx = ctx;
    job_count = count;
    SDL_AtomicSet(&next_job, 0);
    SDL_AtomicSet(&busy, wake);
    SDL_AtomicSet(&work_us, 0);
    SDL_AtomicSet(&wake_us, 0);
    post_us = Sys_Microseconds();

    for (int i = 0; i < wake; i++)
        SDL_SemPost(start_sem);

    vkpt_jobs_last.by_caller = run_jobs();

    const uint64_t t1 = Sys_Microseconds();
    SDL_SemWait(done_sem);
    const uint64_t t2 = Sys_Microseconds();
    vkpt_jobs_last.wall_us = t2 - t0;
    vkpt_jobs_last.tail_us = t2 - t1;
    vkpt_jobs_last.work_us = (unsigned)SDL_AtomicGet(&work_us);
    vkpt_jobs_last.wake_us = (unsigned)SDL_AtomicGet(&wake_us);
}
