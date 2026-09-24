#include "onec_ka2_inventory.hpp"
#include "onec_raw_log.hpp"
#include "onec_url_normalizer.hpp"

#include <boost/json.hpp>
#include <cpr/cpr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace json = boost::json;

namespace scand::onec::ka2 {

namespace {

struct HttpError : public std::runtime_error {
    long status;
    HttpError(long s, const std::string& msg)
        : std::runtime_error(msg), status(s) {}
};

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

bool get_bool(const json::object& o, const char* key, bool def = false) {
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

std::string encode_path_for_cpr(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool ok = (c < 0x80) &&
                  c != ' ' && c != '\'' && c != '"' &&
                  c != '<' && c != '>' && c != '#' &&
                  c != '{' && c != '}' && c != '|' &&
                  c != '\\' && c != '^' && c != '`' &&
                  c >= 0x20;
        if (ok) out += static_cast<char>(c);
        else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string escape_odata_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'') out += "''";
        else           out += c;
    }
    return out;
}

} // namespace

bool Ka2Inventory::is_valid_guid(const std::string& s) {
    if (s.size() != 36) return false;
    static const int dash[] = {8, 13, 18, 23};
    for (int p : dash) if (s[p] != '-') return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

std::vector<std::vector<std::string>>
Ka2Inventory::chunk_vector(const std::vector<std::string>& values,
                           std::size_t chunk_size)
{
    std::vector<std::vector<std::string>> out;
    if (chunk_size == 0) chunk_size = 40;
    for (std::size_t i = 0; i < values.size(); i += chunk_size) {
        std::size_t end = std::min(i + chunk_size, values.size());
        out.emplace_back(values.begin() + i, values.begin() + end);
    }
    return out;
}

std::string
Ka2Inventory::build_guid_filter(const std::string& field,
                                const std::vector<std::string>& guids)
{
    if (guids.empty()) return "";
    std::string out;
    for (std::size_t i = 0; i < guids.size(); ++i) {
        if (i > 0) out += " or ";
        out += field + " eq guid'" + guids[i] + "'";
    }
    return out;
}

// ===========================================================================
// Конструктор
// ===========================================================================
Ka2Inventory::Ka2Inventory(std::string base_url,
                           std::string username,
                           std::string password,
                           int         timeout_ms,
                           bool        allow_insecure_http,
                           Ka2Options  opts,
                           bool        allow_private_network)   // NEW
    : username_(std::move(username))
    , password_(std::move(password))
    , timeout_ms_(timeout_ms)
    , opts_(opts)
    , allow_insecure_http_(allow_insecure_http)                 // NEW
    , allow_private_network_(allow_private_network)             // NEW
{
    OnecUrlNormalizeOptions url_opts;
    url_opts.allow_insecure_http   = allow_insecure_http;
    url_opts.allow_private_network = allow_private_network;     // NEW

    base_url_ = normalize_onec_publication_url(base_url, url_opts);

    std::string b = base_url_;
    while (!b.empty() && b.back() == '/') b.pop_back();
    root_ = b + "/odata/standard.odata";
}

std::string Ka2Inventory::read_page(
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& params,
    std::size_t skip, std::size_t top,
    const std::atomic<bool>* stop) const
{
    std::string encoded_path = encode_path_for_cpr(path);
    std::string base = root_ + "/" + encoded_path;

    cpr::Parameters cpr_params;
    for (const auto& [k, v] : params) cpr_params.Add({k, v});
    cpr_params.Add({"$top",  std::to_string(top)});
    cpr_params.Add({"$skip", std::to_string(skip)});

    if (opts_.verbose) {
        std::string log_url = base + "?";
        bool first = true;
        for (const auto& [k, v] : params) {
            if (!first) log_url += "&";
            log_url += k + "=" + v;
            first = false;
        }
        if (!first) log_url += "&";
        log_url += "$top=" + std::to_string(top);
        log_url += "&$skip=" + std::to_string(skip);
        OnecRawLog::instance().add(
            "KA2 GET len=" + std::to_string(log_url.size()) + " " + log_url);
    }

    for (int attempt = 1; attempt <= opts_.max_retries; ++attempt) {
        if (stop && stop->load())
            throw std::runtime_error("stopped by user");

        int timeout = std::max(timeout_ms_, 20000) + (attempt - 1) * 20000;

        auto t0 = std::chrono::steady_clock::now();
        cpr::Response r = cpr::Get(
            cpr::Url{base},
            cpr_params,
            cpr::Authentication{username_, password_, cpr::AuthMode::BASIC},
            cpr::Header{{"Accept", "application/json"},
                        {"User-Agent", "scand-ka2/2.0"}},
            cpr::Timeout{timeout});

        if (r.status_code == 200) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            OnecRawLog::instance().add(
                "KA2 TIMING path=" + path +
                " skip=" + std::to_string(skip) +
                " top=" + std::to_string(top) +
                " ms=" + std::to_string(ms) +
                " bytes=" + std::to_string(r.text.size()));
            return r.text;
        }

        if (r.status_code == 414) {
            OnecRawLog::instance().add("KA2 HTTP 414: URL too long, will split");
            throw HttpError(414, "URI Too Long");
        }

        std::string err = "KA2 HTTP " + std::to_string(r.status_code)
                        + " [" + r.error.message + "]"
                        + " attempt=" + std::to_string(attempt)
                        + "/" + std::to_string(opts_.max_retries)
                        + " path=" + path
                        + " skip=" + std::to_string(skip);
        OnecRawLog::instance().add(err);

        if (attempt < opts_.max_retries)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(opts_.retry_delay_ms));
    }

    throw HttpError(0, "KA2 read_page failed: " + path);
}

