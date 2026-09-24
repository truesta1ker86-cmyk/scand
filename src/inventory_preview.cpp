#include "inventory_preview.hpp"
#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace scand::inventory {

namespace {

struct Selection {
    std::optional<double> value;
    std::string           source;
};

// Приоритет: batch → purchase → dealer
Selection select_unit_cost(const CostInput& row) {
    Selection s;
    if (row.batch_avg_cost && *row.batch_avg_cost > 0) {
        s.value  = *row.batch_avg_cost;
        s.source = "batch";
        return s;
    }
    if (row.purchase_cost && *row.purchase_cost > 0) {
        s.value  = *row.purchase_cost;
        s.source = "purchase";
        return s;
    }
    if (row.dealer_cost && *row.dealer_cost > 0) {
        s.value  = *row.dealer_cost;
        s.source = "dealer";
        return s;
    }
    return s;
}

} // namespace

SnapshotPreview build_preview(
    const SnapshotRequest& request,
    const PreviewOptions&  opts,
    const std::vector<std::string>& extra_warnings)
{
    SnapshotPreview out;
    out.batch_id   = request.batch_id;
    out.stock_rows = request.stocks.size();
    out.cost_rows  = request.costs.size();

    auto& w = out.warnings;
    w.insert(w.end(), extra_warnings.begin(), extra_warnings.end());

    // --- Уникальные SKU и склады ---
    std::set<std::string> stock_skus;
    std::set<std::string> cost_skus;
    std::set<std::string> warehouses;

    for (const auto& s : request.stocks) {
        stock_skus.insert(s.sku_1c);
        warehouses.insert(s.warehouse_id);
    }
    for (const auto& c : request.costs) {
        cost_skus.insert(c.sku_1c);
    }
    for (const auto& wh : request.warehouses) {
        warehouses.insert(wh.warehouse_id);
    }

    out.warehouse_count = warehouses.size();

    std::set<std::string> all_skus = stock_skus;
    all_skus.insert(cost_skus.begin(), cost_skus.end());
    out.sku_count = all_skus.size();

    // --- Проверки ---
    if (request.stocks.empty() && request.costs.empty()) {
        w.emplace_back("Снимок пуст: нет ни остатков, ни себестоимости");
    }

    // offer_id
    std::set<std::string> missing_offer_skus;
    for (const auto& s : request.stocks) {
        if (!s.offer_id || s.offer_id->empty()) {
            missing_offer_skus.insert(s.sku_1c);
        }
    }
    if (!missing_offer_skus.empty()) {
        w.emplace_back("Не заполнен offer_id у " +
                       std::to_string(missing_offer_skus.size()) + " SKU");
    }

    // stocks_without_cost
    {
        std::vector<std::string> diff;
        std::set_difference(stock_skus.begin(), stock_skus.end(),
                            cost_skus.begin(),  cost_skus.end(),
                            std::back_inserter(diff));
        if (!diff.empty())
            w.emplace_back("Нет себестоимости для " +
                           std::to_string(diff.size()) + " SKU с остатками");
    }

    // costs_without_stock
    {
        std::vector<std::string> diff;
        std::set_difference(cost_skus.begin(),  cost_skus.end(),
                            stock_skus.begin(), stock_skus.end(),
                            std::back_inserter(diff));
        if (!diff.empty())
            w.emplace_back("Есть себестоимость для " +
                           std::to_string(diff.size()) + " SKU без остатков");
    }

    // --- Preview ---
    std::vector<const CostInput*> sorted;
    sorted.reserve(request.costs.size());
    for (const auto& c : request.costs) sorted.push_back(&c);

    std::sort(sorted.begin(), sorted.end(),
        [](const CostInput* a, const CostInput* b) {
            return a->sku_1c < b->sku_1c;
        });

    size_t limit = opts.preview_limit == 0 ? 100 : opts.preview_limit;

    for (size_t i = 0; i < sorted.size() && i < limit; ++i) {
        const auto& c = *sorted[i];
        auto sel = select_unit_cost(c);

        if (!sel.value || sel.source.empty()) {
            w.emplace_back("SKU " + c.sku_1c + ": себестоимость не выбрана");
            continue;
        }

        out.cost_preview.push_back(CostPreview{
            c.sku_1c, *sel.value, sel.source,
        });
    }

    if (request.costs.size() > limit) {
        w.emplace_back("Предпросмотр себестоимости ограничен " +
                       std::to_string(limit) + " строками");
    }

    return out;
}

} // namespace scand::inventory
