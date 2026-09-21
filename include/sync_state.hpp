#pragma once
#include "sync_checkpoint.hpp"
#include <string>
#include <mutex>
#include <optional>
#include <unordered_map>

// Singleton для хранения чекпоинтов в памяти процесса
class SyncStateStore {
public:
    static SyncStateStore& instance();

    std::optional<SyncCheckpoint> get(const std::string& source) const;
    void set(const SyncCheckpoint& cp);
    void mark_synced(const std::string& source);
    void remove(const std::string& source);

private:
    SyncStateStore() = default;
    mutable std::mutex                              mutex_;
    std::unordered_map<std::string, SyncCheckpoint> store_;
};
