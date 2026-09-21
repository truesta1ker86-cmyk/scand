#pragma once
#include "onec_catalog.hpp"
#include "onec_sync_repository.hpp"
#include "onec_types.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using OnecCatalogSyncLimits = OnecCatalogLimits;

// Флаг остановки — свой на каждый запуск
using StopFlag = std::shared_ptr<std::atomic<bool>>;

struct OnecCatalogSyncResult {
    size_t      accepted   = 0;
    size_t      migrated   = 0;
    size_t      conflicts  = 0;
    size_t      ambiguous  = 0;
    size_t      failed     = 0;
    std::string error;
    std::string measured_at;
};

struct OnecCatalogProgress {
    SyncStage   stage       = SyncStage::Idle;
    size_t      offset      = 0;
    size_t      page_size   = 0;
    size_t      processed   = 0;
    size_t      total       = 0;
    size_t      page_num    = 0;
    size_t      total_pages = 0;
    size_t      failed      = 0;
    double      percent     = 0.0;
    std::string error;
};

class OnecCatalogSync {
public:
    OnecCatalogSync(OnecCatalog& catalog,
                    OnecSyncRepository& repo,
                    const std::string& source = "onec_catalog");

    using RecordCallback   = std::function<void(const OnecCatalogRecord&)>;
    using ProgressCallback = std::function<void(const OnecCatalogProgress&)>;

    // Запуск — generation + свой stop flag
    struct RunHandle {
        uint64_t generation = 0;
        StopFlag stop_flag;
    };

    RunHandle begin_run();

    bool is_current_run(uint64_t generation) const {
        return run_generation_.load() == generation;
    }

    OnecCatalogSyncResult read_stream_parallel(
        const OnecCatalogSyncLimits& limits,
        size_t thread_count,
        const RunHandle& handle,
        const RecordCallback& on_record,
        const ProgressCallback& on_progress = nullptr);

    // Остановить ТЕКУЩИЙ запуск (старые уже неактуальны)
    void request_stop();

    // Сбросить чекпоинт
    void reset();

    // Batch-режим (без generation)
    std::vector<OnecCatalogRecord> read_and_normalize(
        const OnecCatalogSyncLimits& limits);

    OnecCatalogSyncResult pull(const OnecCatalogSyncLimits& limits);

    static size_t ambiguous_count(
        const std::vector<OnecCatalogRecord>& records);

private:
    OnecCatalog&        catalog_;
    OnecSyncRepository& repo_;
    std::string         source_;

    mutable std::mutex    run_mutex_;
    std::atomic<uint64_t> run_generation_{0};
    StopFlag              current_stop_flag_;
};
