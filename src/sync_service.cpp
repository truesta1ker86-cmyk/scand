#include "sync_service.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Конструктор
// ---------------------------------------------------------------------------
SyncService::SyncService(OnecClient& onec,
                         OzonClient& ozon,
                         OnecCatalogSync& onec_catalog_sync,
                         OnecSyncRepository& onec_repo,
                         DbRepository& db,
                         WorkerPool& pool,
                         std::shared_ptr<Notifier> notifier,
                         int notify_every_percent,
                         int notify_min_interval_sec)
    : onec_(onec)
    , ozon_(ozon)
    , onec_catalog_sync_(onec_catalog_sync)
    , onec_repo_(onec_repo)
    , db_(db)
    , pool_(pool)
    , notifier_(std::move(notifier))
    , notify_every_percent_(notify_every_percent)
    , notify_min_interval_sec_(notify_min_interval_sec) {}

// ===========================================================================
// 1С (старый режим через HTTP-сервис)
// ===========================================================================
bool SyncService::trigger_full() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    bool ok = pool_.try_submit([this] { run_full_sync(); });
    if (!ok) running_.store(false);
    return ok;
}

bool SyncService::trigger_single(const std::string& product_id) {
    return pool_.try_submit([this, product_id] { run_single_sync(product_id); });
}

