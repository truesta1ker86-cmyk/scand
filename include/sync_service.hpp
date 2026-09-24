#pragma once
#include "db_pool.hpp"
#include "db_repository.hpp"
#include "notifier.hpp"
#include "onec_client.hpp"
#include "ozon_client.hpp"
#include "sync_runs_repository.hpp"
#include "sync_state.hpp"
#include "worker_pool.hpp"

#include <atomic>
#include <boost/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class SyncService {
public:
    SyncService(OnecClient& onec,
                OzonClient& ozon,
                DbRepository& db,
                WorkerPool& pool,
                std::shared_ptr<Notifier> notifier,
                std::shared_ptr<scand::db::ConnectionPool> db_pool,
                int notify_every_percent = 5,
                int notify_min_interval_sec = 2,
                std::shared_ptr<SyncRunsRepository> sync_runs_repo = nullptr);

    // Ozon
    bool trigger_ozon_sync();
    bool trigger_ozon_full();
    void stop_ozon();
    bool is_ozon_running() const { return ozon_running_.load(); }
    SyncProgress current_ozon_progress() const;

    boost::json::object ozon_snapshot() const;

    // Сброс in-memory состояния Ozon (используется /api/sync/reset)
    void reset_ozon_progress();

    // Product catalog (SSE)
    bool trigger_product_catalog_pull();
    bool is_product_catalog_running() const {
        return pc_running_.load(std::memory_order_relaxed);
    }

    // 1С старый режим
    bool trigger_full();
    bool trigger_single(const std::string& product_id);
    bool is_running() const { return running_.load(); }

    // Возобновление
    void resume_on_start();
    std::optional<SyncProgress> take_pending_resume();

    // graceful shutdown
    void request_stop();
    bool is_shutting_down() const { return shutting_down_.load(); }

private:
    void run_full_sync();
    void run_single_sync(std::string product_id);
    void run_ozon_sync();
    void run_product_catalog_pull();

    void maybe_flush_checkpoint(const SyncCheckpoint& cp, bool force = false);
    void send_progress(const std::string& source,
                       long long processed, long long total,
                       const std::string& status,
                       SyncCheckpoint& cp);

    bool has_unfinished_ozon_sync(SyncCheckpoint& out_cp);

    std::string ozon_job_start(const std::string& kind);
    void        ozon_job_finish_done(long long accepted,
                                     long long migrated,
                                     long long conflicts);
    void        ozon_job_finish_failed(const std::string& error);

    OnecClient&         onec_;
    OzonClient&         ozon_;
    DbRepository&       db_;
    WorkerPool&         pool_;
    std::shared_ptr<Notifier> notifier_;

    std::atomic<bool> ozon_running_{false};
    std::atomic<bool> ozon_stop_{false};
    mutable std::mutex progress_mutex_;
    SyncProgress       ozon_progress_;

    std::atomic<bool> running_{false};
    std::atomic<bool> pc_running_{false};
    std::shared_ptr<scand::db::ConnectionPool> db_pool_;

    std::shared_ptr<SyncRunsRepository> sync_runs_repo_;
    std::string  ozon_job_id_;
    mutable std::mutex ozon_job_mutex_;

    int notify_every_percent_;
    int notify_min_interval_sec_;
    std::chrono::steady_clock::time_point last_notify_time_;
    std::mutex notify_time_mutex_;

    std::mutex                  pending_mutex_;
    std::optional<SyncProgress> pending_resume_;

    std::atomic<bool> shutting_down_{false};

    static constexpr int CHECKPOINT_FLUSH_EVERY  = 10;
    static constexpr int MAX_RETRY_DELAY_SEC     = 60;
    static constexpr int INITIAL_RETRY_DELAY_SEC = 2;
};