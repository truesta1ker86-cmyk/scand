#include "sync_state.hpp"

SyncStateStore& SyncStateStore::instance() {
    static SyncStateStore inst;
    return inst;
}

std::optional<SyncCheckpoint> SyncStateStore::get(const std::string& source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(source);
    if (it == store_.end()) return std::nullopt;
    return it->second;
}

void SyncStateStore::set(const SyncCheckpoint& cp) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_[cp.source] = cp;
}

void SyncStateStore::mark_synced(const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(source);
    if (it != store_.end()) {
        it->second.dirty_pages = 0;
    }
}

void SyncStateStore::remove(const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_.erase(source);
}
