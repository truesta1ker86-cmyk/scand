#include "onec_client.hpp"
#include <boost/json.hpp>
#include <cpr/cpr.h>
#include <iostream>

namespace json = boost::json;

// ---------------------------------------------------------------------------
// Конструктор
// ---------------------------------------------------------------------------
OnecClient::OnecClient(std::string base_url, std::string user, std::string password,
                       int page_size, int timeout_ms,
                       bool allow_insecure_http,
                       bool allow_private_network)
    : base_url_(std::move(base_url))
    , user_(std::move(user))
    , password_(std::move(password))
    , page_size_(page_size)
    , timeout_ms_(timeout_ms)
    , allow_insecure_http_(allow_insecure_http)
    , allow_private_network_(allow_private_network)
{
    // Preflight не вызывается в этом классе (нет <onec_odata_preflight>),
    // но флаги сохраняются: если решите добавить проверку — используйте их.
    // Например:
    //
    //   scand::onec::odata::probe_onec_odata(
    //       base_url_, user_, password_,
    //       allow_insecure_http_,
    //       timeout_ms_,
    //       allow_private_network_);
}

// ---------------------------------------------------------------------------
// Страница товаров 1С
// ---------------------------------------------------------------------------
std::vector<Product1C> OnecClient::fetch_page(int page) const {
    std::vector<Product1C> products;

    std::string url = base_url_ + "/products?page="
                    + std::to_string(page)
                    + "&size=" + std::to_string(page_size_);

    std::cout << "[1C] GET " << url << std::endl;

    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Authentication{user_, password_, cpr::AuthMode::BASIC},
        cpr::Timeout{timeout_ms_}
    );

    if (r.status_code != 200) {
        std::cerr << "[1C ERROR] HTTP " << r.status_code << std::endl;
        return products;
    }

    try {
        auto data = json::parse(r.text);
        const json::array* items = nullptr;

        if (data.is_object() && data.as_object().contains("items"))
            items = &data.as_object().at("items").as_array();
        else if (data.is_array())
            items = &data.as_array();

        if (items) {
            for (const auto& item : *items) {
                if (!item.is_object()) continue;
                auto& obj = item.as_object();

                Product1C p;
                p.id       = obj.contains("id")       ? json::value_to<std::string>(obj.at("id"))       : "";
                p.offer_id = obj.contains("offer_id") ? json::value_to<std::string>(obj.at("offer_id")) : "";
                p.name     = obj.contains("name")     ? json::value_to<std::string>(obj.at("name"))     : "";
                p.price    = obj.contains("price")    ? json::value_to<double>(obj.at("price"))         : 0.0;

                if (!p.id.empty()) products.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[1C PARSE ERROR] " << e.what() << std::endl;
    }

    return products;
}

// ---------------------------------------------------------------------------
// Один товар 1С
// ---------------------------------------------------------------------------
std::optional<Product1C> OnecClient::fetch_product(const std::string& id) const {
    std::string url = base_url_ + "/products/" + id;

    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Authentication{user_, password_, cpr::AuthMode::BASIC},
        cpr::Timeout{timeout_ms_}
    );

    if (r.status_code != 200) return std::nullopt;

    try {
        auto data = json::parse(r.text);
        const json::object* obj = nullptr;
        if (data.is_object()) obj = &data.as_object();
        else if (data.is_array() && !data.as_array().empty())
            obj = &data.as_array().front().as_object();

        if (!obj) return std::nullopt;

        Product1C p;
        p.id       = obj->contains("id")       ? json::value_to<std::string>(obj->at("id"))       : id;
        p.offer_id = obj->contains("offer_id") ? json::value_to<std::string>(obj->at("offer_id")) : "";
        p.name     = obj->contains("name")     ? json::value_to<std::string>(obj->at("name"))     : "";
        p.price    = obj->contains("price")    ? json::value_to<double>(obj->at("price"))         : 0.0;

        return p;
    } catch (const std::exception& e) {
        std::cerr << "[1C PARSE ERROR] " << e.what() << std::endl;
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Сырой ответ от 1С (для отладки)
// ---------------------------------------------------------------------------
OnecClient::RawResponse OnecClient::fetch_raw(const std::string& path) const {
    RawResponse result;

    std::string url = base_url_ + path;
    result.url = url;

    std::cout << "[1C RAW] GET " << url << std::endl;

    cpr::Response r = cpr::Get(
        cpr::Url{url},
        cpr::Authentication{user_, password_, cpr::AuthMode::BASIC},
        cpr::Timeout{timeout_ms_}
    );

    result.status_code = r.status_code;
    result.body        = r.text;
    result.error       = r.error.message;

    std::cout << "[1C RAW] HTTP " << r.status_code
              << ", " << r.text.size() << " bytes" << std::endl;

    return result;
}
