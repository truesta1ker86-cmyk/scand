#include "ozon_client.hpp"
#include <boost/json.hpp>
#include <cpr/cpr.h>
#include <iostream>

namespace json = boost::json;

OzonClient::OzonClient(std::string client_id, std::string api_key,
                       int page_size, int timeout_ms)
    : client_id_(std::move(client_id))
    , api_key_(std::move(api_key))
    , page_size_(page_size)
    , timeout_ms_(timeout_ms) {}

// ---------------------------------------------------------------------------
// Пагинация через last_id (для синхронизации с чекпоинтом)
// ---------------------------------------------------------------------------
OzonPage OzonClient::fetch_page_cursor(const std::string& last_id) const {
    OzonPage result;

    std::string url = base_url_ + "/v3/product/list";
    std::cout << "[OZON] POST " << url
              << " (last_id=\"" << last_id << "\")" << std::endl;

    json::object body{
        {"filter", json::object{{"visibility", "ALL"}}},
        {"last_id", last_id},
        {"limit", page_size_}
    };

    cpr::Response r = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Client-Id", client_id_},
            {"Api-Key", api_key_},
            {"Content-Type", "application/json"}
        },
        cpr::Body{json::serialize(body)},
        cpr::Timeout{timeout_ms_}
    );

    // --- Сетевая ошибка (HTTP 0, 4xx, 5xx, таймаут) ---
    if (r.status_code != 200) {
        std::cerr << "[OZON ERROR] HTTP " << r.status_code
                  << " (" << r.error.message << "): "
                  << r.text.substr(0, 200) << std::endl;
        result.network_error = true;
        return result;
    }

    try {
        auto data = json::parse(r.text);
        if (data.is_object() && data.as_object().contains("result")) {
            auto& res = data.as_object().at("result").as_object();

            if (res.contains("items")) {
                for (const auto& item : res.at("items").as_array()) {
                    if (!item.is_object()) continue;
                    auto& obj = item.as_object();

                    OzonProduct p;
                    if (obj.contains("product_id") && obj.at("product_id").is_int64())
                        p.id = std::to_string(obj.at("product_id").as_int64());
                    if (obj.contains("offer_id") && obj.at("offer_id").is_string())
                        p.sku = json::value_to<std::string>(obj.at("offer_id"));

                    if (!p.id.empty()) result.items.push_back(std::move(p));
                }
            }

            if (res.contains("last_id") && res.at("last_id").is_string())
                result.last_id = json::value_to<std::string>(res.at("last_id"));
            if (res.contains("total") && res.at("total").is_int64())
                result.total = res.at("total").as_int64();
        }
    } catch (const std::exception& e) {
        std::cerr << "[OZON PARSE ERROR] " << e.what() << std::endl;
        result.network_error = true;
        return result;
    }

    result.has_more = !result.items.empty()
                   && static_cast<int>(result.items.size()) == page_size_;

    return result;
}

