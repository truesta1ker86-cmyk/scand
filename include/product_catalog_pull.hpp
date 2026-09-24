#pragma once
#include "onec_catalog.hpp"
#include "sse_broker.hpp"

#include <boost/json.hpp>
#include <atomic>
#include <chrono>
#include <string>

namespace scand::onec {

struct ProductCatalogPullResult {
    size_t      total       = 0;
    size_t      read        = 0;
    size_t      skipped     = 0;
    size_t      errors      = 0;
    size_t      pages_done  = 0;
    size_t      pages_total = 0;
    long long   elapsed_ms  = 0;
    double      speed       = 0.0;
    std::string error;
};

ProductCatalogPullResult pull_product_catalog(
    OnecCatalog& onec_catalog,
    const OnecCatalogLimits& limits = {500, 200'000},
    const std::atomic<bool>* stop = nullptr);

} // namespace scand::onec
