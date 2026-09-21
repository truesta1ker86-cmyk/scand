#pragma once
#include <cstddef>
#include <string>

struct OnecCatalogRecord {
    std::string code;
    std::string article;
    std::string name;
    std::string ref_key;
    std::string measured_at;
};

struct OnecSyncState {
    std::string source;
    std::string url;
    std::string catalog;
    int         page_size   = 0;
    int         total_pages = 0;
    size_t      accepted    = 0;
    std::string started_at;
    std::string updated_at;
    std::string status;
};

enum class OnecErrorKind {
    Ok,
    Retryable,
    Fatal
};

struct OnecPageResult {
    OnecErrorKind kind        = OnecErrorKind::Ok;
    int           http_status = 0;
    std::string   error;
    std::string   body;
    size_t        attempts    = 0;
};

// ---------------------------------------------------------------------------
// Стадии синхронизации
// ---------------------------------------------------------------------------
enum class SyncStage {
    Idle,
    Connecting,
    FetchingTotal,
    LoadingCheckpoint,
    FetchingPage,
    Parsing,
    Saving,
    Retrying,
    Finalizing,
    Done,
    Stopped,
    Failed,
};

inline std::string stage_to_string(SyncStage s) {
    switch (s) {
        case SyncStage::Idle:              return "idle";
        case SyncStage::Connecting:        return "connecting";
        case SyncStage::FetchingTotal:     return "fetching_total";
        case SyncStage::LoadingCheckpoint: return "loading_checkpoint";
        case SyncStage::FetchingPage:      return "fetching_page";
        case SyncStage::Parsing:           return "parsing";
        case SyncStage::Saving:            return "saving";
        case SyncStage::Retrying:          return "retrying";
        case SyncStage::Finalizing:        return "finalizing";
        case SyncStage::Done:              return "done";
        case SyncStage::Stopped:           return "stopped";
        case SyncStage::Failed:            return "failed";
    }
    return "unknown";
}
