// SPDX-License-Identifier: GPL-2.0-or-later

// Threads, mutexes and condition variables, one API for every platform.
//
// Shaped like std::thread, minus what nothing here uses: no timed waits, no
// shared mutexes, no futures. Both Xboxes fall back to SDL, whose libc++ is
// built without a thread backend.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

// Both Xboxes take the SDL path: neither has a usable libc++ thread backend.
#if defined(_XBOX)
  #define BJ_STD_THREADS 0
#else
  #define BJ_STD_THREADS 1
#endif

#if BJ_STD_THREADS
  #include <chrono>
  #include <condition_variable>
  #include <mutex>
  #include <thread>
#else
  #include <SDL.h>
#endif

namespace bj {

// A thread's stack. The decoder holds a good deal on its own frame and the
// XDK's default is smaller than newlib's, so ask rather than assume.
constexpr size_t kDefaultStackBytes = 256 * 1024;

class Lock;
class CondVar;

class Mutex {
public:
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

#if BJ_STD_THREADS
    Mutex() = default;
    void lock()   { m_.lock(); }
    void unlock() { m_.unlock(); }
private:
    friend class Lock;
    friend class CondVar;
    std::mutex m_;
#else
    Mutex()  { m_ = SDL_CreateMutex(); }
    ~Mutex() { if (m_) SDL_DestroyMutex(m_); }
    void lock()   { SDL_LockMutex(m_); }
    void unlock() { SDL_UnlockMutex(m_); }
private:
    friend class Lock;
    friend class CondVar;
    SDL_mutex* m_ = nullptr;
#endif
};

// Locked for its whole life. std::lock_guard.
class ScopedLock {
public:
    explicit ScopedLock(Mutex& m) : m_(m) { m_.lock(); }
    ~ScopedLock() { m_.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
private:
    Mutex& m_;
};

// Can be released and retaken, which is what a condition variable needs.
// std::unique_lock.
class Lock {
public:
    explicit Lock(Mutex& m) : m_(m) { m_.lock(); }
    ~Lock() { if (owns_) m_.unlock(); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void lock()   { m_.lock();   owns_ = true;  }
    void unlock() { m_.unlock(); owns_ = false; }
    bool owns() const { return owns_; }

private:
    friend class CondVar;
    Mutex& m_;
    bool   owns_ = true;
};

class CondVar {
public:
    CondVar(const CondVar&) = delete;
    CondVar& operator=(const CondVar&) = delete;

#if BJ_STD_THREADS
    CondVar() = default;

    void wait(Lock& lock)
    {
        // std::condition_variable wants a unique_lock, and this one already
        // holds the mutex, so hand it the mutex without letting it lock again.
        std::unique_lock<std::mutex> adopted(lock.m_.m_, std::adopt_lock);
        cv_.wait(adopted);
        adopted.release();
    }
    void notifyOne() { cv_.notify_one(); }
    void notifyAll() { cv_.notify_all(); }
private:
    std::condition_variable cv_;
#else
    CondVar()  { c_ = SDL_CreateCond(); }
    ~CondVar() { if (c_) SDL_DestroyCond(c_); }

    void wait(Lock& lock) { SDL_CondWait(c_, lock.m_.m_); }
    void notifyOne() { SDL_CondSignal(c_); }
    void notifyAll() { SDL_CondBroadcast(c_); }
private:
    SDL_cond* c_ = nullptr;
#endif

public:
    // The predicate form, which is the only one this codebase uses. Written
    // here rather than forwarded so both backends get the same loop: a
    // condition variable may wake spuriously on either.
    template <class Predicate>
    void wait(Lock& lock, Predicate stop)
    {
        while (!stop()) wait(lock);
    }
};

class Thread {
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // The body is a callable taking nothing. Where std::thread would have
    // taken a member function and its arguments, call sites use a lambda.
    explicit Thread(std::function<void()> body,
                    const char* name = "butterandjelly",
                    size_t stackBytes = kDefaultStackBytes);

    Thread(Thread&& other) noexcept { swap(other); }
    Thread& operator=(Thread&& other) noexcept
    {
        if (this != &other) { Thread dead; dead.swap(*this); swap(other); }
        return *this;
    }
    ~Thread();

    bool joinable() const;
    void join();

private:
    void swap(Thread& other) noexcept
    {
#if BJ_STD_THREADS
        std::swap(t_, other.t_);
#else
        std::swap(t_, other.t_);
#endif
    }

#if BJ_STD_THREADS
    std::thread t_;
#else
    SDL_Thread* t_ = nullptr;
#endif
};

void SleepMs(uint32_t ms);

}  // namespace bj