std::vector<json::object> Ka2Inventory::read_chunked(
    const std::string& path,
    const std::string& filter_field,
    const std::vector<std::string>& values,
    const std::string& select_fields,
    const std::atomic<bool>* stop) const
{
    std::vector<json::object> out;
    if (values.empty()) return out;

    auto chunks = chunk_vector(values, opts_.chunk_size);

    for (const auto& chunk : chunks) {
        if (stop && stop->load()) break;

        std::string filter = build_guid_filter(filter_field, chunk);
        std::size_t skip = 0;

        while (true) {
            if (stop && stop->load()) break;

            std::vector<std::pair<std::string, std::string>> params;
            params.emplace_back("$format", "json");
            params.emplace_back("$filter", filter);
            if (!select_fields.empty())
                params.emplace_back("$select", select_fields);

            std::string body;
            try {
                body = read_page(path, params, skip,
                                 opts_.page_size_flat, stop);
            } catch (const std::exception& e) {
                OnecRawLog::instance().add(
                    std::string("KA2 chunked error: ") + e.what());
                break;
            }

            auto data = json::parse(body);
            const json::array* items = extract_value(data);
            if (!items || items->empty()) break;

            for (const auto& v : *items)
                if (v.is_object()) out.push_back(v.as_object());

            if (items->size() < opts_.page_size_flat) break;
            skip += items->size();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    return out;
}

std::vector<json::object> Ka2Inventory::read_chunked_parallel(
    const std::string& path,
    const std::string& filter_field,
    const std::vector<std::string>& values,
    const std::string& select_fields,
    const std::atomic<bool>* stop) const
{
    std::vector<json::object> result;
    if (values.empty()) return result;

    auto chunks = chunk_vector(values, opts_.chunk_size);
    int workers = std::max(1, opts_.parallel_workers);

    if (opts_.verbose) {
        OnecRawLog::instance().add(
            "KA2 parallel start: path=" + path +
            " chunks=" + std::to_string(chunks.size()) +
            " workers=" + std::to_string(workers));
    }

    std::mutex mtx;
    std::size_t next_chunk = 0;
    std::atomic<std::size_t> done_chunks{0};

    auto worker = [&]() {
        while (true) {
            std::size_t my_idx;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (next_chunk >= chunks.size()) return;
                my_idx = next_chunk++;
            }
            if (stop && stop->load()) return;

            const auto& chunk = chunks[my_idx];
            std::string filter = build_guid_filter(filter_field, chunk);

            std::vector<std::pair<std::string, std::string>> params;
            params.emplace_back("$format", "json");
            params.emplace_back("$filter", filter);
            if (!select_fields.empty())
                params.emplace_back("$select", select_fields);

            std::size_t skip = 0;
            std::vector<json::object> local;

            while (true) {
                if (stop && stop->load()) return;
                std::string body;
                try {
                    body = read_page(path, params, skip,
                                     opts_.page_size_flat, stop);
                } catch (const std::exception& e) {
                    OnecRawLog::instance().add(
                        std::string("KA2 parallel chunk err: ") + e.what());
                    break;
                }
                auto data = json::parse(body);
                const json::array* items = extract_value(data);
                if (!items || items->empty()) break;
                for (const auto& v : *items)
                    if (v.is_object()) local.push_back(v.as_object());
                if (items->size() < opts_.page_size_flat) break;
                skip += items->size();
            }

            {
                std::lock_guard<std::mutex> lk(mtx);
                for (auto& o : local) result.push_back(std::move(o));
            }

            std::size_t n = ++done_chunks;
            if (opts_.verbose && (n % 50 == 0)) {
                OnecRawLog::instance().add(
                    "KA2 parallel progress: " + std::to_string(n) +
                    "/" + std::to_string(chunks.size()));
            }
        }
    };

    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    for (int i = 0; i < workers; ++i)
        futures.push_back(std::async(std::launch::async, worker));
    for (auto& f : futures) f.get();

    if (opts_.verbose) {
        OnecRawLog::instance().add(
            "KA2 parallel done: path=" + path +
            " total=" + std::to_string(result.size()));
    }
    return result;
}

std::vector<json::object> Ka2Inventory::read_balance_chunked(
    const std::string& register_name,
    const std::string& period,
    const std::string& dimensions,
    const std::string& filter_field,
    const std::vector<std::string>& values,
    const std::atomic<bool>* stop) const
{
    std::vector<json::object> out;
    if (values.empty()) return out;

    const std::size_t VB_CHUNK = 200;
    auto chunks = chunk_vector(values, VB_CHUNK);

    std::mutex mtx;
    std::size_t next_idx = 0;

    std::function<std::vector<json::object>(const std::vector<std::string>&)>
    process_chunk;
    process_chunk = [&](const std::vector<std::string>& chunk)
        -> std::vector<json::object>
    {
        if (stop && stop->load()) return {};

        std::string condition = build_guid_filter(filter_field, chunk);

        std::ostringstream path;
        path << register_name << "/Balance("
             << "Period=datetime'" << period << "',"
             << "Condition='" << escape_odata_string(condition) << "',"
             << "Dimensions='" << escape_odata_string(dimensions) << "')";

        std::vector<std::pair<std::string, std::string>> params;
        params.emplace_back("$format", "json");

        try {
            std::string body = read_page(path.str(), params, 0, 1000, stop);
            auto data = json::parse(body);
            const json::array* items = extract_value(data);
            std::vector<json::object> local;
            if (items) {
                for (const auto& v : *items)
                    if (v.is_object()) local.push_back(v.as_object());
            }
            return local;
        }
        catch (const HttpError& e) {
            if (e.status == 414 && chunk.size() > 1) {
                std::size_t mid = chunk.size() / 2;
                std::vector<std::string> left(chunk.begin(), chunk.begin() + mid);
                std::vector<std::string> right(chunk.begin() + mid, chunk.end());
                OnecRawLog::instance().add(
                    "KA2 balance 414: split " +
                    std::to_string(chunk.size()) + " -> " +
                    std::to_string(left.size()) + " + " +
                    std::to_string(right.size()));
                auto a = process_chunk(left);
                auto b = process_chunk(right);
                a.insert(a.end(),
                         std::make_move_iterator(b.begin()),
                         std::make_move_iterator(b.end()));
                return a;
            }
            OnecRawLog::instance().add(
                std::string("KA2 balance HttpError: ") + e.what());
            return {};
        }
        catch (const std::exception& e) {
            OnecRawLog::instance().add(
                std::string("KA2 balance error: ") + e.what());
            return {};
        }
    };

    int workers = std::max(1, opts_.parallel_workers);
    if (opts_.verbose) {
        OnecRawLog::instance().add(
            "KA2 balance parallel start: register=" + register_name +
            " chunks=" + std::to_string(chunks.size()) +
            " workers=" + std::to_string(workers));
    }

    auto worker = [&]() {
        while (true) {
            std::size_t my_idx;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (next_idx >= chunks.size()) return;
                my_idx = next_idx++;
            }
            if (stop && stop->load()) return;

            auto rows = process_chunk(chunks[my_idx]);
            {
                std::lock_guard<std::mutex> lk(mtx);
                for (auto& r : rows) out.push_back(std::move(r));
            }
        }
    };

    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    for (int i = 0; i < workers; ++i)
        futures.push_back(std::async(std::launch::async, worker));
    for (auto& f : futures) f.get();

    OnecRawLog::instance().add(
        "KA2 balance done: " + register_name +
        " chunks=" + std::to_string(chunks.size()) +
        " rows=" + std::to_string(out.size()));

    return out;
}

