#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "db_pool.hpp"

struct InventoryCheckpoint {
    long long   id = 0;
    std::string job_id;
    std::string source;
    std::string last_stage;   // start | catalog | ka2_pull | save | done
    std::string period;
    std::string batch_id;

    std::vector<std::string> product_keys;
    std::unordered_map<std::string, std::string> offer_by_ref;

    long long   products_accepted = 0;
    long long   products_migrated = 0;
    long long   products_conflicts = 0;

    std::string started_at;
    std::string updated_at;
    std::string finished_at;
    std::string status;       // running | interrupted | failed | done
    std::string error;
};

class InventoryCheckpointRepository {
public:
    explicit InventoryCheckpointRepository(
        std::shared_ptr<scand::db::ConnectionPool> pool);

    void start(const std::string& job_id,
               int tenant_id = 1);

    void update(const std::string& job_id,
                const std::string& last_stage,
                const std::string& period,
                const std::string& batch_id,
                const std::vector<std::string>& product_keys,
                const std::unordered_map<std::string, std::string>& offer_by_ref,
                long long accepted,
                long long migrated,
                long long conflicts,
                int tenant_id = 1);

    void finish(const std::string& job_id,
                const std::string& status,
                const std::string& error,
                int tenant_id = 1);

    std::optional<InventoryCheckpoint> find_unfinished(int tenant_id = 1);

    void abandon_running(const std::string& reason, int tenant_id = 1);

    void mark_interrupted(const std::string& reason, int tenant_id = 1);

private:
    std::shared_ptr<scand::db::ConnectionPool> pool_;
};
