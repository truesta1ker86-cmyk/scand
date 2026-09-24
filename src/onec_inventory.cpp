#include "onec_inventory.hpp"
#include "onec_raw_log.hpp"
#include "onec_url_normalizer.hpp"

#include <boost/json.hpp>
#include <cpr/cpr.h>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace json = boost::json;

namespace scand::onec {

// ===========================================================================
// Утилиты JSON
// ===========================================================================
namespace {

std::string get_str(const json::object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->value().is_string()) return "";
    return json::value_to<std::string>(it->value());
}

double get_num(const json::object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end()) return 0.0;
    const auto& v = it->value();
    if (v.is_double()) return v.as_double();
    if (v.is_int64())  return static_cast<double>(v.as_int64());
    if (v.is_uint64()) return static_cast<double>(v.as_uint64());
    if (v.is_string()) {
        try { return std::stod(json::value_to<std::string>(v)); }
        catch (...) { return 0.0; }
    }
    return 0.0;
}

bool get_bool(const json::object& o, const char* key, bool def = true) {
    auto it = o.find(key);
    if (it == o.end()) return def;
    if (it->value().is_bool()) return it->value().as_bool();
    return def;
}

const json::array* extract_value(const json::value& data) {
    if (!data.is_object()) return nullptr;
    auto it = data.as_object().find("value");
    if (it == data.as_object().end() || !it->value().is_array()) return nullptr;
    return &it->value().as_array();
}

const json::array* extract_recordset(const json::object& o) {
    auto it = o.find("RecordSet");
    if (it == o.end() || !it->value().is_array()) return nullptr;
    return &it->value().as_array();
}

} // namespace

// ===========================================================================
// Конструктор
// ===========================================================================
OnecInventory::OnecInventory(std::string base_url,
                             std::string username,
                             std::string password,
                             int         timeout_ms,
                             bool        allow_insecure_http,
                             bool        allow_private_network)   // NEW
    : username_(std::move(username))
    , password_(std::move(password))
    , timeout_ms_(timeout_ms)
    , allow_insecure_http_(allow_insecure_http)                   // NEW
    , allow_private_network_(allow_private_network)               // NEW
{
    OnecUrlNormalizeOptions opts;
    opts.allow_insecure_http   = allow_insecure_http;
    opts.allow_private_network = allow_private_network;           // NEW

    base_url_ = normalize_onec_publication_url(base_url, opts);

    std::string b = base_url_;
    while (!b.empty() && b.back() == '/') b.pop_back();
    root_ = b + "/odata/standard.odata";
}

// ===========================================================================
// Универсальное чтение страницы
// ===========================================================================
std::string OnecInventory::read_page(
    const std::string& entity_path,
    const std::string& extra_query,
    size_t skip, size_t top) const
{
    if (top > 500) top = 500;

    std::string url = root_ + "/" + entity_path
        + "?$format=json&$top=" + std::to_string(top)
        + "&$skip=" + std::to_string(skip);

    if (!extra_query.empty())
        url += "&" + extra_query;

    OnecRawLog::instance().add("INV GET " + url);

    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Authentication{username_, password_, cpr::AuthMode::BASIC},
        cpr::Header{{"Accept", "application/json"},
                    {"User-Agent", "scand-inventory/1.0"}},
        cpr::Timeout{timeout_ms_});

    if (r.status_code != 200) {
        throw std::runtime_error(
            "INV HTTP " + std::to_string(r.status_code) +
            " [" + r.error.message + "] url=" + url +
            " (skip=" + std::to_string(skip) +
            ", top=" + std::to_string(top) + ")");
    }
    return r.text;
}

