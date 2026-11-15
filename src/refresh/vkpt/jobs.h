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

#pragma once

/* THE RENDERER'S CPU WORKERS (jobs.c), shared by the Vulkan and D3D12 backends:
   fn(ctx, index) for index 0..count-1 across the main thread and a pool of
   workers kept between frames, each taking the next index as it comes free;
   returns when all are done. Main thread only, and not from inside a job.
   pt_cpu_threads: 0 = automatic, 1 = the main thread alone (the A/B), n = n. */
typedef void (*vkpt_job_fn)(void *ctx, int index);

void vkpt_parallel_for(int count, vkpt_job_fn fn, void *ctx);

/* The last vkpt_parallel_for, for a slow frame's log line (blood's): its jobs, the
   workers woken, the jobs the caller ran itself, its wall time, the jobs' own time
   summed over every thread, the slowest worker's wake-up after the posts, and the
   caller's wait for the workers once its own share was done. Microseconds. A worker
   that wakes late holds the call up even with no jobs left for it - the caller waits
   for every one it woke. */
typedef struct {
    int                 count, woken, by_caller;
    unsigned long long  wall_us, work_us, wake_us, tail_us;
} vkpt_jobs_stats_t;
extern vkpt_jobs_stats_t vkpt_jobs_last;
// the threads the next vkpt_parallel_for runs on, the main thread included
int vkpt_parallel_threads(void);
void vkpt_jobs_shutdown(void);
