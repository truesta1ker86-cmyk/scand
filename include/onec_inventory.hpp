#pragma once
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scand::onec {

struct StockRow {
    std::string sku_1c;         // Номенклатура_Key (UUID из Catalog_Номенклатура)
    std::string warehouse_id;   // Склад_Key (UUID из Catalog_Склады)
    double      quantity = 0.0;
};

struct CostRow {
    std::string           sku_1c;
    std::optional<double> batch_avg_cost;   // средняя по партиям из СебестоимостьТоваров
    std::optional<double> purchase_cost;    // минимум от поставщиков
    std::optional<double> dealer_cost;      // продажная цена из ЦеныНоменклатуры
};

struct PriceRow {
    std::string sku_1c;
    double      price = 0.0;
    std::string period;                     // ISO 8601
};

class OnecInventory {
public:
    OnecInventory(std::string base_url,
                  std::string username,
                  std::string password,
                  int         timeout_ms,
                  bool        allow_insecure_http   = false,
                  bool        allow_private_network = false);   // NEW

    // Остатки: AccumulationRegister_ТоварыНаСкладах/Balance
    std::vector<StockRow> read_stocks(
        const std::atomic<bool>* stop = nullptr,
        size_t max_rows = 200'000) const;

    // Себестоимость: AccumulationRegister_СебестоимостьТоваров/Balance
    // JOIN через Catalog_АналитикаУчетаНоменклатуры (карта кэшируется)
    std::vector<CostRow> read_costs(
        const std::atomic<bool>* stop = nullptr,
        size_t max_rows = 200'000) const;

    // Продажные цены: InformationRegister_ЦеныНоменклатуры
    std::vector<PriceRow> read_prices(
        const std::atomic<bool>* stop = nullptr,
        size_t max_rows = 200'000) const;

    // Закупочные цены: InformationRegister_ЦеныНоменклатурыПоставщиков
    std::vector<PriceRow> read_supplier_prices(
        const std::atomic<bool>* stop = nullptr,
        size_t max_rows = 200'000) const;

private:
    std::string base_url_;
    std::string username_;
    std::string password_;
    int         timeout_ms_;
    std::string root_;
    bool        allow_insecure_http_   = false;   // NEW
    bool        allow_private_network_ = false;   // NEW

    // Универсальный page-reader
    std::string read_page(const std::string& entity_path,
                          const std::string& extra_query,
                          size_t skip, size_t top) const;

    // Карта: АналитикаУчетаНоменклатуры_Key → Номенклатура_Key
    mutable std::unordered_map<std::string, std::string> analit_cache_;
    mutable std::chrono::steady_clock::time_point        analit_cache_at_;

    std::unordered_map<std::string, std::string>
    load_analit_map(const std::atomic<bool>* stop = nullptr) const;
};

} // namespace scand::onec