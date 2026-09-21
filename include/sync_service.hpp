#pragma once
#include "db_repository.hpp"
#include "notifier.hpp"
#include "onec_catalog_sync.hpp"
#include "onec_client.hpp"
#include "onec_sync_repository.hpp"
#include "ozon_client.hpp"
#include "sync_state.hpp"
#include "worker_pool.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class SyncService {
public:
    SyncService(OnecClient& onec,
                OzonClient& ozon,
                OnecCatalogSync& onec_catalog_sync,
                OnecSyncRepository& onec_repo,
                DbRepository& db,
                WorkerPool& pool,
                std::shared_ptr<Notifier> notifier,
                int notify_every_percent = 5,
                int notify_min_interval_sec = 2);

    // Ozon
    bool trigger_ozon_sync();
    bool trigger_ozon_full();
    void stop_ozon();
    bool is_ozon_running() const { return ozon_running_.load(); }
    SyncProgress current_ozon_progress() const;

    // 1С каталог
    bool trigger_onec_sync();
    bool trigger_onec_full();
    void stop_onec();
    bool is_onec_running() const { return onec_running_.load(); }
    SyncProgress current_onec_progress() const;

    // 1С старый режим
    bool trigger_full();
    bool trigger_single(const std::string& product_id);
    bool is_running() const { return running_.load(); }

    // Возобновление
    void resume_on_start();
    std::optional<SyncProgress> take_pending_resume();

private:
    void run_full_sync();
    void run_single_sync(std::string product_id);
    void run_ozon_sync();
    void run_onec_sync(bool full_reset, uint64_t my_gen);

    void maybe_flush_checkpoint(const SyncCheckpoint& cp, bool force = false);
    void send_progress(const std::string& source,
                       long long processed, long long total,
                       const std::string& status,
                       SyncCheckpoint& cp);

    bool has_unfinished_ozon_sync(SyncCheckpoint& out_cp);
    bool has_unfinished_onec_sync();

    OnecClient&         onec_;
    OzonClient&         ozon_;
    OnecCatalogSync&    onec_catalog_sync_;
    OnecSyncRepository& onec_repo_;
    DbRepository&       db_;
    WorkerPool&         pool_;
    std::shared_ptr<Notifier> notifier_;

    std::atomic<bool> ozon_running_{false};
    std::atomic<bool> ozon_stop_{false};
    mutable std::mutex progress_mutex_;
    SyncProgress       ozon_progress_;

    std::atomic<bool> onec_running_{false};
    std::atomic<bool> onec_stop_{false};
    std::atomic<uint64_t> onec_generation_{0};   // ← generation для 1С
    mutable std::mutex onec_progress_mutex_;
    SyncProgress       onec_progress_;

    std::atomic<bool> running_{false};

    int notify_every_percent_;
    int notify_min_interval_sec_;
    std::chrono::steady_clock::time_point last_notify_time_;
    std::mutex notify_time_mutex_;

    std::mutex                  pending_mutex_;
    std::optional<SyncProgress> pending_resume_;

    static constexpr int CHECKPOINT_FLUSH_EVERY  = 10;
    static constexpr int MAX_RETRY_DELAY_SEC     = 60;
    static constexpr int INITIAL_RETRY_DELAY_SEC = 2;
};
