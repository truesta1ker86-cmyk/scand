#include "onec_catalog.hpp"
#include "onec_raw_log.hpp"
#include "onec_url_normalizer.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <atomic>
#include <boost/json.hpp>
#include <chrono>
#include <cpr/cpr.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace json = boost::json;

// ---------------------------------------------------------------------------
// Конструктор
// ---------------------------------------------------------------------------
OnecCatalog::OnecCatalog(std::string base_url,
                         std::string username,
                         std::string password,
                         int timeout_ms,
                         bool allow_insecure_http,
                         bool allow_private_network)     // NEW
    : username_(std::move(username))
    , password_(std::move(password))
    , timeout_ms_(timeout_ms)
    , allow_insecure_http_(allow_insecure_http)          // NEW
    , allow_private_network_(allow_private_network)      // NEW
{
    OnecUrlNormalizeOptions opts;
    opts.allow_insecure_http   = allow_insecure_http;
    opts.allow_private_network = allow_private_network;  // NEW

    try {
        base_url_ = normalize_onec_publication_url(base_url, opts);
    } catch (const OnecConnectionInputError& e) {
        OnecRawLog::instance().add(std::string("URL rejected: ") + e.what());
        throw;
    }

    std::string b = base_url_;
    while (!b.empty() && b.back() == '/') b.pop_back();
    root_ = b + "/odata/standard.odata";

    std::ostringstream oss;
    oss << "INIT: root=" << root_
        << " user=" << username_
        << " timeout=" << timeout_ms_ << "ms"
        << " allow_insecure_http=" << allow_insecure_http_
        << " allow_private_network=" << allow_private_network_;
    OnecRawLog::instance().add(oss.str());
}

std::string OnecCatalog::build_url(const std::string& path) const {
    if (path.empty()) return root_;
    if (path.front() == '/') return root_ + path;
    return root_ + "/" + path;
}

// ---------------------------------------------------------------------------
// Общий конструктор OData-пути страницы (устраняет дублирование)
// ---------------------------------------------------------------------------
std::string OnecCatalog::build_page_path(size_t skip, size_t top) const {
    if (top > 500) top = 500;

    return std::string("/Catalog_Номенклатура")
         + "?$format=json"
         + "&$top="    + std::to_string(top)
         + "&$skip="   + std::to_string(skip)
         + "&$select=Ref_Key,Code,Артикул,Description,НаименованиеПолное,"
           "IsFolder,DeletionMark,Parent_Key,ЕдиницаИзмерения_Key,"
           "СтавкаНДС_Key,ВесЧислитель,ВесЗнаменатель"
         + "&$orderby=Code";
}

// ---------------------------------------------------------------------------
// Сырой запрос по произвольному пути
// ---------------------------------------------------------------------------
OnecCatalog::RawResponse OnecCatalog::read_raw(const std::string& path) const {
    RawResponse result;
    result.url = build_url(path);
    OnecRawLog::instance().add("GET " + result.url);

    cpr::Response r = cpr::Get(
        cpr::Url{result.url},
        cpr::Authentication{username_, password_, cpr::AuthMode::BASIC},
        cpr::Header{{"Accept", "application/json"},
                    {"User-Agent", "O1-Control/1C-Catalog-ReadOnly"}},
        cpr::Timeout{timeout_ms_});

    result.status_code = r.status_code;
    result.body        = r.text;
    result.error       = r.error.message;
    return result;
}

// ---------------------------------------------------------------------------
// Низкоуровневое чтение одной страницы (без retry)
// ---------------------------------------------------------------------------
std::string OnecCatalog::read_page(size_t skip, size_t top) const {
    std::string path = build_page_path(skip, top);
    std::string url  = build_url(path);

    int effective_timeout = timeout_ms_;
    if (effective_timeout <= 0) effective_timeout = 10000;

    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Authentication{username_, password_, cpr::AuthMode::BASIC},
        cpr::Header{{"Accept", "application/json"},
                    {"User-Agent", "O1-Control/1C-Catalog-ReadOnly"}},
        cpr::Timeout{effective_timeout});

    if (r.status_code != 200) {
        throw std::runtime_error(
            "1C HTTP " + std::to_string(r.status_code) +
            ": " + r.text.substr(0, 300));
    }
    return r.text;
}

std::string OnecCatalog::read_raw_page(size_t skip, size_t top) const {
    return read_page(skip, top);
}

