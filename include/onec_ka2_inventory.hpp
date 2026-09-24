#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <boost/json.hpp>

namespace scand::onec::ka2 {

struct CatalogProduct {
    std::string ref_key;
    std::string article;
    std::string description;
    std::string name;
    std::string unit_ref;
    bool        use_packs     = false;
    bool        deletion_mark = false;
    bool        is_folder     = false;
};

struct Warehouse {
    std::string ref_key;
    std::string description;
    bool        deletion_mark = false;
    bool        is_folder     = false;
};

struct AnalyticKey {
    std::string ref_key;
    std::string nomenclature_key;
    std::string characteristic_key;
};

struct StockBalance {
    std::string nomenclature_key;
    std::string characteristic_key;
    std::string warehouse_key;
    double      quantity = 0.0;
};

struct CostBalance {
    std::string analytic_key;
    std::string organization_key;
    std::string batch_key;
    double      quantity = 0.0;
    double      cost     = 0.0;
};

struct SupplierPrice {
    std::string period;
    std::string partner_key;
    std::string nomenclature_key;
    std::string characteristic_key;
    double      price = 0.0;
};

struct Ka2Options {
    std::size_t chunk_size       = 40;
    std::size_t page_size_flat   = 5000;
    int         max_retries      = 3;
    int         retry_delay_ms   = 1000;
    int         parallel_workers = 4;
    bool        verbose          = true;
};

struct PullResult {
    std::vector<CatalogProduct> products;
    std::vector<Warehouse>      warehouses;
    std::vector<AnalyticKey>    analytics;
    std::vector<StockBalance>   stocks;
    std::vector<CostBalance>    costs;
    std::vector<SupplierPrice>  suppliers;
    std::vector<std::string>    warnings;
};

class Ka2Inventory {
public:
    Ka2Inventory(std::string base_url,
                 std::string username,
                 std::string password,
                 int         timeout_ms,
                 bool        allow_insecure_http   = false,
                 Ka2Options  opts = {},
                 bool        allow_private_network = false);   // NEW
    // Внимание: allow_private_network идёт ПОСЛЕ opts.
    // Если у вас уже есть вызовы с 6 аргументами — они не сломаются
    // (значение по умолчанию false).

    PullResult pull(const std::vector<std::string>& product_keys,
                    const std::string&              period,
                    const std::atomic<bool>*        stop = nullptr) const;

    static std::vector<std::vector<std::string>>
    chunk_vector(const std::vector<std::string>& values, std::size_t chunk_size);

    static std::string
    build_guid_filter(const std::string& field,
                      const std::vector<std::string>& guids);

    static bool is_valid_guid(const std::string& s);

private:
    std::string read_page(
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& params,
        std::size_t skip,
        std::size_t top,
        const std::atomic<bool>* stop) const;

    std::vector<boost::json::object> read_chunked(
        const std::string& path,
        const std::string& filter_field,
        const std::vector<std::string>& values,
        const std::string& select_fields,
        const std::atomic<bool>* stop) const;

    std::vector<boost::json::object> read_chunked_parallel(
        const std::string& path,
        const std::string& filter_field,
        const std::vector<std::string>& values,
        const std::string& select_fields,
        const std::atomic<bool>* stop) const;

    std::vector<boost::json::object> read_balance_chunked(
        const std::string& register_name,
        const std::string& period,
        const std::string& dimensions,
        const std::string& filter_field,
        const std::vector<std::string>& values,
        const std::atomic<bool>* stop) const;

    std::vector<Warehouse>
    read_warehouses(const std::atomic<bool>* stop) const;

    std::vector<AnalyticKey>
    read_analytics(const std::vector<std::string>& product_keys,
                   const std::atomic<bool>* stop) const;

    std::vector<StockBalance>
    read_stocks(const std::string& period,
                const std::vector<std::string>& product_keys,
                const std::atomic<bool>* stop) const;

    std::vector<CostBalance>
    read_costs(const std::string& period,
               const std::vector<std::string>& analytic_keys,
               const std::atomic<bool>* stop) const;

    std::vector<SupplierPrice>
    read_suppliers(const std::vector<std::string>& product_keys,
                   const std::atomic<bool>* stop) const;

    std::string base_url_;
    std::string username_;
    std::string password_;
    int         timeout_ms_;
    std::string root_;
    Ka2Options  opts_;
    bool        allow_insecure_http_   = false;   // NEW
    bool        allow_private_network_ = false;   // NEW
};

} // namespace scand::onec::ka2