#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scand::inventory {

// ===========================================================================
// Один остаток (соответствует Python OneCStockInput)
// ===========================================================================
struct StockInput {
    std::string                sku_1c;
    std::optional<std::string> offer_id;
    std::string                warehouse_id;
    std::optional<std::string> warehouse_name;
    long long                  available = 0;
    long long                  reserved  = 0;
};

// ===========================================================================
// Один склад (OneCWarehouseInput)
// ===========================================================================
struct WarehouseInput {
    std::string warehouse_id;
    std::string warehouse_name;
};

// ===========================================================================
// Одна себестоимость (OneCCostInput)
// ===========================================================================
struct CostInput {
    std::string                sku_1c;
    std::optional<double>      batch_avg_cost;
    std::optional<double>      purchase_cost;
    std::optional<std::string> purchase_source;     // "supplier_register" | "purchase_document"
    std::optional<std::string> purchase_source_at;  // ISO 8601
    std::optional<double>      dealer_cost;
};

// ===========================================================================
// Полный снимок (OneCInventorySnapshotRequest)
// ===========================================================================
struct SnapshotRequest {
    std::string                  batch_id;
    std::string                  measured_at;   // ISO 8601 UTC
    std::string                  mode = "FULL";
    std::vector<StockInput>      stocks;
    std::vector<WarehouseInput>  warehouses;
    std::vector<CostInput>       costs;
};

// ===========================================================================
// Строка preview (OneCCostPreview)
// ===========================================================================
struct CostPreview {
    std::string sku_1c;
    double      selected_cost = 0.0;
    std::string cost_source;   // "batch" | "purchase" | "dealer"
};

// ===========================================================================
// Результат preview (OneCInventorySnapshotPreviewResponse)
// ===========================================================================
struct SnapshotPreview {
    std::string              status = "valid";
    std::string              batch_id;
    size_t                   stock_rows      = 0;
    size_t                   cost_rows       = 0;
    size_t                   sku_count       = 0;
    size_t                   warehouse_count = 0;
    std::vector<CostPreview> cost_preview;
    std::vector<std::string> warnings;
};

// ===========================================================================
// Опции
// ===========================================================================
struct PreviewOptions {
    size_t preview_limit = 100;   // было 50, Python использует 100
};

// ===========================================================================
// API
// ===========================================================================
SnapshotPreview build_preview(
    const SnapshotRequest& request,
    const PreviewOptions&  opts = {},
    const std::vector<std::string>& extra_warnings = {}
);

} // namespace scand::inventory
