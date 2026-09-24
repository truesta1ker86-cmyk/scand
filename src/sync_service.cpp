#include "sync_service.hpp"
#include "onec_product_catalog.hpp"
#include "sse_broker.hpp"

#include <algorithm>
#include <boost/json.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace json = boost::json;

// ===========================================================================
// Конструктор
// ===========================================================================
SyncService::SyncService(OnecClient& onec,
                         OzonClient& ozon,
                         DbRepository& db,
                         WorkerPool& pool,
                         std::shared_ptr<Notifier> notifier,
                         std::shared_ptr<scand::db::ConnectionPool> db_pool,
                         int notify_every_percent,
                         int notify_min_interval_sec,
                         std::shared_ptr<SyncRunsRepository> sync_runs_repo)
    : onec_(onec)
    , ozon_(ozon)
    , db_(db)
    , pool_(pool)
    , notifier_(std::move(notifier))
    , db_pool_(std::move(db_pool))
    , sync_runs_repo_(std::move(sync_runs_repo))
    , notify_every_percent_(notify_every_percent)
    , notify_min_interval_sec_(notify_min_interval_sec) {}

// ===========================================================================
// SyncRuns helpers
// ===========================================================================
std::string SyncService::ozon_job_start(const std::string& kind) {
    std::string job_id = kind + "-" + std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    {
        std::lock_guard<std::mutex> lk(ozon_job_mutex_);
        ozon_job_id_ = job_id;
    }

    if (sync_runs_repo_) {
        try {
            sync_runs_repo_->abandon_running(1, "ozon", "superseded by new run");
            sync_runs_repo_->start("ozon", job_id, 1);
        } catch (const std::exception& e) {
            std::cerr << "[sync_runs][ozon] start failed: "
                      << e.what() << std::endl;
        }
    }
    return job_id;
}

void SyncService::ozon_job_finish_done(long long accepted,
                                       long long migrated,
                                       long long conflicts) {
    if (!sync_runs_repo_) return;

    std::string job_id;
    {
        std::lock_guard<std::mutex> lk(ozon_job_mutex_);
        job_id = ozon_job_id_;
    }
    if (job_id.empty()) return;

    try {
        sync_runs_repo_->finish_done(
            1, "ozon", job_id,
            accepted, migrated, conflicts,
            0, 0, 0, 0, 0);
    } catch (const std::exception& e) {
        std::cerr << "[sync_runs][ozon] finish_done failed: "
                  << e.what() << std::endl;
    }
}

void SyncService::ozon_job_finish_failed(const std::string& error) {
    if (!sync_runs_repo_) return;

    std::string job_id;
    {
        std::lock_guard<std::mutex> lk(ozon_job_mutex_);
        job_id = ozon_job_id_;
    }
    if (job_id.empty()) return;

    try {
        sync_runs_repo_->finish_failed(1, "ozon", job_id, error, "error");
    } catch (const std::exception& e) {
        std::cerr << "[sync_runs][ozon] finish_failed failed: "
                  << e.what() << std::endl;
    }
}

// ===========================================================================
// Graceful shutdown
// ===========================================================================
void SyncService::request_stop() {
    bool expected = false;
    if (!shutting_down_.compare_exchange_strong(expected, true)) return;

    std::cout << "[SyncService] request_stop: останавливаем Ozon и 1С\n";
    ozon_stop_.store(true);
}

// ===========================================================================
// 1С — старый HTTP-режим
// ===========================================================================
bool SyncService::trigger_full() {
    if (shutting_down_.load()) return false;
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return false;
    bool ok = pool_.try_submit([this] { run_full_sync(); });
    if (!ok) running_.store(false);
    return ok;
}

bool SyncService::trigger_single(const std::string& product_id) {
    if (shutting_down_.load()) return false;
    return pool_.try_submit([this, product_id] { run_single_sync(product_id); });
}

