// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/thread.h"

namespace bj {

#if BJ_STD_THREADS

Thread::Thread(std::function<void()> body, const char* name, size_t stackBytes)
{
    (void)name;         // no portable way to name one, and nothing reads it
    (void)stackBytes;   // the standard library gives no way to ask
    t_ = std::thread(std::move(body));
}

Thread::~Thread()
{
    // Same contract as std::thread: a running thread must be joined before its
    // handle goes away. Left to std::thread this would call std::terminate.
    if (t_.joinable()) t_.join();
}

bool Thread::joinable() const { return t_.joinable(); }

void Thread::join() { if (t_.joinable()) t_.join(); }

void SleepMs(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

#else  // SDL

namespace {

// The body outlives this call, so it goes on the heap and the thread owns it.
//
// SDLCALL, because OXDK compiles with stdcall as the default convention while
// SDL_ThreadFunction is cdecl, and the two do not interchange.
int SDLCALL RunBody(void* data)
{
    std::function<void()>* body = (std::function<void()>*)data;
    if (body && *body) (*body)();
    delete body;
    return 0;
}

}  // namespace

Thread::Thread(std::function<void()> body, const char* name, size_t stackBytes)
{
    std::function<void()>* owned = new std::function<void()>(std::move(body));
    t_ = SDL_CreateThreadWithStackSize(RunBody, name, stackBytes, owned);
    if (!t_) delete owned;
}

Thread::~Thread()
{
    if (t_) SDL_WaitThread(t_, nullptr);
}

bool Thread::joinable() const { return t_ != nullptr; }

void Thread::join()
{
    if (!t_) return;
    SDL_WaitThread(t_, nullptr);
    t_ = nullptr;
}

void SleepMs(uint32_t ms) { SDL_Delay(ms); }

#endif

}  // namespace bj
