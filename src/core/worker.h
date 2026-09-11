// SPDX-License-Identifier: GPL-2.0-or-later

// worker.h: a small fixed thread pool.
//
// Every network call in the core blocks and the UI must not, so they run here
// and hand results back through a queue the main thread drains each frame.

#pragma once

#include "core/thread.h"

#include <deque>
#include <functional>
#include <vector>

class WorkerPool {
public:
    using Job = std::function<void()>;

    explicit WorkerPool(int threadCount = 3);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Queued jobs run on a pool thread, in submission order.
    void submit(Job job);

    // Called from a pool thread to hand a result back. The callback runs on
    // the main thread inside drainResults, so it may touch the renderer.
    void post(Job mainThreadCallback);

    // Main thread only. Runs everything posted since the last call.
    void drainResults();

    // Stops accepting work and joins. Safe to call twice.
    void shutdown();

    size_t pendingJobs();

private:
    void threadMain();

    std::vector<bj::Thread> threads_;

    bj::Mutex              jobMutex_;
    bj::CondVar jobCv_;
    std::deque<Job>         jobs_;
    int                     activeJobs_ = 0;
    bool                    stopping_   = false;

    bj::Mutex      resultMutex_;
    std::deque<Job> results_;
};
