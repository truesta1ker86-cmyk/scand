#pragma once

#include "db_pool.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace scand::onec::catalog {

struct OneCProductRecord {
    std::string code;
    std::string article;
    std::string name;
    std::string ref_key;
    std::string measured_at;   // ISO 8601 UTC
};

struct OneCProductCatalogPullResult {
    std::size_t accepted   = 0;
    std::size_t migrated   = 0;
    std::size_t conflicts  = 0;
    std::string measured_at;
};

class OneCProductCatalogError : public std::runtime_error {
public:
    explicit OneCProductCatalogError(const std::string& msg)
        : std::runtime_error(msg) {}
};

// Полный read-only цикл: читает Catalog_Номенклатура из 1С через OData
// и атомарно заменяет локальный справочник inventory1c_products.
// Бросает OneCProductCatalogError при любой ошибке.
OneCProductCatalogPullResult pull_onec_product_catalog(
    pqxx::work& tx,
    long long   tenant_id);

// Чистая функция: нормализует строки, отбрасывает DeletionMark/IsFolder,
// дедуплицирует коды. Вынесена отдельно для тестирования.
std::vector<OneCProductRecord> normalize_onec_product_catalog(
    const std::vector<std::vector<std::string>>& rows,
    const std::string& measured_at);

} // namespace scand::onec::catalog