// ===========================================================================
// Остатки: AccumulationRegister_ТоварыНаСкладах/Balance
// ===========================================================================
std::vector<StockRow> OnecInventory::read_stocks(
    const std::atomic<bool>* stop, size_t max_rows) const
{
    std::vector<StockRow> out;
    size_t skip = 0;
    const size_t page = 500;

    while (skip < max_rows) {
        if (stop && stop->load()) break;

        std::string body = read_page(
            "AccumulationRegister_ТоварыНаСкладах/Balance",
            "", skip, page);

        auto data = json::parse(body);
        const json::array* items = extract_value(data);
        if (!items || items->empty()) break;

        for (const auto& v : *items) {
            if (!v.is_object()) continue;
            const auto& o = v.as_object();

            StockRow row;
            row.sku_1c       = get_str(o, "Номенклатура_Key");
            row.warehouse_id = get_str(o, "Склад_Key");
            row.quantity     = get_num(o, "ВНаличииBalance");

            if (row.sku_1c.empty()) continue;
            if (row.quantity <= 0)  continue;

            out.push_back(std::move(row));
        }

        if (items->size() < page) break;
        skip += items->size();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return out;
}

// ===========================================================================
// Карта: АналитикаУчетаНоменклатуры_Key → Номенклатура_Key
// ===========================================================================
std::unordered_map<std::string, std::string>
OnecInventory::load_analit_map(const std::atomic<bool>* stop) const
{
    auto now = std::chrono::steady_clock::now();
    if (!analit_cache_.empty()
        && (now - analit_cache_at_) < std::chrono::minutes(10))
    {
        return analit_cache_;
    }

    std::unordered_map<std::string, std::string> map;
    size_t skip = 0;
    const size_t page = 500;

    while (true) {
        if (stop && stop->load()) break;

        std::string body = read_page(
            "Catalog_АналитикаУчетаНоменклатуры", "", skip, page);

        auto data = json::parse(body);
        const json::array* items = extract_value(data);
        if (!items || items->empty()) break;

        for (const auto& v : *items) {
            if (!v.is_object()) continue;
            const auto& o = v.as_object();

            std::string ref = get_str(o, "Ref_Key");
            std::string nom = get_str(o, "Номенклатура_Key");

            if (!ref.empty() && !nom.empty()) {
                map[ref] = nom;
            }
        }

        if (items->size() < page) break;
        skip += items->size();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    analit_cache_    = std::move(map);
    analit_cache_at_ = now;
    return analit_cache_;
}

// ===========================================================================
// Себестоимость: ВРЕМЕННО ОТКЛЮЧЕНО
// ===========================================================================
std::vector<CostRow> OnecInventory::read_costs(
    const std::atomic<bool>* stop, size_t max_rows) const
{
    (void)stop;
    (void)max_rows;

    OnecRawLog::instance().add(
        "INV: read_costs disabled (Catalog_Analitika not in OData publication)");
    return {};
}

// ===========================================================================
// Продажные цены
// ===========================================================================
std::vector<PriceRow> OnecInventory::read_prices(
    const std::atomic<bool>* stop, size_t max_rows) const
{
    struct Best {
        double      price = 0.0;
        std::string period;
    };
    std::unordered_map<std::string, Best> best;

    size_t skip = 0;
    const size_t page = 500;

    while (skip < max_rows) {
        if (stop && stop->load()) break;

        std::string body = read_page(
            "InformationRegister_ЦеныНоменклатуры", "", skip, page);

        auto data = json::parse(body);
        const json::array* items = extract_value(data);
        if (!items || items->empty()) break;

        for (const auto& v : *items) {
            if (!v.is_object()) continue;
            const auto& outer = v.as_object();

            const json::array* rs = extract_recordset(outer);
            if (!rs) continue;

            for (const auto& rec : *rs) {
                if (!rec.is_object()) continue;
                const auto& o = rec.as_object();

                if (!get_bool(o, "Active", true)) continue;

                std::string sku = get_str(o, "Номенклатура_Key");
                if (sku.empty()) continue;

                double price = get_num(o, "Цена");
                if (price <= 0) continue;

                std::string period = get_str(o, "Period");

                auto it = best.find(sku);
                if (it == best.end() || period > it->second.period) {
                    best[sku] = Best{price, period};
                }
            }
        }

        if (items->size() < page) break;
        skip += items->size();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::vector<PriceRow> out;
    out.reserve(best.size());
    for (auto& [sku, b] : best) {
        PriceRow row;
        row.sku_1c = sku;
        row.price  = b.price;
        row.period = b.period;
        out.push_back(std::move(row));
    }
    return out;
}

// ===========================================================================
// Закупочные цены
// ===========================================================================
std::vector<PriceRow> OnecInventory::read_supplier_prices(
    const std::atomic<bool>* stop, size_t max_rows) const
{
    struct Best {
        double      price = 0.0;
        std::string period;
    };
    std::unordered_map<std::string, Best> best;

    size_t skip = 0;
    const size_t page = 500;

    while (skip < max_rows) {
        if (stop && stop->load()) break;

        std::string body = read_page(
            "InformationRegister_ЦеныНоменклатурыПоставщиков", "", skip, page);

        auto data = json::parse(body);
        const json::array* items = extract_value(data);
        if (!items || items->empty()) break;

        for (const auto& v : *items) {
            if (!v.is_object()) continue;
            const auto& outer = v.as_object();

            const json::array* rs = extract_recordset(outer);
            if (!rs) continue;

            for (const auto& rec : *rs) {
                if (!rec.is_object()) continue;
                const auto& o = rec.as_object();

                if (!get_bool(o, "Active", true)) continue;

                std::string sku = get_str(o, "Номенклатура_Key");
                if (sku.empty()) continue;

                double price = get_num(o, "Цена");
                if (price <= 0) continue;

                std::string period = get_str(o, "Period");

                auto it = best.find(sku);
                if (it == best.end()
                    || price < it->second.price
                    || (price == it->second.price
                        && period > it->second.period))
                {
                    best[sku] = Best{price, period};
                }
            }
        }

        if (items->size() < page) break;
        skip += items->size();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::vector<PriceRow> out;
    out.reserve(best.size());
    for (auto& [sku, b] : best) {
        PriceRow row;
        row.sku_1c = sku;
        row.price  = b.price;
        row.period = b.period;
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace scand::onec
