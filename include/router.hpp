#pragma once
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace http = boost::beast::http;
namespace json = boost::json;

struct RequestContext {
    const http::request<http::string_body>& req;
    json::value                             body;
    std::unordered_map<std::string, std::string> path_params;
};

struct Response {
    http::status status = http::status::ok;
    json::value  body   = json::object();

    // Если raw_body не пусто — ответ отдаётся как есть,
    // с Content-Type из raw_content_type. Иначе — JSON.
    std::string  raw_body;
    std::string  raw_content_type;
};

using Handler = std::function<Response(const RequestContext&)>;

class Router {
public:
    void add(http::verb method, const std::string& pattern, Handler handler);
    void get(const std::string& pattern, Handler h)  { add(http::verb::get,  pattern, std::move(h)); }
    void post(const std::string& pattern, Handler h) { add(http::verb::post, pattern, std::move(h)); }
    void del(const std::string& pattern, Handler h)  { add(http::verb::delete_, pattern, std::move(h)); }

    Response dispatch(http::verb method,
                      const std::string& target,
                      const http::request<http::string_body>& req) const;

private:
    struct Route {
        http::verb   method;
        std::string  pattern;
        std::vector<std::string> segments;
        Handler      handler;
    };

    std::vector<Route> routes_;

    static std::vector<std::string> split_path(const std::string& path);
    static bool match(const Route& route,
                      const std::vector<std::string>& segments,
                      std::unordered_map<std::string, std::string>& out_params);
};
