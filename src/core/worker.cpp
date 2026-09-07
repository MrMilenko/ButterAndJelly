// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/worker.h"
#include "core/thread.h"

WorkerPool::WorkerPool(int threadCount)
{
    if (threadCount < 1) threadCount = 1;
    threads_.reserve((size_t)threadCount);
    for (int i = 0; i < threadCount; ++i) {
        threads_.emplace_back([this] { threadMain(); });
    }
}

WorkerPool::~WorkerPool()
{
    shutdown();
}

void WorkerPool::threadMain()
{
    for (;;) {
        Job job;
        {
            bj::Lock lock(jobMutex_);
            jobCv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++activeJobs_;
        }

        if (job) job();

        {
            bj::ScopedLock lock(jobMutex_);
            --activeJobs_;
        }
    }
}

void WorkerPool::submit(Job job)
{
    {
        bj::ScopedLock lock(jobMutex_);
        if (stopping_) return;
        jobs_.push_back(std::move(job));
    }
    jobCv_.notifyOne();
}

void WorkerPool::post(Job mainThreadCallback)
{
    bj::ScopedLock lock(resultMutex_);
    results_.push_back(std::move(mainThreadCallback));
}

void WorkerPool::drainResults()
{
    // Swap the queue out before running anything: a callback is allowed to
    // post more work, and we must not deadlock or run it in the same pass.
    std::deque<Job> ready;
    {
        bj::ScopedLock lock(resultMutex_);
        ready.swap(results_);
    }
    for (Job& job : ready) {
        if (job) job();
    }
}

size_t WorkerPool::pendingJobs()
{
    bj::ScopedLock lock(jobMutex_);
    return jobs_.size() + (size_t)activeJobs_;
}

void WorkerPool::shutdown()
{
    {
        bj::ScopedLock lock(jobMutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    jobCv_.notifyAll();
    for (bj::Thread& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}
