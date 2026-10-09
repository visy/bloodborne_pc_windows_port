// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#include "bbport_compile_progress.h"
#include "bbport_threads.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"

namespace Vulkan {

namespace {
void SetWorkerPriority(bool background) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(),
                      background ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#else
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), background ? 5 : 0);
#endif
}

u32 EnvThreads() {
    if (const char* env = std::getenv("BB_SHADER_THREADS")) {
        return u32(std::clamp(std::atoi(env), 1, 64));
    }
    return 0;
}
} // namespace

PipelineCompiler::PipelineCompiler() : core{std::make_shared<Core>()} {}

PipelineCompiler::~PipelineCompiler() {
    Stop();
}

u32 PipelineCompiler::GameplayWorkers() {
    if (const u32 env = EnvThreads()) {
        return env;
    }
    return std::max(1u, BbThreads::Available() / 4);
}

u32 PipelineCompiler::StartupWorkers() {
    if (const u32 env = EnvThreads()) {
        return env;
    }
    const u32 available = BbThreads::Available();
    return std::clamp(available > 2 ? available - 1 : available, 1u, 32u);
}

void PipelineCompiler::Start(u32 count, bool background) {
    if (core->stopping) {
        return;
    }
    count = std::max(1u, count);
    core->background = background;
    core->worker_limit = count;
    while (workers.size() < count) {
        ++core->alive;
        workers.emplace_back(WorkerLoop, core, u32(workers.size()));
    }
    core->queue_cv.notify_all();
}

void PipelineCompiler::StopWorkers() {
    {
        std::scoped_lock lk{core->queue_mutex};
        core->exiting = true;
    }
    core->queue_cv.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
    core->exiting = false;
}

bool PipelineCompiler::Enqueue(JobPtr job, u32 priority) {
    {
        std::scoped_lock lk{core->queue_mutex};
        if (core->stopping) {
            return false;
        }
        job->priority = priority;
        job->seq = core->next_seq++;
        ++core->unfinished;
        core->queue.push_back(std::move(job));
        std::push_heap(core->queue.begin(), core->queue.end(), Order{});
    }
    core->queue_cv.notify_one();
    return true;
}

void PipelineCompiler::Execute(Core& core, const JobPtr& job) {
    const auto start = std::chrono::steady_clock::now();
    try {
        job->Run();
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan, "Pipeline compiler: job failed ({})", e.what());
        job->failed = true;
    }
    job->duration_us = u32(std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count());
    BbCompileProgress::RecordJob(job->duration_us);
    {
        std::scoped_lock lk{core.done_mutex};
        job->state.store(Job::Done, std::memory_order_release);
        core.completed.push_back(job);
        core.completed_count.fetch_add(1, std::memory_order_release);
    }
    core.done_cv.notify_all();
    if (job->progress) {
        BbCompileProgress::Done(!job->failed);
    }
    {
        std::scoped_lock lk{core.queue_mutex};
        --core.unfinished;
    }
    core.queue_cv.notify_all();
}

void PipelineCompiler::WorkerLoop(std::shared_ptr<Core> core, u32 index) {
    const std::string name = "bb:Shader" + std::to_string(index);
    Common::SetCurrentThreadName(name.c_str());
    bool background = core->background.load();
    SetWorkerPriority(background);
    for (;;) {
        JobPtr job;
        {
            std::unique_lock lk{core->queue_mutex};
            core->queue_cv.wait(lk, [&] {
                return core->stopping.load() || core->exiting.load() ||
                       (!core->queue.empty() && index < core->worker_limit.load());
            });
            if (core->stopping || core->exiting) {
                break;
            }
            std::pop_heap(core->queue.begin(), core->queue.end(), Order{});
            job = std::move(core->queue.back());
            core->queue.pop_back();
        }
        u32 expected = Job::Queued;
        if (!job->state.compare_exchange_strong(expected, Job::Running)) {
            continue; // taken by a waiting thread (Wait), which did the bookkeeping
        }
        if (const bool want = core->background.load(); want != background) {
            background = want;
            SetWorkerPriority(background);
        }
        Execute(*core, job);
    }
    {
        std::scoped_lock lk{core->done_mutex};
        --core->alive;
    }
    core->done_cv.notify_all();
}

u64 PipelineCompiler::Wait(const JobPtr& job) {
    const auto start = std::chrono::steady_clock::now();
    u32 expected = Job::Queued;
    if (job->state.compare_exchange_strong(expected, Job::Running)) {
        // Still queued: built here instead of waiting behind other jobs. The worker that pops
        // the queue entry skips it.
        Execute(*core, job);
    } else {
        std::unique_lock lk{core->done_mutex};
        core->done_cv.wait(lk, [&] { return job->IsFinished(); });
    }
    return u64(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count());
}

std::vector<PipelineCompiler::JobPtr> PipelineCompiler::DrainCompleted() {
    std::vector<JobPtr> out;
    std::scoped_lock lk{core->done_mutex};
    out.swap(core->completed);
    core->completed_count.store(0, std::memory_order_release);
    return out;
}

void PipelineCompiler::WaitIdle() {
    std::unique_lock lk{core->queue_mutex};
    core->queue_cv.wait(lk, [&] { return core->unfinished.load() == 0 || core->stopping.load(); });
}

void PipelineCompiler::Cancel() {
    std::vector<JobPtr> dropped;
    {
        std::scoped_lock lk{core->queue_mutex};
        core->stopping = true;
        dropped.swap(core->queue);
    }
    for (auto& job : dropped) {
        u32 expected = Job::Queued;
        if (job->state.compare_exchange_strong(expected, Job::Cancelled)) {
            if (job->progress) {
                BbCompileProgress::Done(false);
            }
            --core->unfinished;
        }
    }
    core->queue_cv.notify_all();
    {
        std::scoped_lock lk{core->done_mutex};
    }
    core->done_cv.notify_all();
}

void PipelineCompiler::Stop(u32 timeout_ms) {
    Cancel();
    if (workers.empty()) {
        return;
    }
    bool exited = true;
    {
        std::unique_lock lk{core->done_mutex};
        exited = core->done_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                        [&] { return core->alive.load() == 0; });
    }
    for (auto& worker : workers) {
        if (!worker.joinable()) {
            continue;
        }
        if (exited) {
            worker.join();
        } else {
            worker.detach(); // a job stuck in the driver: do not hang the shutdown
        }
    }
    if (!exited) {
        LOG_WARNING(Render_Vulkan, "Pipeline compiler: workers still busy at shutdown, detached");
    }
    workers.clear();
}

} // namespace Vulkan
