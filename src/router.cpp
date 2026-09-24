#include "router.hpp"
#include <sstream>

namespace {

std::string strip_query(const std::string& target) {
    auto pos = target.find('?');
    return (pos == std::string::npos) ? target : target.substr(0, pos);
}

// Парсим ?key=value&key2=value2
std::unordered_map<std::string, std::string>
parse_query(const std::string& target) {
    std::unordered_map<std::string, std::string> out;
    auto qpos = target.find('?');
    if (qpos == std::string::npos) return out;

    std::string qs = target.substr(qpos + 1);
    std::istringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        if (pair.empty()) continue;
        auto eq = pair.find('=');
        if (eq == std::string::npos) {
            out[pair] = "";
        } else {
            out[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
    }
    return out;
}

} // namespace

void Router::add(http::verb method, const std::string& pattern, Handler handler) {
    routes_.push_back(Route{
        method,
        pattern,
        split_path(pattern),
        std::move(handler)
    });
}

std::vector<std::string> Router::split_path(const std::string& path) {
    std::vector<std::string> out;
    std::stringstream ss(path);
    std::string segment;

    while (std::getline(ss, segment, '/')) {
        if (!segment.empty()) out.push_back(segment);
    }
    return out;
}

bool Router::match(const Route& route,
                   const std::vector<std::string>& segments,
                   std::unordered_map<std::string, std::string>& out_params) {
    if (route.segments.size() != segments.size()) return false;

    for (size_t i = 0; i < route.segments.size(); ++i) {
        const auto& pat = route.segments[i];
        const auto& val = segments[i];

        if (pat.size() >= 2 && pat.front() == '{' && pat.back() == '}') {
            std::string name = pat.substr(1, pat.size() - 2);
            out_params[name] = val;
        } else if (pat != val) {
            return false;
        }
    }
    return true;
}

Response Router::dispatch(http::verb method,
                          const std::string& target,
                          const http::request<http::string_body>& req) const {
    std::string path = strip_query(target);
    auto segments = split_path(path);

    // ← Парсим query один раз, используется во всех route.handler
    auto query_params = parse_query(target);

    bool path_matched_any = false;

    for (const auto& route : routes_) {
        std::unordered_map<std::string, std::string> params;
        if (!match(route, segments, params)) continue;

        path_matched_any = true;

        if (route.method != method) continue;

        // ← Передаём query_params в RequestContext
        RequestContext ctx{req, json::object(), std::move(params), query_params};

        if (!req.body().empty()) {
            try {
                ctx.body = json::parse(req.body());
            } catch (...) {
                Response bad;
                bad.status = http::status::bad_request;
                bad.body = json::object({{"error", "invalid json"}});
                return bad;
            }
        }

        return route.handler(ctx);
    }

    Response res;
    if (path_matched_any) {
        res.status = http::status::method_not_allowed;
        res.body = json::object({{"error", "method not allowed"}});
    } else {
        res.status = http::status::not_found;
        res.body = json::object({{"error", "not found"}});
    }
    return res;
}