void SyncService::run_full_sync() {
    auto mem = SyncStateStore::instance().get("onec");
    SyncCheckpoint cp = mem ? *mem : db_.load_checkpoint("onec");

    const int page_size = onec_.page_size();
    int    page  = cp.last_page + 1;
    size_t total = cp.total_synced;

    while (true) {
        auto products = onec_.fetch_page(page);
        if (products.empty()) break;

        total += products.size();
        db_.upsert_batch_1c(products);

        cp.source       = "onec";
        cp.last_page    = page;
        cp.total_synced = total;
        cp.dirty_pages += 1;
        cp.last_status  = "running";
        SyncStateStore::instance().set(cp);
        db_.save_checkpoint(cp);

        if (static_cast<int>(products.size()) < page_size) break;
        ++page;
    }

    cp.last_status = "done";
    db_.save_checkpoint(cp);
    running_.store(false);
}

void SyncService::run_single_sync(std::string product_id) {
    auto product = onec_.fetch_product(product_id);
    if (product) db_.upsert_one_1c(*product);
}

// ===========================================================================
// Ozon
// ===========================================================================
bool SyncService::trigger_ozon_sync() {
    if (shutting_down_.load()) return false;
    bool expected = false;
    if (!ozon_running_.compare_exchange_strong(expected, true)) return false;
    ozon_stop_.store(false);

    ozon_job_start("sync");

    bool ok = pool_.try_submit([this] { run_ozon_sync(); });
    if (!ok) {
        ozon_running_.store(false);
        ozon_job_finish_failed("pool submit failed");
    }
    return ok;
}

bool SyncService::trigger_ozon_full() {
    if (shutting_down_.load()) return false;
    db_.reset_checkpoint("ozon");
    SyncStateStore::instance().remove("ozon");
    return trigger_ozon_sync();
}

void SyncService::stop_ozon() {
    std::cout << "[SYNC-OZON] stop_ozon() called" << std::endl;
    db_.update_checkpoint_status("ozon", "stopping");
    {
        std::lock_guard<std::mutex> lk(progress_mutex_);
        ozon_progress_.status = "stopping";
    }
    if (notifier_) {
        SyncProgress p;
        {
            std::lock_guard<std::mutex> lk(progress_mutex_);
            p = ozon_progress_;
        }
        p.status = "stopping";
        notifier_->notify(p);
    }
    ozon_stop_.store(true);
}

void SyncService::run_ozon_sync() {
    constexpr int MAX_CONSECUTIVE_FAILURES = 10;

    SyncCheckpoint cp;
    auto mem = SyncStateStore::instance().get("ozon");
    if (mem) cp = *mem;
    else     cp = db_.load_checkpoint("ozon");

    std::string last_id = cp.last_cursor;
    size_t      total   = cp.total_synced;
    int         page    = cp.last_page;

    int retry_delay_sec      = INITIAL_RETRY_DELAY_SEC;
    int consecutive_failures = 0;

    if (cp.last_cursor.empty() && cp.total_synced == 0) {
        cp.last_notified_percent = -1;
        cp.total_expected        = 0;
    }

    cp.last_status = "running";
    db_.save_checkpoint(cp);

    bool        task_failed = false;
    std::string task_error;

    while (true) {
        if (ozon_stop_.load()) break;

        OzonPage page_result = ozon_.fetch_page_cursor(last_id);

        if (page_result.network_error) {
            consecutive_failures++;
            cp.last_status = "retry";
            db_.save_checkpoint(cp);
            send_progress("ozon", total, cp.total_expected, "retry", cp);

            if (consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                cp.last_status = "failed";
                db_.save_checkpoint(cp);
                send_progress("ozon", total, cp.total_expected, "failed", cp);
                std::cerr << "[SYNC-OZON] Too many consecutive failures ("
                          << consecutive_failures << "), aborting\n";
                task_failed = true;
                task_error  = "too many consecutive network failures";
                break;
            }

            std::this_thread::sleep_for(std::chrono::seconds(retry_delay_sec));
            retry_delay_sec = std::min(retry_delay_sec * 2, MAX_RETRY_DELAY_SEC);
            continue;
        }

        consecutive_failures = 0;
        retry_delay_sec      = INITIAL_RETRY_DELAY_SEC;

        if (cp.total_expected == 0 && page_result.total > 0) {
            cp.total_expected = page_result.total;
            send_progress("ozon", total, cp.total_expected, "running", cp);
        }

        if (page_result.items.empty()) break;

        std::vector<std::string> ids;
        ids.reserve(page_result.items.size());
        for (const auto& p : page_result.items) ids.push_back(p.id);

        auto details = ozon_.fetch_info_batch(ids);

        std::vector<std::string> skus;
        skus.reserve(details.size());
        for (const auto& p : details)
            if (!p.sku.empty()) skus.push_back(p.sku);

        auto prices = ozon_.fetch_prices_stocks_batch(skus);

        std::unordered_map<std::string, OzonProduct> by_sku;
        for (auto& p : prices) by_sku[p.sku] = std::move(p);

        for (auto& p : details) {
            auto it = by_sku.find(p.sku);
            if (it != by_sku.end()) {
                p.price    = it->second.price;
                p.currency = it->second.currency;
                p.in_stock = it->second.in_stock;
            }
        }

        db_.upsert_batch_ozon(details);

        total += details.size();
        ++page;
        last_id = page_result.last_id;

        cp.source       = "ozon";
        cp.last_cursor  = last_id;
        cp.last_page    = page;
        cp.total_synced = total;
        cp.dirty_pages += 1;
        cp.last_status  = "running";
        SyncStateStore::instance().set(cp);
        db_.save_checkpoint(cp);

        send_progress("ozon", total, cp.total_expected, "running", cp);

        if (!page_result.has_more) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (task_failed) {
        ozon_job_finish_failed(task_error);
    } else if (!ozon_stop_.load()) {
        cp.last_status = "done";
        db_.save_checkpoint(cp);
        send_progress("ozon", total, cp.total_expected, "done", cp);

        ozon_job_finish_done(static_cast<long long>(total), 0, 0);
    } else {
        {
            std::lock_guard<std::mutex> lk(progress_mutex_);
            ozon_progress_.status = "stopped";
        }
        if (notifier_) {
            SyncProgress p;
            {
                std::lock_guard<std::mutex> lk(progress_mutex_);
                p = ozon_progress_;
            }
            p.status = "stopped";
            notifier_->notify(p);
        }
        ozon_job_finish_failed("stopped by user");
    }

    ozon_running_.store(false);
}

SyncProgress SyncService::current_ozon_progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return ozon_progress_;
}

