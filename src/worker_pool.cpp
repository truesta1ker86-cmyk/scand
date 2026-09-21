#include "worker_pool.hpp"
#include <iostream>

WorkerPool::WorkerPool(size_t threads, size_t max_queue)
    : max_queue_(max_queue) {
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

WorkerPool::~WorkerPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

bool WorkerPool::try_submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= max_queue_) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
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
        }
        try {
            task();
        } catch (const std::exception& e) {
            std::cerr << "[WORKER ERROR] " << e.what() << std::endl;
        }
    }
}
