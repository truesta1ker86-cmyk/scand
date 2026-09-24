#include "inventory_checkpoint_repository.hpp"

#include <iostream>
#include <boost/json.hpp>
#include <pqxx/pqxx>

namespace json = boost::json;

// =============================================================================
// Конструктор
// =============================================================================
InventoryCheckpointRepository::InventoryCheckpointRepository(
    std::shared_ptr<scand::db::ConnectionPool> pool)
    : pool_(std::move(pool))
{}

// =============================================================================
// start
// =============================================================================
void InventoryCheckpointRepository::start(const std::string& job_id,
                                          int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    // Закрываем ВСЁ незавершённое — и running, и interrupted.
    // Это критично: иначе retry-таймер и авто-возобновление будут
    // бесконечно находить старые чекпоинты и перезапускать задачу.
    tx.exec_params(
        "UPDATE inventory1c_checkpoints "
        "   SET status      = 'failed', "
        "       finished_at = NOW(), "
        "       updated_at  = NOW(), "
        "       error       = 'superseded by new run' "
        " WHERE tenant_id = $1 AND status IN ('running','interrupted')",
        tenant_id);

    tx.exec_params(
        "INSERT INTO inventory1c_checkpoints "
        "    (tenant_id, job_id, source, last_stage, status, started_at, updated_at) "
        "VALUES ($1, $2, 'inventory_pull', 'start', 'running', NOW(), NOW())",
        tenant_id, job_id);

    tx.commit();
}

// =============================================================================
// update
// =============================================================================
void InventoryCheckpointRepository::update(
    const std::string& job_id,
    const std::string& last_stage,
    const std::string& period,
    const std::string& batch_id,
    const std::vector<std::string>& product_keys,
    const std::unordered_map<std::string, std::string>& offer_by_ref,
    long long accepted,
    long long migrated,
    long long conflicts,
    int tenant_id)
{
    json::array keys_json;
    for (const auto& k : product_keys) keys_json.emplace_back(k);

    json::object offers_json;
    for (const auto& [ref, offer] : offer_by_ref) offers_json[ref] = offer;

    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    tx.exec_params(
        "UPDATE inventory1c_checkpoints "
        "   SET last_stage          = $3, "
        "       period              = $4, "
        "       batch_id            = $5, "
        "       product_keys_json   = $6::jsonb, "
        "       offer_by_ref_json   = $7::jsonb, "
        "       products_accepted   = $8, "
        "       products_migrated   = $9, "
        "       products_conflicts  = $10, "
        "       updated_at          = NOW() "
        " WHERE tenant_id = $1 AND job_id = $2 AND status = 'running'",
        tenant_id, job_id, last_stage, period, batch_id,
        json::serialize(keys_json),
        json::serialize(offers_json),
        accepted, migrated, conflicts);

    tx.commit();
}

// =============================================================================
// finish
// =============================================================================
void InventoryCheckpointRepository::finish(const std::string& job_id,
                                           const std::string& status,
                                           const std::string& error,
                                           int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    tx.exec_params(
        "UPDATE inventory1c_checkpoints "
        "   SET status      = $3, "
        "       error       = $4, "
        "       finished_at = NOW(), "
        "       updated_at  = NOW() "
        " WHERE tenant_id = $1 AND job_id = $2",
        tenant_id, job_id, status, error);

    tx.commit();
}

// =============================================================================
// find_unfinished — ТОЛЬКО interrupted (не running!)
//
// Раньше искал ('running','interrupted') — это и был источник цикла:
// retry-таймер видел активную задачу и перезапускал её.
// Теперь retry срабатывает только когда задача реально упала.
// =============================================================================
std::optional<InventoryCheckpoint> InventoryCheckpointRepository::find_unfinished(
    int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    auto res = tx.exec_params(
        "SELECT id, job_id, source, last_stage, "
        "       COALESCE(period, ''), COALESCE(batch_id, ''), "
        "       COALESCE(product_keys_json,  '[]'::jsonb)::text, "
        "       COALESCE(offer_by_ref_json,  '{}'::jsonb)::text, "
        "       products_accepted, products_migrated, products_conflicts, "
        "       started_at::text, updated_at::text, "
        "       COALESCE(finished_at::text, ''), "
        "       status, COALESCE(error, '') "
        "  FROM inventory1c_checkpoints "
        " WHERE tenant_id = $1 AND status = 'interrupted' "
        " ORDER BY updated_at DESC "
        " LIMIT 1",
        tenant_id);

    tx.commit();
    if (res.empty()) return std::nullopt;

    const auto& row = res[0];
    InventoryCheckpoint cp;
    cp.id                 = row[0].as<long long>();
    cp.job_id             = row[1].as<std::string>();
    cp.source             = row[2].as<std::string>();
    cp.last_stage         = row[3].as<std::string>();
    cp.period             = row[4].as<std::string>();
    cp.batch_id           = row[5].as<std::string>();

    try {
        auto parsed = json::parse(row[6].as<std::string>());
        if (parsed.is_array()) {
            for (const auto& v : parsed.as_array()) {
                if (v.is_string()) cp.product_keys.push_back(std::string(v.as_string()));
            }
        }
    } catch (...) {}

    try {
        auto parsed = json::parse(row[7].as<std::string>());
        if (parsed.is_object()) {
            for (const auto& [k, v] : parsed.as_object()) {
                if (v.is_string()) cp.offer_by_ref[std::string(k)] = std::string(v.as_string());
            }
        }
    } catch (...) {}

    cp.products_accepted  = row[8].as<long long>();
    cp.products_migrated  = row[9].as<long long>();
    cp.products_conflicts = row[10].as<long long>();
    cp.started_at         = row[11].as<std::string>();
    cp.updated_at         = row[12].as<std::string>();
    cp.finished_at        = row[13].as<std::string>();
    cp.status             = row[14].as<std::string>();
    cp.error              = row[15].as<std::string>();

    return cp;
}

// =============================================================================
// abandon_running — закрывает и running, и interrupted
// =============================================================================
void InventoryCheckpointRepository::abandon_running(const std::string& reason,
                                                    int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    tx.exec_params(
        "UPDATE inventory1c_checkpoints "
        "   SET status      = 'failed', "
        "       error       = COALESCE(NULLIF(error, ''), $2), "
        "       finished_at = NOW(), "
        "       updated_at  = NOW() "
        " WHERE tenant_id = $1 AND status IN ('running','interrupted')",
        tenant_id, reason);

    tx.commit();
}

// =============================================================================
// mark_interrupted — мягкое закрытие (рестарт сервиса)
// =============================================================================
void InventoryCheckpointRepository::mark_interrupted(const std::string& reason,
                                                     int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    tx.exec_params(
        "UPDATE inventory1c_checkpoints "
        "   SET status      = 'interrupted', "
        "       error       = COALESCE(NULLIF(error, ''), $2), "
        "       finished_at = NOW(), "
        "       updated_at  = NOW() "
        " WHERE tenant_id = $1 AND status = 'running'",
        tenant_id, reason);

    tx.commit();
}
