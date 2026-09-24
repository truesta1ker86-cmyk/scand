#pragma once

#include <memory>
#include <optional>
#include <string>

#include "db_pool.hpp"

struct SyncRunRecord {
    long long   id = 0;
    std::string source;
    std::string job_id;
    std::string status;       // running | done | failed | interrupted
    std::string stage;
    std::string started_at;
    std::string finished_at;
    long long   duration_ms = 0;
    long long   accepted = 0;
    long long   migrated = 0;
    long long   conflicts = 0;
    long long   warehouses = 0;
    long long   stocks = 0;
    long long   costs = 0;
    long long   stocks_accepted = 0;
    long long   costs_accepted = 0;
    std::string error;
};

class SyncRunsRepository {
public:
    explicit SyncRunsRepository(std::shared_ptr<scand::db::ConnectionPool> pool);

    void start(const std::string& source,
               const std::string& job_id,
               int tenant_id = 1);

    void finish_done(int tenant_id,
                     const std::string& source,
                     const std::string& job_id,
                     long long accepted,
                     long long migrated,
                     long long conflicts,
                     long long warehouses,
                     long long stocks,
                     long long costs,
                     long long stocks_accepted,
                     long long costs_accepted);

    void finish_failed(int tenant_id,
                       const std::string& source,
                       const std::string& job_id,
                       const std::string& error,
                       const std::string& stage = "error");

    void abandon_running(int tenant_id,
                         const std::string& source,
                         const std::string& reason = "abandoned");

    void mark_interrupted(int tenant_id,
                          const std::string& source,
                          const std::string& reason = "service restarted");

    std::optional<SyncRunRecord> last_success(int tenant_id,
                                              const std::string& source);

    std::optional<SyncRunRecord> last_run(int tenant_id,
                                          const std::string& source);

private:
    std::optional<SyncRunRecord> query_one(const std::string& sql,
                                           int tenant_id,
                                           const std::string& source);

    std::shared_ptr<scand::db::ConnectionPool> pool_;
};