void SyncService::run_full_sync() {
    auto mem = SyncStateStore::instance().get("onec");
    SyncCheckpoint cp = mem ? *mem : db_.load_checkpoint("onec");

    const int page_size = onec_.page_size();
    int page = cp.last_page + 1;
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
// OZON
// ===========================================================================
bool SyncService::trigger_ozon_sync() {
    bool expected = false;
    if (!ozon_running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    ozon_stop_.store(false);
    bool ok = pool_.try_submit([this] { run_ozon_sync(); });
    if (!ok) ozon_running_.store(false);
    return ok;
}

bool SyncService::trigger_ozon_full() {
    db_.reset_checkpoint("ozon");
    SyncStateStore::instance().remove("ozon");
    return trigger_ozon_sync();
}

void SyncService::stop_ozon() {
    std::cout << "[SYNC-OZON] stop_ozon() called" << std::endl;

    db_.update_checkpoint_status("ozon", "stopped");

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

    ozon_stop_.store(true);
    ozon_running_.store(false);
}

void SyncService::run_ozon_sync() {
    SyncCheckpoint cp;
    auto mem = SyncStateStore::instance().get("ozon");
    if (mem) cp = *mem;
    else     cp = db_.load_checkpoint("ozon");

    std::string last_id = cp.last_cursor;
    size_t total = cp.total_synced;
    int page = cp.last_page;

    int retry_delay_sec = INITIAL_RETRY_DELAY_SEC;
    int consecutive_failures = 0;

    if (cp.last_cursor.empty() && cp.total_synced == 0) {
        cp.last_notified_percent = -1;
        cp.total_expected = 0;
    }

    cp.last_status = "running";
    db_.save_checkpoint(cp);

    while (true) {
        if (ozon_stop_.load()) {
            break;
        }

        OzonPage page_result = ozon_.fetch_page_cursor(last_id);

        if (page_result.network_error) {
            consecutive_failures++;
            cp.last_status = "retry";
            db_.save_checkpoint(cp);
            send_progress("ozon", total, cp.total_expected, "retry", cp);
            std::this_thread::sleep_for(std::chrono::seconds(retry_delay_sec));
            retry_delay_sec = std::min(retry_delay_sec * 2, MAX_RETRY_DELAY_SEC);
            continue;
        }

        consecutive_failures = 0;
        retry_delay_sec = INITIAL_RETRY_DELAY_SEC;

        if (cp.total_expected == 0 && page_result.total > 0) {
            cp.total_expected = page_result.total;
            send_progress("ozon", total, cp.total_expected, "running", cp);
        }

        if (page_result.items.empty()) break;

        std::vector<std::string> ids;
        for (const auto& p : page_result.items) ids.push_back(p.id);

        auto details = ozon_.fetch_info_batch(ids);

        std::vector<std::string> skus;
        for (const auto& p : details) if (!p.sku.empty()) skus.push_back(p.sku);

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

    // Не перезаписываем "stopped", если пользователь нажал Стоп
    if (!ozon_stop_.load()) {
        cp.last_status = "done";
        db_.save_checkpoint(cp);
        send_progress("ozon", total, cp.total_expected, "done", cp);
    }

    ozon_running_.store(false);
}

SyncProgress SyncService::current_ozon_progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return ozon_progress_;
}

// ===========================================================================
// 1С КАТАЛОГ (через OData)
// ===========================================================================
bool SyncService::trigger_onec_sync() {
    bool expected = false;
    if (!onec_running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    onec_stop_.store(false);

    // Новая generation
    uint64_t my_gen = onec_generation_.fetch_add(1) + 1;

    bool ok = pool_.try_submit([this, my_gen] {
        run_onec_sync(false, my_gen);
    });
    if (!ok) onec_running_.store(false);
    return ok;
}

bool SyncService::trigger_onec_full() {
    bool expected = false;
    if (!onec_running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    onec_stop_.store(false);

    uint64_t my_gen = onec_generation_.fetch_add(1) + 1;

    bool ok = pool_.try_submit([this, my_gen] {
        run_onec_sync(true, my_gen);
    });
    if (!ok) onec_running_.store(false);
    return ok;
}

// ---------------------------------------------------------------------------
// Остановка 1С: мгновенный UI + generation сбросит старые воркеры
// ---------------------------------------------------------------------------
void SyncService::stop_onec() {
    std::cout << "[SYNC-ONEC] stop_onec() called" << std::endl;

    // 1. Мгновенно пишем финальный статус в БД
    try {
        onec_repo_.update_state_status("onec_catalog", "stopped");
    } catch (const std::exception& e) {
        std::cerr << "[SYNC-ONEC] DB write failed: " << e.what() << std::endl;
    }

    // 2. Мгновенно обновляем память
    {
        std::lock_guard<std::mutex> lk(onec_progress_mutex_);
        onec_progress_.status = "stopped";
    }

    // 3. Мгновенно шлём SSE
    if (notifier_) {
        SyncProgress p;
        {
            std::lock_guard<std::mutex> lk(onec_progress_mutex_);
            p = onec_progress_;
        }
        p.status = "stopped";
        notifier_->notify(p);
    }

    // 4. Ставим stop flag текущего run
    onec_catalog_sync_.request_stop();

    // 5. Сбрасываем running в памяти
    onec_stop_.store(true);
    onec_running_.store(false);

    std::cout << "[SYNC-ONEC] stop flags set, UI notified" << std::endl;
}

SyncProgress SyncService::current_onec_progress() const {
    std::lock_guard<std::mutex> lock(onec_progress_mutex_);
    return onec_progress_;
}

// ---------------------------------------------------------------------------
// Рабочий поток 1С
// ---------------------------------------------------------------------------
void SyncService::run_onec_sync(bool full_reset, uint64_t my_gen) {
    std::cout << "[SYNC-ONEC] Started (generation " << my_gen << ")" << std::endl;

    // Не сбрасываем onec_stop_ — generation сам разрулит
    // Но сбрасываем, чтобы этот run мог работать
    onec_stop_.store(false);

    if (full_reset) {
        onec_catalog_sync_.reset();
    }

    // Начинаем новый run — получаем generation + stop flag
    auto handle = onec_catalog_sync_.begin_run();

    // Если нас перебили ещё до старта — выходим
    if (handle.generation != my_gen) {
        std::cout << "[SYNC-ONEC] Run " << my_gen
                  << " superseded (actual " << handle.generation
                  << "), aborting" << std::endl;
        return;
    }

    // Начальное состояние
    {
        std::lock_guard<std::mutex> lk(onec_progress_mutex_);
        onec_progress_.source    = "onec";
        onec_progress_.processed = 0;
        onec_progress_.total     = 0;
        onec_progress_.percent   = 0.0;
        onec_progress_.status    = "running";
    }

    if (notifier_) {
        notifier_->notify(SyncProgress{"onec", 0, 0, 0.0, "running"});
    }

    OnecCatalogSyncLimits limits;
    limits.page_size = 500;
    limits.max_rows  = 200'000;

    std::chrono::steady_clock::time_point last_notify;
    std::mutex                            notify_mutex;

    auto on_progress = [&](const OnecCatalogProgress& p) {
        // Если generation устарел — не трогаем состояние
        if (!onec_catalog_sync_.is_current_run(my_gen)) return;

        double percent = 0.0;
        if (p.total > 0) {
            percent = 100.0 * static_cast<double>(p.processed)
                    / static_cast<double>(p.total);
        } else if (p.stage == SyncStage::Done) {
            percent = 100.0;
        }

        {
            std::lock_guard<std::mutex> lk(onec_progress_mutex_);
            onec_progress_.source    = "onec";
            onec_progress_.processed = static_cast<long long>(p.processed);
            onec_progress_.total     = static_cast<long long>(p.total);
            onec_progress_.percent   = percent;
            if (onec_progress_.status != "stopped") {
                onec_progress_.status = "running";
            }
        }

        bool send = false;
        {
            std::lock_guard<std::mutex> lk(notify_mutex);
            auto now = std::chrono::steady_clock::now();
            if (last_notify.time_since_epoch().count() == 0) {
                last_notify = now;
                send = true;
            } else {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_notify).count();
                if (elapsed >= 1) {
                    last_notify = now;
                    send = true;
                }
            }
        }

        if (send && notifier_) {
            notifier_->notify(SyncProgress{
                "onec",
                static_cast<long long>(p.processed),
                static_cast<long long>(p.total),
                percent,
                "running"
            });
        }
    };

    auto on_record = [](const OnecCatalogRecord&) {};

    OnecCatalogSyncResult result;
    try {
        result = onec_catalog_sync_.read_stream_parallel(
            limits, 4, handle, on_record, on_progress
        );
    } catch (const std::exception& e) {
        std::cerr << "[SYNC-ONEC ERROR] " << e.what() << std::endl;
        result.error = e.what();
    }

    // Если нас перебили во время работы — не трогаем состояние
    if (!onec_catalog_sync_.is_current_run(my_gen)) {
        std::cout << "[SYNC-ONEC] Run " << my_gen
                  << " superseded after finish, ignoring result" << std::endl;
        return;
    }

    // Определяем финальный статус
    std::string final_status;
    if (onec_stop_.load())           final_status = "stopped";
    else if (!result.error.empty())  final_status = "failed";
    else if (result.failed > 0)      final_status = "failed";
    else                             final_status = "done";

    // В БД перезаписываем только если это не stopped
    if (final_status != "stopped") {
        try {
            onec_repo_.update_state_status("onec_catalog", final_status);
        } catch (...) {}
    }

    {
        std::lock_guard<std::mutex> lk(onec_progress_mutex_);
        onec_progress_.source    = "onec";
        onec_progress_.processed = static_cast<long long>(result.accepted);
        onec_progress_.percent   = (final_status == "stopped")
                                    ? onec_progress_.percent
                                    : 100.0;
        onec_progress_.status    = final_status;
    }

    if (notifier_) {
        SyncProgress p;
        {
            std::lock_guard<std::mutex> lk(onec_progress_mutex_);
            p = onec_progress_;
        }
        p.status = final_status;
        notifier_->notify(p);
    }

    std::cout << "[SYNC-ONEC] Finished (generation " << my_gen
              << "). Accepted: " << result.accepted
              << ", failed: " << result.failed
              << ", status: " << final_status << std::endl;

    // Сбрасываем running только если мы всё ещё актуальны
    onec_running_.store(false);
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
        if (notify_every_percent_ <= 0) {
            percent_ok = true;
        } else if (total > 0) {
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
                if (elapsed < notify_min_interval_sec_) {
                    interval_ok = false;
                }
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
// Возобновление после перезапуска
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

bool SyncService::has_unfinished_onec_sync() {
    OnecSyncState state;
    if (!onec_repo_.load_state("onec_catalog", state)) {
        return false;
    }

    if (state.status == "done")     return false;
    if (state.status == "stopped")  return false;
    if (state.status == "stopping") return false;

    auto completed = onec_repo_.get_completed_pages("onec_catalog");
    if (completed.empty() && state.accepted == 0) {
        return false;
    }

    return true;
}

void SyncService::resume_on_start() {
    std::cout << "[RESUME] ================================" << std::endl;
    std::cout << "[RESUME] Checking unfinished syncs..." << std::endl;

    {
        std::cout << "[RESUME] Ozon: checking..." << std::endl;
        SyncCheckpoint cp;
        if (has_unfinished_ozon_sync(cp)) {
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_resume_ = SyncProgress{
                    "ozon",
                    cp.total_synced,
                    cp.total_expected,
                    cp.total_expected > 0
                        ? 100.0 * cp.total_synced / cp.total_expected
                        : 0.0,
                    "resumed"
                };
            }
            trigger_ozon_sync();
        }
    }

    {
        std::cout << "[RESUME] 1C: checking..." << std::endl;
        if (has_unfinished_onec_sync()) {
            OnecSyncState state;
            if (onec_repo_.load_state("onec_catalog", state)) {
                if (notifier_) {
                    notifier_->notify(SyncProgress{
                        "onec",
                        static_cast<long long>(state.accepted),
                        0,
                        0.0,
                        "resumed"
                    });
                }
                trigger_onec_sync();
            }
        }
    }

    std::cout << "[RESUME] ================================" << std::endl;
}

std::optional<SyncProgress> SyncService::take_pending_resume() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto result = pending_resume_;
    pending_resume_.reset();
    return result;
}