// ===========================================================================
// Сброс in-memory состояния Ozon
// ===========================================================================
void SyncService::reset_ozon_progress() {
    {
        std::lock_guard<std::mutex> lk(progress_mutex_);
        SyncProgress empty;
        empty.source    = "";
        empty.processed = 0;
        empty.total     = 0;
        empty.percent   = 0.0;
        empty.status    = "";
        ozon_progress_ = empty;
    }

    ozon_running_.store(false);
    ozon_stop_.store(false);

    {
        std::lock_guard<std::mutex> lk(ozon_job_mutex_);
        ozon_job_id_.clear();
    }
}

// ===========================================================================
// JSON-снимки
// ===========================================================================
json::object SyncService::ozon_snapshot() const {
    SyncProgress p = current_ozon_progress();
    return json::object{
        {"source",    p.source},
        {"processed", p.processed},
        {"total",     p.total},
        {"percent",   p.percent},
        {"status",    p.status}
    };
}

// ===========================================================================
// Прогресс
// ===========================================================================
void SyncService::send_progress(const std::string& source,
                                long long processed, long long total,
                                const std::string& status,
                                SyncCheckpoint& cp) {
    double percent = 0.0;
    if (total > 0) {
        percent = 100.0 * static_cast<double>(processed)
                / static_cast<double>(total);
    }

    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        ozon_progress_.source    = source;
        ozon_progress_.processed = processed;
        ozon_progress_.total     = total;
        ozon_progress_.percent   = percent;
        ozon_progress_.status    = status;
    }

    bool should_notify = (status != "running");

    if (status == "running") {
        bool percent_ok = false;
        if (notify_every_percent_ <= 0) percent_ok = true;
        else if (total > 0) {
            int current_bucket = static_cast<int>(percent)
                               / notify_every_percent_ * notify_every_percent_;
            if (current_bucket > cp.last_notified_percent) {
                cp.last_notified_percent = current_bucket;
                percent_ok = true;
            }
        }

        bool interval_ok = true;
        if (notify_min_interval_sec_ > 0) {
            std::lock_guard<std::mutex> lock(notify_time_mutex_);
            auto now = std::chrono::steady_clock::now();
            if (last_notify_time_.time_since_epoch().count() > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_notify_time_).count();
                if (elapsed < notify_min_interval_sec_) interval_ok = false;
            }
        }
        should_notify = percent_ok && interval_ok;
    }

    if (!should_notify) return;

    {
        std::lock_guard<std::mutex> lock(notify_time_mutex_);
        last_notify_time_ = std::chrono::steady_clock::now();
    }

    SyncProgress p{source, processed, total, percent, status};
    if (notifier_) notifier_->notify(p);
}

