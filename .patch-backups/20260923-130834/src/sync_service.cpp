#include "sync_service.hpp"
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
                         int notify_every_percent,
                         int notify_min_interval_sec)
    : onec_(onec)
    , ozon_(ozon)
    , db_(db)
    , pool_(pool)
    , notifier_(std::move(notifier))
    , notify_every_percent_(notify_every_percent)
    , notify_min_interval_sec_(notify_min_interval_sec) {}

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
    bool ok = pool_.try_submit([this] { run_ozon_sync(); });
    if (!ok) ozon_running_.store(false);
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
    // Защита от бесконечного retry.
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

    if (!ozon_stop_.load()) {
        cp.last_status = "done";
        db_.save_checkpoint(cp);
        send_progress("ozon", total, cp.total_expected, "done", cp);
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
    }

    ozon_running_.store(false);
}

SyncProgress SyncService::current_ozon_progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return ozon_progress_;
}

// ===========================================================================
// 1С OData
// ===========================================================================
// ===========================================================================
// JSON-снимки для HTTP-эндпоинтов
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
// Прогресс Ozon
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