std::vector<Warehouse>
Ka2Inventory::read_warehouses(const std::atomic<bool>* stop) const
{
    std::vector<Warehouse> out;
    std::size_t skip = 0;

    std::vector<std::pair<std::string, std::string>> params;
    params.emplace_back("$format", "json");
    params.emplace_back("$select", "Ref_Key,Description,DeletionMark,IsFolder");

    while (true) {
        if (stop && stop->load()) break;
        std::string body;
        try {
            body = read_page("Catalog_Склады", params, skip,
                             opts_.page_size_flat, stop);
        } catch (const std::exception& e) {
            OnecRawLog::instance().add(
                std::string("KA2 warehouses err: ") + e.what());
            break;
        }
        auto data = json::parse(body);
        const json::array* items = extract_value(data);
        if (!items || items->empty()) break;

        for (const auto& v : *items) {
            if (!v.is_object()) continue;
            const auto& o = v.as_object();
            Warehouse w;
            w.ref_key       = get_str(o, "Ref_Key");
            w.description   = get_str(o, "Description");
            w.deletion_mark = get_bool(o, "DeletionMark", false);
            w.is_folder     = get_bool(o, "IsFolder", false);
            if (w.deletion_mark || w.is_folder) continue;
            if (w.ref_key.empty()) continue;
            out.push_back(std::move(w));
        }

        if (items->size() < opts_.page_size_flat) break;
        skip += items->size();
    }
    return out;
}

