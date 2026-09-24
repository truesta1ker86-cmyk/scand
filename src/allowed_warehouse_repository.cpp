#include "allowed_warehouse_repository.hpp"

#include <pqxx/pqxx>

// =============================================================================
// Конструктор
// =============================================================================
AllowedWarehouseRepository::AllowedWarehouseRepository(
    std::shared_ptr<scand::db::ConnectionPool> pool)
    : pool_(std::move(pool))
{}

// =============================================================================
// list
// =============================================================================
std::vector<AllowedWarehouse> AllowedWarehouseRepository::list(int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    auto res = tx.exec_params(
        "SELECT warehouse_id, enabled, COALESCE(note, '') "
        "  FROM allowed_warehouses "
        " WHERE tenant_id = $1 "
        " ORDER BY warehouse_id",
        tenant_id);

    tx.commit();

    std::vector<AllowedWarehouse> out;
    out.reserve(res.size());
    for (const auto& row : res) {
        AllowedWarehouse w;
        w.warehouse_id = row[0].as<std::string>();
        w.enabled      = row[1].as<bool>();
        w.note         = row[2].as<std::string>();
        out.push_back(std::move(w));
    }
    return out;
}

// =============================================================================
// enabled_ids
// =============================================================================
std::unordered_set<std::string> AllowedWarehouseRepository::enabled_ids(int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    auto res = tx.exec_params(
        "SELECT warehouse_id FROM allowed_warehouses "
        " WHERE tenant_id = $1 AND enabled = TRUE",
        tenant_id);

    tx.commit();

    std::unordered_set<std::string> out;
    out.reserve(res.size());
    for (const auto& row : res) {
        out.insert(row[0].as<std::string>());
    }
    return out;
}

// =============================================================================
// upsert
// =============================================================================
void AllowedWarehouseRepository::upsert(const std::string& warehouse_id,
                                        bool enabled,
                                        const std::string& note,
                                        int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    tx.exec_params(
        "INSERT INTO allowed_warehouses "
        "    (tenant_id, warehouse_id, enabled, note) "
        "VALUES ($1, $2, $3, $4) "
        "ON CONFLICT (tenant_id, warehouse_id) DO UPDATE "
        "   SET enabled    = EXCLUDED.enabled, "
        "       note       = EXCLUDED.note, "
        "       updated_at = NOW()",
        tenant_id, warehouse_id, enabled, note);

    tx.commit();
}

// =============================================================================
// remove
// =============================================================================
void AllowedWarehouseRepository::remove(const std::string& warehouse_id,
                                        int tenant_id)
{
    auto lease = pool_->acquire();
    pqxx::work tx(lease.get());

    tx.exec_params(
        "DELETE FROM allowed_warehouses "
        " WHERE tenant_id = $1 AND warehouse_id = $2",
        tenant_id, warehouse_id);

    tx.commit();
}
