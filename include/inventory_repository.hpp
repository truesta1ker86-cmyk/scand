#pragma once
#include "db_pool.hpp"
#include "result.hpp"
#include "inventory_preview.hpp"

#include <memory>
#include <string>

namespace scand::inventory {

struct ImportResult {
    std::string status;              // "imported" | "duplicate"
    std::string batch_id;
    size_t      stocks_accepted = 0;
    size_t      costs_accepted  = 0;
};

class InventoryRepository {
public:
    explicit InventoryRepository(std::shared_ptr<db::ConnectionPool> pool);

    // Идемпотентный импорт снимка.
    // - batch_id уже есть с тем же payload_hash → {"duplicate", batch_id}
    // - batch_id есть с другим payload_hash    → failure
    // - новый                                  → {"imported", batch_id, counts}
    scand::Result<ImportResult> import_snapshot(
        const SnapshotRequest& request,
        long long tenant_id = 1) const;

    scand::VoidResult cleanup_older_than(int days) const;

private:
    std::shared_ptr<db::ConnectionPool> pool_;
};

} // namespace scand::inventory
