#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "db_pool.hpp"

struct AllowedWarehouse {
    std::string warehouse_id;
    bool        enabled = true;
    std::string note;
};

class AllowedWarehouseRepository {
public:
    explicit AllowedWarehouseRepository(
        std::shared_ptr<scand::db::ConnectionPool> pool);

    /** Все склады из БД (включая выключенные) */
    std::vector<AllowedWarehouse> list(int tenant_id = 1);

    /** Только включённые — для фильтрации pull */
    std::unordered_set<std::string> enabled_ids(int tenant_id = 1);

    /** Включить/выключить/добавить */
    void upsert(const std::string& warehouse_id,
                bool enabled,
                const std::string& note = "",
                int tenant_id = 1);

    /** Удалить */
    void remove(const std::string& warehouse_id,
                int tenant_id = 1);

private:
    std::shared_ptr<scand::db::ConnectionPool> pool_;
};
