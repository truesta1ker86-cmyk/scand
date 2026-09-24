#include "worker_pool.hpp"
#include <iostream>

WorkerPool::WorkerPool(std::size_t threads, std::size_t max_queue)
    : max_queue_(max_queue) {
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_  = true;
        accepting_ = false;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

bool WorkerPool::try_submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !accepting_) return false;
        if (tasks_.size() >= max_queue_) return false;
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void WorkerPool::stop_accepting() {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
}

std::size_t WorkerPool::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

bool WorkerPool::wait_idle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_idle_.wait_for(lock, timeout, [this] {
        return tasks_.empty() && active_.load() == 0;
    });
}

void WorkerPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

            if (stopping_ && tasks_.empty()) return;

            task = std::move(tasks_.front());
            tasks_.pop();
            active_.fetch_add(1, std::memory_order_relaxed);
        }

        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "[WORKER ERROR] " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[WORKER ERROR] unknown exception" << std::endl;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.fetch_sub(1, std::memory_order_relaxed) == 1
                && tasks_.empty()) {
                cv_idle_.notify_all();
            }
        }
    }
}
