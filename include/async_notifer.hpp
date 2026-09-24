include/async_notifier.hpp << 'EOF'
#pragma once
#include "notifier.hpp"
#include "worker_pool.hpp"
#include <memory>
#include <utility>

// Оборачивает синхронный Notifier, вынося вызов в WorkerPool,
// чтобы не блокировать горячий путь.
class AsyncNotifier : public Notifier {
public:
    AsyncNotifier(std::shared_ptr<Notifier> inner, WorkerPool& pool)
        : inner_(std::move(inner)), pool_(pool) {}

    void notify(const SyncProgress& p) override {
        auto inner = inner_;
        pool_.try_submit([inner, p] {
            try { if (inner) inner->notify(p); }
            catch (...) { /* не роняем воркер */ }
        });
    }

private:
    std::shared_ptr<Notifier> inner_;
    WorkerPool&               pool_;
};