// ---------------------------------------------------------------------------
// Чтение страницы с retry + агрессивная остановка.
// ---------------------------------------------------------------------------
OnecPageResult OnecCatalog::read_page_with_retry(
    size_t skip, size_t top,
    int max_attempts, int base_delay_ms,
    std::atomic<bool>* abort_flag,
    const std::atomic<bool>* external_stop) const
{
    OnecPageResult result;
    if (top > 500) top = 500;

    std::string path = build_page_path(skip, top);
    std::string url  = build_url(path);

    int effective_timeout = timeout_ms_;
    if (effective_timeout <= 0) effective_timeout = 10000;

    int delay_ms = base_delay_ms;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        result.attempts = attempt;

        if (abort_flag && abort_flag->load()) {
            result.kind  = OnecErrorKind::Fatal;
            result.error = "aborted by user";
            return result;
        }
        if (external_stop && external_stop->load()) {
            result.kind  = OnecErrorKind::Fatal;
            result.error = "stopped by user";
            return result;
        }

        cpr::Session session;
        session.SetUrl(cpr::Url{url});
        session.SetAuth(cpr::Authentication{
            username_, password_, cpr::AuthMode::BASIC});
        session.SetHeader(cpr::Header{
            {"Accept", "application/json"},
            {"User-Agent", "O1-Control/1C-Catalog-ReadOnly"}});
        session.SetTimeout(cpr::Timeout{effective_timeout});

        session.SetLowSpeed(cpr::LowSpeed{1, 2});

        session.SetProgressCallback(cpr::ProgressCallback(
            [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                intptr_t) -> bool {
                if (abort_flag && abort_flag->load())          return false;
                if (external_stop && external_stop->load())    return false;
                return true;
            }));

        cpr::Response r = session.Get();

        if (external_stop && external_stop->load()) {
            result.kind  = OnecErrorKind::Fatal;
            result.error = "stopped by user";
            return result;
        }
        if (abort_flag && abort_flag->load()) {
            result.kind  = OnecErrorKind::Fatal;
            result.error = "aborted by user";
            return result;
        }

        result.http_status = r.status_code;

        if (r.status_code >= 200 && r.status_code < 300) {
            try {
                auto test = json::parse(r.text);
                (void)test;
                result.kind = OnecErrorKind::Ok;
                result.body = r.text;
                return result;
            } catch (const std::exception& e) {
                result.error = std::string("invalid JSON: ") + e.what();
                OnecRawLog::instance().add(
                    "RETRY attempt=" + std::to_string(attempt) +
                    " skip=" + std::to_string(skip) +
                    " JSON invalid: " + e.what());
            }
        }
        else if (r.status_code == 0) {
            result.error = "network error: " + r.error.message;
            OnecRawLog::instance().add(
                "RETRY attempt=" + std::to_string(attempt) +
                " skip=" + std::to_string(skip) +
                " network: " + r.error.message);
        }
        else if (r.status_code == 429 || r.status_code == 503) {
            result.error = "rate limit: " + std::to_string(r.status_code);
            delay_ms *= 2;
        }
        else if (r.status_code >= 500) {
            result.error = "server error: " + std::to_string(r.status_code);
        }
        else {
            result.kind  = OnecErrorKind::Fatal;
            result.error = "HTTP " + std::to_string(r.status_code) +
                           ": " + r.text.substr(0, 300);
            return result;
        }

        if (attempt < max_attempts) {
            auto sleep_end = std::chrono::steady_clock::now()
                           + std::chrono::milliseconds(delay_ms);
            while (std::chrono::steady_clock::now() < sleep_end) {
                if (abort_flag && abort_flag->load()) {
                    result.kind  = OnecErrorKind::Fatal;
                    result.error = "aborted by user";
                    return result;
                }
                if (external_stop && external_stop->load()) {
                    result.kind  = OnecErrorKind::Fatal;
                    result.error = "stopped by user";
                    return result;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            delay_ms = std::min(delay_ms * 2, 60000);
        }
    }

    result.kind = OnecErrorKind::Retryable;
    OnecRawLog::instance().add(
        "FAILED skip=" + std::to_string(skip) +
        " after " + std::to_string(max_attempts) + " attempts");
    return result;
}

// ---------------------------------------------------------------------------
// Вспомогательные: парсинг
// ---------------------------------------------------------------------------
namespace {

std::string get_str(const json::object& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return "";
    const auto& v = it->value();
    if (v.is_string())  return json::value_to<std::string>(v);
    if (v.is_bool())    return v.as_bool() ? "true" : "false";
    if (v.is_int64())   return std::to_string(v.as_int64());
    if (v.is_uint64())  return std::to_string(v.as_uint64());
    if (v.is_double())  return std::to_string(v.as_double());
    if (v.is_null())    return "";
    return "";
}

std::vector<OnecCatalogRow> parse_rows_impl(const std::string& json_text) {
    std::vector<OnecCatalogRow> rows;
    auto data = json::parse(json_text);

    const json::array* items = nullptr;
    if (data.is_object()) {
        auto it = data.as_object().find("value");
        if (it != data.as_object().end() && it->value().is_array())
            items = &it->value().as_array();
    } else if (data.is_array()) {
        items = &data.as_array();
    }

    if (!items) return rows;

    rows.reserve(items->size());
    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        const auto& obj = item.as_object();

        OnecCatalogRow row;
        row.ref_key       = get_str(obj, "Ref_Key");
        row.code          = get_str(obj, "Code");
        row.article       = get_str(obj, "Артикул");
        row.name          = get_str(obj, "Description");
        row.full_name     = get_str(obj, "НаименованиеПолное");
        row.is_folder     = get_str(obj, "IsFolder");
        row.deletion_mark = get_str(obj, "DeletionMark");
        row.parent_key    = get_str(obj, "Parent_Key");
        row.unit_key      = get_str(obj, "ЕдиницаИзмерения_Key");
        row.vat_key       = get_str(obj, "СтавкаНДС_Key");
        row.weight_num    = get_str(obj, "ВесЧислитель");
        row.weight_den    = get_str(obj, "ВесЗнаменатель");
        row.raw_json      = json::serialize(item);

        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

std::vector<OnecCatalogRow> OnecCatalog::parse_rows_static(
    const std::string& json_text)
{
    return parse_rows_impl(json_text);
}

// ---------------------------------------------------------------------------
// Общее количество записей — три варианта OData
// ---------------------------------------------------------------------------
std::optional<size_t> OnecCatalog::read_total_count() const {
    for (int variant = 1; variant <= 3; ++variant) {
        std::string path;
        std::string tag;
        if (variant == 1) {
            path = "/Catalog_Номенклатура?$format=json&$top=1&$count=true";
            tag  = "v4";
        } else if (variant == 2) {
            path = "/Catalog_Номенклатура?$format=json&$top=1&$inlinecount=allpages";
            tag  = "v3";
        } else {
            path = "/Catalog_Номенклатура/$count";
            tag  = "/$count";
        }

        std::string url = build_url(path);
        OnecRawLog::instance().add("COUNT " + tag + " GET " + url);

        cpr::Response r = cpr::Get(
            cpr::Url{url},
            cpr::Authentication{username_, password_, cpr::AuthMode::BASIC},
            cpr::Header{{"Accept", "application/json"},
                        {"User-Agent", "O1-Control/1C-Catalog-ReadOnly"}},
            cpr::Timeout{timeout_ms_});

        if (r.status_code != 200) continue;

        try {
            if (variant == 3) {
                return static_cast<size_t>(std::stoull(r.text));
            }

            auto data = json::parse(r.text);
            if (!data.is_object()) continue;

            for (const char* key : {"@odata.count", "odata.count", "count"}) {
                auto it = data.as_object().find(key);
                if (it == data.as_object().end()) continue;
                auto v = it->value();
                if (v.is_int64())  return static_cast<size_t>(v.as_int64());
                if (v.is_uint64()) return static_cast<size_t>(v.as_uint64());
                if (v.is_string()) {
                    try { return static_cast<size_t>(std::stoull(json::value_to<std::string>(v))); }
                    catch (...) {}
                }
            }
        } catch (const std::exception& e) {
            OnecRawLog::instance().add(
                std::string("COUNT parse: ") + e.what());
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Все страницы / все строки
// ---------------------------------------------------------------------------
std::vector<std::string> OnecCatalog::read_all_pages(
    const OnecCatalogLimits& limits) const
{
    std::vector<std::string> pages;
    size_t offset = 0;
    while (offset < limits.max_rows) {
        size_t remaining = limits.max_rows - offset;
        size_t this_page = std::min(limits.page_size, remaining);

        std::string page_json = read_page(offset, this_page);
        if (page_json.empty()) break;
        pages.push_back(std::move(page_json));

        offset += this_page;
    }
    return pages;
}

std::vector<OnecCatalogRow> OnecCatalog::read_all_rows(
    const OnecCatalogLimits& limits) const
{
    std::vector<OnecCatalogRow> all;
    size_t offset = 0;
    while (offset < limits.max_rows) {
        size_t remaining = limits.max_rows - offset;
        size_t this_page = std::min(limits.page_size, remaining);

        std::string page_json = read_page(offset, this_page);
        auto rows = parse_rows_impl(page_json);
        if (rows.empty()) break;

        all.insert(all.end(),
                   std::make_move_iterator(rows.begin()),
                   std::make_move_iterator(rows.end()));

        if (rows.size() < this_page) break;
        offset += rows.size();
    }
    return all;
}