void SyncService::maybe_flush_checkpoint(const SyncCheckpoint& cp, bool force) {
    if (force || cp.dirty_pages >= CHECKPOINT_FLUSH_EVERY) {
        db_.save_checkpoint(cp);
        SyncStateStore::instance().mark_synced(cp.source);
    }
}

// ===========================================================================
// Возобновление
// ===========================================================================
bool SyncService::has_unfinished_ozon_sync(SyncCheckpoint& out_cp) {
    SyncCheckpoint cp = db_.load_checkpoint("ozon");
    if (cp.last_cursor.empty() && cp.total_synced == 0) return false;
    if (cp.last_status == "done")    return false;
    if (cp.last_status == "stopped") return false;
    if (cp.total_expected > 0 && cp.total_synced >= cp.total_expected) return false;
    out_cp = cp;
    return true;
}

void SyncService::resume_on_start() {
    std::cout << "[RESUME] ================================\n";
    std::cout << "[RESUME] Checking unfinished syncs...\n";

    if (shutting_down_.load()) {
        std::cout << "[RESUME] shutting down, skip\n";
        return;
    }

    {
        std::cout << "[RESUME] Ozon: checking...\n";
        SyncCheckpoint cp;
        if (has_unfinished_ozon_sync(cp)) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_resume_ = SyncProgress{
                    "ozon", cp.total_synced, cp.total_expected,
                    cp.total_expected > 0
                        ? 100.0 * cp.total_synced / cp.total_expected : 0.0,
                    "resumed"
                };
            }
            trigger_ozon_sync();
        }
    }

    std::cout << "[RESUME] ================================\n";
}

std::optional<SyncProgress> SyncService::take_pending_resume() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto result = pending_resume_;
    pending_resume_.reset();
    return result;
}

// ===========================================================================
// Product catalog pull (SSE)
// ===========================================================================
bool SyncService::trigger_product_catalog_pull() {
    if (shutting_down_.load()) return false;

    bool expected = false;
    if (!pc_running_.compare_exchange_strong(expected, true))
        return false;

    bool ok = pool_.try_submit([this] { run_product_catalog_pull(); });
    if (!ok) pc_running_.store(false);
    return ok;
}

void SyncService::run_product_catalog_pull() {
    struct FlagGuard {
        std::atomic<bool>& f;
        ~FlagGuard() { f.store(false); }
    } guard{pc_running_};

    namespace json = boost::json;

    auto emit = [](json::object body) {
        try {
            SseBroker::instance().broadcast(
                "product_catalog_progress", std::move(body));
        } catch (...) {}
    };

    emit(json::object{
        {"stage",   "started"},
        {"status",  "running"},
        {"message", "Запуск pull..."},
    });

    try {
        if (!db_pool_) throw std::runtime_error("db_pool is null");

        auto lease = db_pool_->acquire();
        pqxx::work tx(lease.get());

        scand::onec::catalog::PullOptions opts;
        opts.tenant_id  = 1;
        opts.event_name = "product_catalog_progress";

        auto result =
            scand::onec::catalog::pull_onec_product_catalog_sse(tx, opts);

        tx.commit();

        emit(json::object{
            {"stage",       "done"},
            {"status",      "ok"},
            {"accepted",    static_cast<long long>(result.accepted)},
            {"migrated",    static_cast<long long>(result.migrated)},
            {"conflicts",   static_cast<long long>(result.conflicts)},
            {"measured_at", result.measured_at},
        });
    } catch (const std::exception& e) {
        emit(json::object{
            {"stage",  "done"},
            {"status", "failed"},
            {"error",  e.what()},
        });
    }
}
