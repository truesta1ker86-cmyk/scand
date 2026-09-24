#pragma once
#include "onec_catalog.hpp"
#include "product.hpp"
#include <vector>

class OnecProductMapper {
public:
    static std::vector<Product1C> map(
        const std::vector<OnecCatalogRow>& rows);

    static size_t skipped_count(const std::vector<OnecCatalogRow>& rows);
};
