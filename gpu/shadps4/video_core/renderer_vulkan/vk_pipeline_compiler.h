// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: worker pool that builds pipelines off the GPU thread (the startup precompile, async
// graphics pipelines, optimized links of pipeline libraries). Jobs own copies of everything they
// read and never touch the PipelineCache maps or the cache storage: the thread that owns the
// cache publishes their results (PipelineCache::PublishCompleted). No lock here is held while a
// job runs; the GPU thread only takes short critical sections (queue push, completed drain).

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/types.h"

namespace Vulkan {

class PipelineCompiler {
public:
    /// Lower value runs first.
    enum Priority : u32 {
        PriorityLive = 1,     ///< a draw was skipped for it
        PriorityPrefetch = 2, ///< seen ahead of the GPU thread
        PriorityWarmUp = 3,   ///< startup precompile
        PriorityOptimize = 4, ///< optimized relink of a fast-linked pipeline library
    };

    struct Job {
        enum State : u32 { Queued, Running, Done, Cancelled };

        virtual ~Job() = default;
        /// Builds the result (any thread). An exception marks the job failed.
        virtual void Run() = 0;

        [[nodiscard]] bool IsDone() const {
            return state.load(std::memory_order_acquire) == Done;
        }
        [[nodiscard]] bool IsFinished() const {
            const u32 s = state.load(std::memory_order_acquire);
            return s == Done || s == Cancelled;
        }

        std::atomic<u32> state{Queued};
        bool failed = false;    ///< valid once Done
        bool progress = false;  ///< counted in BbCompileProgress (Done(ok) when it ends)
        bool published = false; ///< owner thread: result taken
        u32 priority = PriorityLive;
        u64 seq = 0;
        u32 duration_us = 0;
    };
    using JobPtr = std::shared_ptr<Job>;

    PipelineCompiler();
    ~PipelineCompiler();

    /// Runs `count` workers (BB_SHADER_THREADS overrides): more are started, extra ones idle.
    void Start(u32 count, bool background);
    /// Joins the (idle) workers; jobs still queued stay queued for the next Start.
    void StopWorkers();
    /// Queues a job. False after Stop (the caller builds it itself).
    bool Enqueue(JobPtr job, u32 priority);
    /// Until the job finished: runs it on this thread when no worker took it yet (never waits
    /// behind other jobs). Returns the wait in microseconds.
    u64 Wait(const JobPtr& job);
    /// Cheap check for the owner thread: jobs finished since the last DrainCompleted.
    [[nodiscard]] bool HasCompleted() const {
        return core->completed_count.load(std::memory_order_acquire) != 0;
    }
    std::vector<JobPtr> DrainCompleted();
    /// Until every job queued so far finished (or Stop).
    void WaitIdle();
    /// Any thread: cancels queued jobs, workers exit after their current job, WaitIdle returns,
    /// Enqueue fails from now on. Does not join (Stop / StopWorkers do).
    void Cancel();
    /// Cancels queued jobs and stops the workers: waits up to `timeout_ms` for running jobs,
    /// then leaves the workers that are still busy (a hung driver call) detached.
    void Stop(u32 timeout_ms = 3000);
    [[nodiscard]] bool Stopped() const {
        return core->stopping.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 NumWorkers() const {
        return u32(workers.size());
    }
    [[nodiscard]] u32 Pending() const {
        return core->unfinished.load(std::memory_order_relaxed);
    }

    /// Hardware threads for the worker count during gameplay: max(1, threads / 4).
    static u32 GameplayWorkers();
    /// All but one hardware thread, for the startup precompile (nothing else runs yet).
    static u32 StartupWorkers();

private:
    struct Core;
    static void WorkerLoop(std::shared_ptr<Core> core, u32 index);
    static void Execute(Core& core, const JobPtr& job);

    struct Order {
        bool operator()(const JobPtr& a, const JobPtr& b) const {
            return a->priority != b->priority ? a->priority > b->priority : a->seq > b->seq;
        }
    };

    struct Core {
        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::vector<JobPtr> queue; ///< heap (Order)
        u64 next_seq = 0;
        std::atomic<u32> unfinished{0}; ///< queued or running
        std::atomic<u32> active_workers{0};
        std::atomic<u32> worker_limit{0}; ///< workers with a lower index take jobs
        std::atomic<bool> background{false};
        std::atomic<bool> stopping{false};
        std::atomic<bool> exiting{false}; ///< StopWorkers: idle workers return
        std::atomic<u32> alive{0};

        std::mutex done_mutex;
        std::condition_variable done_cv;
        std::vector<JobPtr> completed;
        std::atomic<u32> completed_count{0};
    };

    std::shared_ptr<Core> core;
    std::vector<std::thread> workers;
};

} // namespace Vulkan