std::vector<AnalyticKey>
Ka2Inventory::read_analytics(const std::vector<std::string>& product_keys,
                             const std::atomic<bool>* stop) const
{
    std::vector<AnalyticKey> out;

    auto rows = read_chunked_parallel(
        "Catalog_КлючиАналитикиУчетаНоменклатуры",
        "Номенклатура_Key",
        product_keys,
        "Ref_Key,Номенклатура_Key",
        stop);

    std::unordered_set<std::string> seen;
    seen.reserve(rows.size());

    for (const auto& o : rows) {
        AnalyticKey a;
        a.ref_key          = get_str(o, "Ref_Key");
        a.nomenclature_key = get_str(o, "Номенклатура_Key");
        if (a.ref_key.empty()) continue;
        if (!seen.insert(a.ref_key).second) continue;
        out.push_back(std::move(a));
    }

    OnecRawLog::instance().add(
        "KA2 analytics: unique=" + std::to_string(out.size()));
    return out;
}

std::vector<StockBalance>
Ka2Inventory::read_stocks(const std::string& period,
                          const std::vector<std::string>& product_keys,
                          const std::atomic<bool>* stop) const
{
    std::vector<StockBalance> out;

    auto rows = read_balance_chunked(
        "AccumulationRegister_ТоварыНаСкладах",
        period,
        "Номенклатура,Характеристика,Склад",
        "Номенклатура_Key",
        product_keys,
        stop);

    for (const auto& o : rows) {
        StockBalance s;
        s.nomenclature_key   = get_str(o, "Номенклатура_Key");
        s.characteristic_key = get_str(o, "Характеристика_Key");
        s.warehouse_key      = get_str(o, "Склад_Key");
        double on_hand  = get_num(o, "ВНаличииBalance");
        double reserved = get_num(o, "КОтгрузкеBalance");
        s.quantity = std::max(0.0, on_hand - reserved);
        if (s.nomenclature_key.empty()) continue;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<CostBalance>
Ka2Inventory::read_costs(const std::string& period,
                         const std::vector<std::string>& analytic_keys,
                         const std::atomic<bool>* stop) const
{
    std::vector<CostBalance> out;
    if (analytic_keys.empty()) return out;

    auto rows = read_balance_chunked(
        "AccumulationRegister_СебестоимостьТоваров",
        period,
        "АналитикаУчетаНоменклатуры,Организация,Партия",
        "АналитикаУчетаНоменклатуры_Key",
        analytic_keys,
        stop);

    for (const auto& o : rows) {
        CostBalance c;
        c.analytic_key     = get_str(o, "АналитикаУчетаНоменклатуры_Key");
        c.organization_key = get_str(o, "Организация_Key");
        c.batch_key        = get_str(o, "Партия_Key");
        c.quantity         = get_num(o, "КоличествоBalance");
        c.cost             = get_num(o, "СтоимостьBalance");
        if (c.analytic_key.empty()) continue;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<SupplierPrice>
Ka2Inventory::read_suppliers(const std::vector<std::string>& product_keys,
                             const std::atomic<bool>* stop) const
{
    std::vector<SupplierPrice> out;

    auto rows = read_chunked_parallel(
        "InformationRegister_ЦеныНоменклатурыПоставщиков_RecordType",
        "Номенклатура_Key",
        product_keys,
        "Period,Партнер_Key,Номенклатура_Key,Цена",
        stop);

    for (const auto& o : rows) {
        SupplierPrice s;
        s.period             = get_str(o, "Period");
        s.partner_key        = get_str(o, "Партнер_Key");
        s.nomenclature_key   = get_str(o, "Номенклатура_Key");
        s.price              = get_num(o, "Цена");
        if (s.nomenclature_key.empty()) continue;
        if (s.price <= 0) continue;
        out.push_back(std::move(s));
    }
    return out;
}

PullResult Ka2Inventory::pull(
    const std::vector<std::string>& product_keys,
    const std::string& period,
    const std::atomic<bool>* stop) const
{
    PullResult result;

    result.warehouses = read_warehouses(stop);
    result.analytics  = read_analytics(product_keys, stop);

    std::vector<std::string> analytic_keys;
    analytic_keys.reserve(result.analytics.size());
    for (const auto& a : result.analytics)
        if (!a.ref_key.empty()) analytic_keys.push_back(a.ref_key);

    if (!analytic_keys.empty()) {
        result.costs = read_costs(period, analytic_keys, stop);
    } else {
        result.warnings.emplace_back(
            "Не найдены ключи аналитики учета номенклатуры — "
            "себестоимость не загружена.");
    }

    result.stocks    = read_stocks(period, product_keys, stop);
    result.suppliers = read_suppliers(product_keys, stop);

    return result;
}

} // namespace scand::onec::ka2
