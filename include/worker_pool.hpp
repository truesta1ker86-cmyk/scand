#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class WorkerPool {
public:
    WorkerPool(size_t threads, size_t max_queue);
    ~WorkerPool();

    bool try_submit(std::function<void()> task);

private:
    void worker_loop();

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    size_t                            max_queue_;
    bool                              stopping_ = false;
};
