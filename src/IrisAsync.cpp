//
//  IrisAsync.cpp
//  Iris
//
//  Created by Ryan Landvater on 10/4/23.
//
#include <assert.h>
#include <sstream>
#include <iostream>
#include "IrisTypes.hpp"
#include "IrisQueue.hpp"
#include "IrisAsync.hpp"

namespace Iris {
namespace Async {

ThreadPool createThreadPool (uint32_t thread_pool_size)
{
    return std::make_shared<__INTERNAL__Pool>(thread_pool_size);
}

void __INTERNAL__Fence::wait_on_signal () {
    complete.wait(false);
}

__INTERNAL__Pool::__INTERNAL__Pool (uint32_t thread_pool_size) :
_threads    (thread_pool_size),
_pending    (0),
status      (POOL_ACTIVE)
{
    // Start all of the callback threads
    for (auto& thread : _threads)
        thread = std::thread {
            &__INTERNAL__Pool::process_tasks,
            this
        };
}
__INTERNAL__Pool::~__INTERNAL__Pool ()
{
    wait_until_complete();
    assert(!_pending && "Async Queue exiting with pending tasks.");
}
inline void WARN_INACTIVE_QUEUE()
{
    std::cerr << "[WARNING] Iris Async Pool: Attempting to enqueue task to inactive queue\n";
}
size_t __INTERNAL__Pool::pending_tasks() const {
    return _pending.load();
}
void __INTERNAL__Pool::issue_task(const LambdaPtr &lambda)
{
    // Return if the pool is not active / Shutting down
    if (status & POOL_TERMINATING) return WARN_INACTIVE_QUEUE();
    
    // Count the task before publishing it: a worker may run it and decrement
    // _pending before this thread continues, and a decrement that arrives
    // first is clamped at zero and lost (found 2026-11-12; TypeTests_1.3).
    _pending++;
    _tasks.push(Callback{
        .callback       = lambda,
        .fenceOptional  = nullptr,
    });
    
    // And notify any waiting implementation threads
    _task_added.notify_one();
}
Fence __INTERNAL__Pool::issue_task_with_fence(const LambdaPtr &lambda)
{
    // Return if the pool is not active / Shutting down
    if (status & POOL_TERMINATING) { WARN_INACTIVE_QUEUE(); return NULL; }
    
    // Create a callback fence
    auto fence = std::make_shared<__INTERNAL__Fence>();
    
    // Count the task before publishing it: a worker may run it and decrement
    // _pending before this thread continues, and a decrement that arrives
    // first is clamped at zero and lost (found 2026-11-12; TypeTests_1.3).
    _pending++;
    _tasks.push(Callback{
        .callback       = lambda,
        .fenceOptional  = fence,
    });
    
    // And notify any waiting implementation threads
    _task_added.notify_one();
    
    return fence;
}
void __INTERNAL__Pool::wait_until_complete ()
{
    {// Switch the pool to the draining state under the workers' mutex, so a
     // worker between its status check and its wait cannot miss the change.
        MutexLock guard (_task_added_mtx);
        auto STATUS = status.load();
        while(!status.compare_exchange_weak(STATUS, (__status)(STATUS|POOL_DRAINING)));
    }
    _task_added.notify_all();                                   // Ensure all exit the wait.
    for (auto& thread : _threads)                               // Iterate over each worker
        if (thread.joinable())                                  // Check if it is outstanding; if so...
            thread.join();                                      // Merge threads and wait for it to complete.
    status.store(POOL_INACTIVE);
}
void __INTERNAL__Pool::terminate()
{
    {   // Switch the pool to the terminated state, under the workers' mutex (see wait_until_complete).
        MutexLock guard (_task_added_mtx);
        auto STATUS = status.load();
        while(!status.compare_exchange_weak(STATUS, (__status)(STATUS|POOL_TERMINATING)));
    }
    _task_added.notify_all();                                   // Ensure all exit the wait.
    for (auto& thread : _threads)                               // Iterate over each worker
        if (thread.joinable())                                  // Check if it is outstanding; if so...
            thread.join();                                      // Merge threads and wait for it to complete.
    status.store(POOL_INACTIVE);
}
void __INTERNAL__Pool::reset() {
    wait_until_complete();
    status.store(POOL_ACTIVE);
    for (auto& thread : _threads)
        thread = std::thread {
            &__INTERNAL__Pool::process_tasks,
            this
        };
}
void __INTERNAL__Pool::process_tasks() {
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    //  HEADER BLOCK                                    //
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ //
    using namespace FIFO2;
    Callback callback_entry;
    Iterator<Callback> __it = _tasks.begin();
    MutexLock task_lock     = MutexLock(_task_added_mtx, std::defer_lock);

    // Run what is queued before deciding whether to stop: a worker that starts
    // after wait_until_complete switched the pool to draining must still run its
    // share, or the call returns with tasks unrun (found by CI on Windows,
    // 2026-11-12; TypeTests_1.3).
    for (;;) {
        try {
            while (!(status & POOL_TERMINATING) && __it.pop(callback_entry)) {
                // Invoke the callback method and then release
                // it's context (to free captured vars).
                callback_entry.callback();
                callback_entry.callback = nullptr;

                // If there is a fence, trigger it to release any waiting threads.
                auto fence = callback_entry.fenceOptional;
                if (fence) {
                    fence->complete = true;
                    fence->complete.notify_all();
                }
                auto pending = _pending.load();
                while (!_pending.compare_exchange_weak(pending, pending?pending-1:0));
            }
        } catch (std::runtime_error& error) {
            std::stringstream LOG;
            LOG         << "[WARNING] Exception thrown on Iris Async callback thread: "
                        << error.what() << "\n";
            std::cerr   << LOG.str();
        }
        // Draining or terminating, and nothing left to pop: this worker is done.
        if (status != POOL_ACTIVE)
            return;

        // Wait for a task to be issued, or for the pool to stop. The status is
        // rechecked under the mutex before blocking, so a drain is never missed;
        // a missed task notification is still retried within a second.
        try {
            task_lock.lock();
            _task_added.wait_for(task_lock, std::chrono::seconds(1),
                                 [this] { return status != POOL_ACTIVE; });
            task_lock.unlock();
        } catch (...) {
            if (task_lock.owns_lock())
                task_lock.unlock();
            task_lock = MutexLock(_task_added_mtx, std::defer_lock);
        }
    }
}
} // END ASYNC NAMESPACE
} // END IRIS NAMESAPCE

