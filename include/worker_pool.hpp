#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class WorkerPool {
public:
    WorkerPool(std::size_t threads, std::size_t max_queue);
    ~WorkerPool();

    bool try_submit(std::function<void()> task);

    void stop_accepting();
    bool wait_idle(std::chrono::milliseconds timeout);
    std::size_t active() const noexcept { return active_.load(); }
    std::size_t queued() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    mutable std::mutex       mutex_;
    std::condition_variable  cv_;
    std::condition_variable  cv_idle_;

    std::queue<std::function<void()>> tasks_;
    const std::size_t max_queue_;

    bool stopping_  = false;
    bool accepting_ = true;
    std::atomic<std::size_t> active_{0};
};
