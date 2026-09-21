#include "onec_catalog_sync.hpp"
#include "onec_raw_log.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// ===========================================================================
// Утилиты
// ===========================================================================
namespace {

std::string now_iso8601_utc() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool is_truthy(const std::string& v) {
    std::string s = to_lower(v);
    return s == "true" || s == "1" || s == "да";
}

std::string clean(const std::string& value, size_t limit) {
    std::string t = trim(value);
    if (t.empty()) return "";
    if (t.size() > limit) {
        throw std::runtime_error("1С: поле слишком длинное.");
    }
    return t;
}

bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    static const int dash_pos[] = {8, 13, 18, 23};
    for (int p : dash_pos) {
        if (s[p] != '-') return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

} // namespace

// ===========================================================================
// Конструктор
// ===========================================================================
OnecCatalogSync::OnecCatalogSync(OnecCatalog& catalog,
                                 OnecSyncRepository& repo,
                                 const std::string& source)
    : catalog_(catalog)
    , repo_(repo)
    , source_(source) {}

// ===========================================================================
// begin_run / request_stop / is_current_run
// ===========================================================================
OnecCatalogSync::RunHandle OnecCatalogSync::begin_run() {
    std::lock_guard<std::mutex> lk(run_mutex_);

    // Новый флаг остановки для этого запуска
    current_stop_flag_ = std::make_shared<std::atomic<bool>>(false);

    // Новая generation
    uint64_t gen = run_generation_.fetch_add(1) + 1;

    std::cout << "[SYNC-ONEC] Beginning run #" << gen << std::endl;

    return RunHandle{gen, current_stop_flag_};
}

void OnecCatalogSync::request_stop() {
    std::lock_guard<std::mutex> lk(run_mutex_);

    if (current_stop_flag_) {
        current_stop_flag_->store(true);
        std::cout << "[SYNC-ONEC] Stop requested for current run" << std::endl;
    }
}

// ===========================================================================
// Сброс чекпоинта
// ===========================================================================
void OnecCatalogSync::reset() {
    repo_.reset_all(source_);
    std::cout << "[SYNC-ONEC] Checkpoint reset for " << source_ << std::endl;
}

// ===========================================================================
// Batch-режим
// ===========================================================================
std::vector<OnecCatalogRecord> OnecCatalogSync::read_and_normalize(
    const OnecCatalogSyncLimits& limits)
{
    if (limits.page_size == 0 || limits.max_rows == 0
        || limits.page_size > 10'000)
    {
        throw std::runtime_error(
            "Некорректный безопасный лимит каталога 1С."
        );
    }

    std::string measured_at = now_iso8601_utc();
    auto rows = catalog_.read_all_rows(limits);

    std::unordered_map<std::string, OnecCatalogRecord> by_ref;
    std::unordered_map<std::string, std::unordered_set<std::string>> refs_by_code;

    for (const auto& row : rows) {
        if (is_truthy(row.deletion_mark)) continue;
        if (is_truthy(row.is_folder))     continue;

        std::string code, ref_key, article, name;
        try {
            code    = clean(row.code,    64);
            ref_key = clean(row.ref_key, 36);
            article = clean(row.article, 128);
            name    = clean(row.name,    512);
        } catch (const std::exception& e) {
            OnecRawLog::instance().add(
                std::string("CATALOG_SYNC clean error: ") + e.what()
            );
            continue;
        }

        if (code.empty() || ref_key.empty()) continue;
        if (!is_uuid(ref_key)) continue;

        std::string ref_lower = to_lower(ref_key);

        OnecCatalogRecord rec;
        rec.code        = code;
        rec.article     = article;
        rec.name        = name;
        rec.ref_key     = ref_lower;
        rec.measured_at = measured_at;

        by_ref[ref_lower] = rec;
        refs_by_code[code].insert(ref_lower);
    }

    std::unordered_set<std::string> ambiguous_codes;
    for (const auto& [code, refs] : refs_by_code) {
        if (refs.size() > 1) ambiguous_codes.insert(code);
    }

    std::vector<OnecCatalogRecord> records;
    records.reserve(by_ref.size());

    for (const auto& [ref, rec] : by_ref) {
        if (ambiguous_codes.count(rec.code)) continue;
        records.push_back(rec);
    }

    std::sort(records.begin(), records.end(),
        [](const OnecCatalogRecord& a, const OnecCatalogRecord& b) {
            return to_lower(a.code) < to_lower(b.code);
        });

    return records;
}

size_t OnecCatalogSync::ambiguous_count(
    const std::vector<OnecCatalogRecord>& records)
{
    std::unordered_map<std::string, std::unordered_set<std::string>> refs_by_code;
    for (const auto& r : records) {
        if (r.code.empty() || r.ref_key.empty()) continue;
        refs_by_code[r.code].insert(r.ref_key);
    }
    size_t n = 0;
    for (const auto& [code, refs] : refs_by_code) {
        if (refs.size() > 1) n += refs.size();
    }
    return n;
}

// ===========================================================================
// Параллельное чтение с generation
// ===========================================================================
OnecCatalogSyncResult OnecCatalogSync::read_stream_parallel(
    const OnecCatalogSyncLimits& limits,
    size_t thread_count,
    const RunHandle& handle,
    const RecordCallback& on_record,
    const ProgressCallback& on_progress)
{
    OnecCatalogSyncResult result;
    result.measured_at = now_iso8601_utc();

    if (thread_count == 0)  thread_count = 4;
    if (thread_count > 16)  thread_count = 16;

    const uint64_t my_gen  = handle.generation;
    const StopFlag my_stop = handle.stop_flag;

    // Проверки актуальности и остановки
    auto is_stale    = [&]() -> bool { return !is_current_run(my_gen); };
    auto is_stopped  = [&]() -> bool { return my_stop->load(); };
    auto should_stop = [&]() -> bool { return is_stale() || is_stopped(); };

    std::mutex callback_mutex;
    auto emit = [&](const OnecCatalogProgress& p) {
        if (is_stale()) return;  // старый run не шлёт события
        if (!on_progress) return;
        std::lock_guard<std::mutex> lk(callback_mutex);
        try { on_progress(p); } catch (...) {}
    };

    // --- Connecting ---
    if (should_stop()) return result;
    {
        OnecCatalogProgress p;
        p.stage = SyncStage::Connecting;
        emit(p);
    }
    std::cout << "[SYNC-ONEC] Run #" << my_gen
              << ": connecting to 1C..." << std::endl;

    // --- FetchingTotal ---
    if (should_stop()) return result;
    {
        OnecCatalogProgress p;
        p.stage = SyncStage::FetchingTotal;
        emit(p);
    }

    size_t total = 0;
    if (auto count = catalog_.read_total_count()) {
        total = *count;
    } else {
        result.error = "Не удалось получить total count от 1С";
        return result;
    }

    size_t total_pages = (total + limits.page_size - 1) / limits.page_size;

    std::cout << "[SYNC-ONEC] Run #" << my_gen
              << ": total=" << total << ", pages=" << total_pages << std::endl;

    // --- LoadingCheckpoint ---
    if (should_stop()) return result;
    {
        OnecCatalogProgress p;
        p.stage       = SyncStage::LoadingCheckpoint;
        p.total       = total;
        p.total_pages = total_pages;
        emit(p);
    }

    OnecSyncState state;
    bool have_state = repo_.load_state(source_, state);

    bool compatible = have_state
        && state.url         == catalog_.get_base_url()
        && state.catalog     == "Catalog_Номенклатура"
        && state.page_size   == static_cast<int>(limits.page_size);

    if (!compatible) {
        repo_.reset_all(source_);

        state.source      = source_;
        state.url         = catalog_.get_base_url();
        state.catalog     = "Catalog_Номенклатура";
        state.page_size   = static_cast<int>(limits.page_size);
        state.total_pages = static_cast<int>(total_pages);
        state.accepted    = 0;
        state.started_at  = result.measured_at;
        state.updated_at  = result.measured_at;
        state.status      = "running";
        repo_.save_state(state);
    } else {
        state.total_pages = static_cast<int>(total_pages);
        state.status      = "running";
        repo_.save_state(state);
    }

    auto completed = repo_.get_completed_pages(source_);
    auto existing  = repo_.get_existing_refs(source_);

    std::cout << "[SYNC-ONEC] Run #" << my_gen
              << ": completed=" << completed.size()
              << ", existing=" << existing.size() << std::endl;

    std::unordered_set<std::string> seen_refs(existing.begin(), existing.end());

    std::mutex                         seen_mutex;
    std::mutex                         error_mutex;
    std::mutex                         repo_mutex;
    std::atomic<size_t>                accepted_total{state.accepted};
    std::atomic<size_t>                processed_pages{completed.size()};
    std::atomic<size_t>                failed_pages{0};
    std::atomic<bool>                  abort_flag{false};
    std::string                        first_fatal_error;
    std::unordered_map<std::string, std::unordered_set<std::string>> refs_by_code;

    // -----------------------------------------------------------------------
    // Worker
    // -----------------------------------------------------------------------
    auto worker = [&](size_t thread_id) {
        for (size_t page_idx = thread_id; page_idx < total_pages;
             page_idx += thread_count)
        {
            if (should_stop()) return;
            if (abort_flag.load()) return;
            if (completed.count(static_cast<int>(page_idx))) continue;

            size_t offset = page_idx * limits.page_size;

            // --- FetchingPage ---
            {
                OnecCatalogProgress p;
                p.stage       = SyncStage::FetchingPage;
                p.offset      = offset;
                p.page_size   = limits.page_size;
                p.processed   = accepted_total.load();
                p.total       = total;
                p.page_num    = page_idx + 1;
                p.total_pages = total_pages;
                p.failed      = failed_pages.load();
                p.percent     = total > 0
                    ? 100.0 * accepted_total.load() / total
                    : 0.0;
                emit(p);
            }

            auto page_result = catalog_.read_page_with_retry(
                offset, limits.page_size, 5, 2000,
                &abort_flag,
                my_stop.get()   // ← указатель на stop flag этого run
            );

            if (should_stop()) return;

            // --- Fatal ---
            if (page_result.kind == OnecErrorKind::Fatal) {
                if (page_result.error == "stopped by user") {
                    std::cout << "[SYNC-ONEC] Run #" << my_gen
                              << ": stopped by user (worker " << thread_id << ")"
                              << std::endl;
                    return;
                }
                {
                    std::lock_guard<std::mutex> lk(error_mutex);
                    if (first_fatal_error.empty()) {
                        first_fatal_error = page_result.error;
                    }
                }
                abort_flag.store(true);
                return;
            }

            // --- Retryable ---
            if (page_result.kind == OnecErrorKind::Retryable) {
                OnecCatalogProgress p;
                p.stage       = SyncStage::Retrying;
                p.offset      = offset;
                p.page_size   = limits.page_size;
                p.processed   = accepted_total.load();
                p.total       = total;
                p.page_num    = page_idx + 1;
                p.total_pages = total_pages;
                p.failed      = failed_pages.load();
                p.percent     = total > 0
                    ? 100.0 * accepted_total.load() / total
                    : 0.0;
                p.error       = page_result.error;
                emit(p);

                {
                    std::lock_guard<std::mutex> lk(repo_mutex);
                    repo_.mark_page_failed(source_, static_cast<int>(page_idx));
                }
                failed_pages.fetch_add(1);
                continue;
            }

            if (should_stop()) return;

            // --- Parsing ---
            {
                OnecCatalogProgress p;
                p.stage       = SyncStage::Parsing;
                p.offset      = offset;
                p.processed   = accepted_total.load();
                p.total       = total;
                p.page_num    = page_idx + 1;
                p.total_pages = total_pages;
                p.failed      = failed_pages.load();
                p.percent     = total > 0
                    ? 100.0 * accepted_total.load() / total
                    : 0.0;
                emit(p);
            }

            std::vector<OnecCatalogRow> rows;
            try {
                rows = OnecCatalog::parse_rows_static(page_result.body);
            } catch (const std::exception& e) {
                OnecRawLog::instance().add(
                    std::string("PARSE ERROR: ") + e.what()
                );
                {
                    std::lock_guard<std::mutex> lk(repo_mutex);
                    repo_.mark_page_failed(source_, static_cast<int>(page_idx));
                }
                failed_pages.fetch_add(1);
                continue;
            }

            std::vector<OnecCatalogRecord> page_records;
            page_records.reserve(rows.size());

            for (const auto& row : rows) {
                if (is_truthy(row.deletion_mark)) continue;
                if (is_truthy(row.is_folder))     continue;

                std::string code, ref_key, article, name;
                try {
                    code    = clean(row.code,    64);
                    ref_key = clean(row.ref_key, 36);
                    article = clean(row.article, 128);
                    name    = clean(row.name,    512);
                } catch (...) {
                    continue;
                }

                if (code.empty() || ref_key.empty()) continue;
                if (!is_uuid(ref_key)) continue;

                std::string ref_lower = to_lower(ref_key);

                {
                    std::lock_guard<std::mutex> lk(seen_mutex);
                    if (seen_refs.count(ref_lower)) continue;
                    seen_refs.insert(ref_lower);
                    refs_by_code[code].insert(ref_lower);
                }

                OnecCatalogRecord rec;
                rec.code        = code;
                rec.article     = article;
                rec.name        = name;
                rec.ref_key     = ref_lower;
                rec.measured_at = result.measured_at;

                page_records.push_back(rec);

                if (on_record) {
                    try { on_record(rec); } catch (...) {}
                }
            }

            // --- Saving ---
            {
                OnecCatalogProgress p;
                p.stage       = SyncStage::Saving;
                p.offset      = offset;
                p.processed   = accepted_total.load();
                p.total       = total;
                p.page_num    = page_idx + 1;
                p.total_pages = total_pages;
                p.failed      = failed_pages.load();
                p.percent     = total > 0
                    ? 100.0 * accepted_total.load() / total
                    : 0.0;
                emit(p);
            }

            // Если generation устарел или stop — не пишем в БД
            if (should_stop()) {
                std::cout << "[SYNC-ONEC] Run #" << my_gen
                          << ": stopped before commit (worker " << thread_id
                          << ")" << std::endl;
                return;
            }

            try {
                std::lock_guard<std::mutex> lk(repo_mutex);
                repo_.commit_page(source_,
                                  static_cast<int>(page_idx),
                                  page_records);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(error_mutex);
                if (first_fatal_error.empty()) {
                    first_fatal_error = e.what();
                }
                abort_flag.store(true);
                return;
            }

            accepted_total += page_records.size();
            processed_pages += 1;
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        workers.emplace_back(worker, i);
    }
    for (auto& t : workers) t.join();

    // Ambiguous
    {
        std::lock_guard<std::mutex> lk(seen_mutex);
        for (const auto& [code, refs] : refs_by_code) {
            if (refs.size() > 1) result.ambiguous += refs.size();
        }
    }

    result.accepted = accepted_total.load();
    result.migrated = accepted_total.load();
    result.failed   = failed_pages.load();

    // --- Finalizing ---
    if (should_stop()) {
        std::cout << "[SYNC-ONEC] Run #" << my_gen
                  << " is stale/stopped, ignoring final status" << std::endl;
        return result;
    }

    {
        OnecCatalogProgress p;
        p.stage       = SyncStage::Finalizing;
        p.processed   = result.accepted;
        p.total       = total;
        p.total_pages = total_pages;
        p.failed      = result.failed;
        p.percent     = total > 0 ? 100.0 * result.accepted / total : 100.0;
        emit(p);
    }

    // --- Финальный статус ---
    // Проверяем ещё раз: если за время работы нас перебили — не пишем в БД
    if (is_stale()) {
        std::cout << "[SYNC-ONEC] Run #" << my_gen
                  << " is stale, ignoring DB write" << std::endl;
        return result;
    }

    if (is_stopped()) {
        // Пользователь нажал Стоп → статус уже "stopped" в БД
        // Ничего не перезаписываем
        std::cout << "[SYNC-ONEC] Run #" << my_gen
                  << ": stopped by user (background)" << std::endl;
        return result;
    }

    if (!first_fatal_error.empty()) {
        repo_.update_state_status(source_, "failed");
        result.error = first_fatal_error;

        OnecCatalogProgress p;
        p.stage     = SyncStage::Failed;
        p.error     = first_fatal_error;
        p.processed = result.accepted;
        p.total     = total;
        p.percent   = total > 0 ? 100.0 * result.accepted / total : 0.0;
        emit(p);
    }
    else if (result.failed > 0) {
        repo_.update_state_status(source_, "failed");

        OnecCatalogProgress p;
        p.stage     = SyncStage::Failed;
        p.error     = "Failed pages: " + std::to_string(result.failed);
        p.processed = result.accepted;
        p.total     = total;
        p.percent   = total > 0 ? 100.0 * result.accepted / total : 0.0;
        emit(p);
    }
    else {
        repo_.update_state_status(source_, "done");

        OnecCatalogProgress p;
        p.stage     = SyncStage::Done;
        p.processed = result.accepted;
        p.total     = total;
        p.percent   = 100.0;
        emit(p);
    }

    std::cout << "[SYNC-ONEC] Run #" << my_gen
              << " finished. Accepted: " << result.accepted
              << ", failed: " << result.failed
              << ", ambiguous: " << result.ambiguous << std::endl;

    return result;
}

// ===========================================================================
// Простой pull
// ===========================================================================
OnecCatalogSyncResult OnecCatalogSync::pull(
    const OnecCatalogSyncLimits& limits)
{
    OnecCatalogSyncResult result;
    result.measured_at = now_iso8601_utc();

    try {
        auto records = read_and_normalize(limits);
        result.accepted  = records.size();
        result.migrated  = records.size();
        result.ambiguous = ambiguous_count(records);
    } catch (const std::exception& e) {
        result.error = e.what();
    }

    return result;
}
