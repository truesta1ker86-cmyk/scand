#include "sync_runs_repository.hpp"

#include <iostream>
#include <pqxx/pqxx>

// =============================================================================
// Конструктор
// =============================================================================
SyncRunsRepository::SyncRunsRepository(
    std::shared_ptr<scand::db::ConnectionPool> pool)
    : pool_(std::move(pool))
{}

// =============================================================================
// start
// =============================================================================
void SyncRunsRepository::start(const std::string& source,
                               const std::string& job_id,
                               int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());
    tx.exec_params(
        "INSERT INTO sync_runs "
        "    (tenant_id, source, job_id, status, started_at) "
        "VALUES "
        "    ($1, $2, $3, 'running', NOW())",
        tenant_id, source, job_id);
    tx.commit();
}

// =============================================================================
// finish_done
// =============================================================================
void SyncRunsRepository::finish_done(int tenant_id,
                                     const std::string& source,
                                     const std::string& job_id,
                                     long long accepted,
                                     long long migrated,
                                     long long conflicts,
                                     long long warehouses,
                                     long long stocks,
                                     long long costs,
                                     long long stocks_accepted,
                                     long long costs_accepted)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());
    tx.exec_params(
        "UPDATE sync_runs "
        "   SET status          = 'done', "
        "       stage           = 'done', "
        "       finished_at     = NOW(), "
        "       duration_ms     = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
        "       accepted        = $4, "
        "       migrated        = $5, "
        "       conflicts       = $6, "
        "       warehouses      = $7, "
        "       stocks          = $8, "
        "       costs           = $9, "
        "       stocks_accepted = $10, "
        "       costs_accepted  = $11 "
        " WHERE tenant_id = $1 AND source = $2 AND job_id = $3",
        tenant_id, source, job_id,
        accepted, migrated, conflicts,
        warehouses, stocks, costs,
        stocks_accepted, costs_accepted);
    tx.commit();
}

// =============================================================================
// finish_failed
// =============================================================================
void SyncRunsRepository::finish_failed(int tenant_id,
                                       const std::string& source,
                                       const std::string& job_id,
                                       const std::string& error,
                                       const std::string& stage)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());
    tx.exec_params(
        "UPDATE sync_runs "
        "   SET status      = 'failed', "
        "       stage       = $4, "
        "       finished_at = NOW(), "
        "       duration_ms = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
        "       error       = $5 "
        " WHERE tenant_id = $1 AND source = $2 AND job_id = $3",
        tenant_id, source, job_id, stage, error);
    tx.commit();
}

// =============================================================================
// abandon_running — жёсткое закрытие (reset by user → failed)
// =============================================================================
void SyncRunsRepository::abandon_running(int tenant_id,
                                         const std::string& source,
                                         const std::string& reason)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());
    tx.exec_params(
        "UPDATE sync_runs "
        "   SET status      = 'failed', "
        "       stage       = COALESCE(NULLIF(stage, ''), 'error'), "
        "       finished_at = NOW(), "
        "       duration_ms = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
        "       error       = COALESCE(NULLIF(error, ''), $3) "
        " WHERE tenant_id = $1 AND source = $2 AND status = 'running'",
        tenant_id, source, reason);
    tx.commit();
}

// =============================================================================
// mark_interrupted — мягкое закрытие (рестарт сервиса → interrupted)
// =============================================================================
void SyncRunsRepository::mark_interrupted(int tenant_id,
                                          const std::string& source,
                                          const std::string& reason)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());
    tx.exec_params(
        "UPDATE sync_runs "
        "   SET status      = 'interrupted', "
        "       finished_at = NOW(), "
        "       duration_ms = EXTRACT(EPOCH FROM (NOW() - started_at)) * 1000, "
        "       error       = COALESCE(NULLIF(error, ''), $3) "
        " WHERE tenant_id = $1 AND source = $2 AND status = 'running'",
        tenant_id, source, reason);
    tx.commit();
}

// =============================================================================
// last_success
// =============================================================================
std::optional<SyncRunRecord> SyncRunsRepository::last_success(
    int tenant_id, const std::string& source)
{
    return query_one(
        "SELECT id, source, job_id, status, stage, "
        "       started_at::text, finished_at::text, "
        "       COALESCE(duration_ms, 0), "
        "       COALESCE(accepted, 0), COALESCE(migrated, 0), COALESCE(conflicts, 0), "
        "       COALESCE(warehouses, 0), COALESCE(stocks, 0), COALESCE(costs, 0), "
        "       COALESCE(stocks_accepted, 0), COALESCE(costs_accepted, 0), "
        "       COALESCE(error, '') "
        "  FROM sync_runs "
        " WHERE tenant_id = $1 AND source = $2 AND status = 'done' "
        " ORDER BY finished_at DESC NULLS LAST "
        " LIMIT 1",
        tenant_id, source);
}

// =============================================================================
// last_run
// =============================================================================
std::optional<SyncRunRecord> SyncRunsRepository::last_run(
    int tenant_id, const std::string& source)
{
    return query_one(
        "SELECT id, source, job_id, status, stage, "
        "       started_at::text, finished_at::text, "
        "       COALESCE(duration_ms, 0), "
        "       COALESCE(accepted, 0), COALESCE(migrated, 0), COALESCE(conflicts, 0), "
        "       COALESCE(warehouses, 0), COALESCE(stocks, 0), COALESCE(costs, 0), "
        "       COALESCE(stocks_accepted, 0), COALESCE(costs_accepted, 0), "
        "       COALESCE(error, '') "
        "  FROM sync_runs "
        " WHERE tenant_id = $1 AND source = $2 "
        " ORDER BY id DESC "
        " LIMIT 1",
        tenant_id, source);
}

// =============================================================================
// query_one
// =============================================================================
std::optional<SyncRunRecord> SyncRunsRepository::query_one(
    const std::string& sql,
    int tenant_id,
    const std::string& source)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());
    auto res = tx.exec_params(sql, tenant_id, source);
    tx.commit();

    if (res.empty()) return std::nullopt;

    const auto& row = res[0];
    SyncRunRecord r;
    r.id              = row[0].as<long long>();
    r.source          = row[1].as<std::string>();
    r.job_id          = row[2].as<std::string>();
    r.status          = row[3].as<std::string>();
    r.stage           = row[4].as<std::string>();
    r.started_at      = row[5].as<std::string>();
    r.finished_at     = row[6].is_null() ? std::string{} : row[6].as<std::string>();
    r.duration_ms     = row[7].as<long long>();
    r.accepted        = row[8].as<long long>();
    r.migrated        = row[9].as<long long>();
    r.conflicts       = row[10].as<long long>();
    r.warehouses      = row[11].as<long long>();
    r.stocks          = row[12].as<long long>();
    r.costs           = row[13].as<long long>();
    r.stocks_accepted = row[14].as<long long>();
    r.costs_accepted  = row[15].as<long long>();
    r.error           = row[16].as<std::string>();
    return r;
}