// ---------------------------------------------------------------------------
// По номеру страницы (для --print-ozon)
// ---------------------------------------------------------------------------
std::vector<OzonProduct> OzonClient::fetch_page(int page) const {
    std::vector<OzonProduct> products;

    std::string url = base_url_ + "/v3/product/list";
    std::cout << "[OZON] POST " << url << " (page " << page << ")" << std::endl;

    json::object body{
        {"filter", json::object{{"visibility", "ALL"}}},
        {"last_id", ""},
        {"limit", page_size_}
    };

    cpr::Response r = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Client-Id", client_id_},
            {"Api-Key", api_key_},
            {"Content-Type", "application/json"}
        },
        cpr::Body{json::serialize(body)},
        cpr::Timeout{timeout_ms_}
    );

    if (r.status_code != 200) {
        std::cerr << "[OZON ERROR] HTTP " << r.status_code << std::endl;
        return products;
    }

    try {
        auto data = json::parse(r.text);
        if (data.is_object() && data.as_object().contains("result")) {
            auto& result = data.as_object().at("result").as_object();
            if (result.contains("items")) {
                for (const auto& item : result.at("items").as_array()) {
                    if (!item.is_object()) continue;
                    auto& obj = item.as_object();

                    OzonProduct p;
                    if (obj.contains("product_id") && obj.at("product_id").is_int64())
                        p.id = std::to_string(obj.at("product_id").as_int64());
                    if (obj.contains("offer_id") && obj.at("offer_id").is_string())
                        p.sku = json::value_to<std::string>(obj.at("offer_id"));

                    if (!p.id.empty()) products.push_back(std::move(p));
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[OZON PARSE ERROR] " << e.what() << std::endl;
    }

    return products;
}

// ---------------------------------------------------------------------------
// Детали по списку product_id
// ---------------------------------------------------------------------------
std::vector<OzonProduct> OzonClient::fetch_info_batch(const std::vector<std::string>& ids) const {
    std::vector<OzonProduct> products;
    if (ids.empty()) return products;

    std::string url = base_url_ + "/v3/product/info/list";

    json::array id_array;
    for (const auto& id : ids) id_array.emplace_back(id);
    json::object body{{"product_id", id_array}};

    cpr::Response r = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Client-Id", client_id_},
            {"Api-Key", api_key_},
            {"Content-Type", "application/json"}
        },
        cpr::Body{json::serialize(body)},
        cpr::Timeout{timeout_ms_}
    );

    if (r.status_code != 200) {
        std::cerr << "[OZON INFO ERROR] HTTP " << r.status_code
                  << ": " << r.text.substr(0, 500) << std::endl;
        return products;
    }

    try {
        auto data = json::parse(r.text);
        if (data.is_object() && data.as_object().contains("items")) {
            for (const auto& item : data.as_object().at("items").as_array()) {
                if (!item.is_object()) continue;
                auto& obj = item.as_object();

                OzonProduct p;
                if (obj.contains("id") && obj.at("id").is_int64())
                    p.id = std::to_string(obj.at("id").as_int64());
                if (obj.contains("offer_id") && obj.at("offer_id").is_string())
                    p.sku = json::value_to<std::string>(obj.at("offer_id"));
                if (obj.contains("name") && obj.at("name").is_string())
                    p.name = json::value_to<std::string>(obj.at("name"));

                if (obj.contains("description_category_id") && obj.at("description_category_id").is_int64())
                    p.scategorie = std::to_string(obj.at("description_category_id").as_int64());
                else if (obj.contains("type_id") && obj.at("type_id").is_int64())
                    p.scategorie = std::to_string(obj.at("type_id").as_int64());

                if (obj.contains("weight") && obj.at("weight").is_int64())
                    p.weight = static_cast<int>(obj.at("weight").as_int64());
                if (obj.contains("created_at") && obj.at("created_at").is_string())
                    p.created_at = json::value_to<std::string>(obj.at("created_at"));
                if (obj.contains("updated_at") && obj.at("updated_at").is_string())
                    p.updated_at = json::value_to<std::string>(obj.at("updated_at"));
                if (obj.contains("description") && obj.at("description").is_string())
                    p.description = json::value_to<std::string>(obj.at("description"));

                p.price = 0.0;

                if (!p.id.empty()) products.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[OZON INFO PARSE ERROR] " << e.what() << std::endl;
    }

    return products;
}

// ---------------------------------------------------------------------------
// Цены и остатки
// ---------------------------------------------------------------------------
std::vector<OzonProduct> OzonClient::fetch_prices_stocks_batch(const std::vector<std::string>& skus) const {
    std::vector<OzonProduct> products;
    if (skus.empty()) return products;

    std::string url = base_url_ + "/v5/product/info/prices";

    json::array offer_array;
    for (const auto& sku : skus) offer_array.emplace_back(sku);

    json::object body{
        {"filter", json::object{
            {"offer_id", offer_array},
            {"visibility", "ALL"}
        }},
        {"limit", static_cast<int>(skus.size())}
    };

    cpr::Response r = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Client-Id", client_id_},
            {"Api-Key", api_key_},
            {"Content-Type", "application/json"}
        },
        cpr::Body{json::serialize(body)},
        cpr::Timeout{timeout_ms_}
    );

    if (r.status_code != 200) {
        std::cerr << "[OZON PRICES ERROR] HTTP " << r.status_code
                  << ": " << r.text.substr(0, 500) << std::endl;
        return products;
    }

    try {
        auto data = json::parse(r.text);
        if (data.is_object() && data.as_object().contains("items")) {
            for (const auto& item : data.as_object().at("items").as_array()) {
                if (!item.is_object()) continue;
                auto& obj = item.as_object();

                OzonProduct p;
                if (obj.contains("offer_id") && obj.at("offer_id").is_string())
                    p.sku = json::value_to<std::string>(obj.at("offer_id"));

                if (obj.contains("price") && obj.at("price").is_object()) {
                    auto& price_obj = obj.at("price").as_object();
                    if (price_obj.contains("price") && price_obj.at("price").is_string()) {
                        try { p.price = std::stod(json::value_to<std::string>(price_obj.at("price"))); }
                        catch (...) {}
                    }
                    if (price_obj.contains("currency_code") && price_obj.at("currency_code").is_string())
                        p.currency = json::value_to<std::string>(price_obj.at("currency_code"));
                }

                if (obj.contains("stocks") && obj.at("stocks").is_object()) {
                    auto& stocks_obj = obj.at("stocks").as_object();
                    if (stocks_obj.contains("stocks") && stocks_obj.at("stocks").is_array()) {
                        int total = 0;
                        for (const auto& s : stocks_obj.at("stocks").as_array()) {
                            if (s.is_object()
                                && s.as_object().contains("present")
                                && s.as_object().at("present").is_int64())
                            {
                                total += static_cast<int>(s.as_object().at("present").as_int64());
                            }
                        }
                        p.in_stock = total;
                    }
                }

                products.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[OZON PRICES PARSE ERROR] " << e.what() << std::endl;
    }

    return products;
}

std::optional<OzonProduct> OzonClient::fetch_product_info(const std::string& product_id) const {
    auto items = fetch_info_batch({product_id});
    if (items.empty()) return std::nullopt;
    return items.front();
}
