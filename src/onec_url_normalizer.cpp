#include "onec_url_normalizer.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>

namespace {

// ---------------------------------------------------------------------------
// Утилиты
// ---------------------------------------------------------------------------

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

// ---------------------------------------------------------------------------
// Разбор URL: scheme://host[:port]/path
// Возвращает false, если не удалось распарсить.
// ---------------------------------------------------------------------------
struct ParsedUrl {
    std::string scheme;
    std::string hostname;
    std::string port;     // "" если не указан
    std::string path;
    bool        has_userinfo = false;
    bool        has_query    = false;
    bool        has_fragment = false;
};

bool parse_url(const std::string& url, ParsedUrl& out) {
    // scheme
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;

    out.scheme = to_lower(url.substr(0, scheme_end));
    if (out.scheme != "http" && out.scheme != "https") return false;

    std::string rest = url.substr(scheme_end + 3);

    // fragment
    auto frag_pos = rest.find('#');
    if (frag_pos != std::string::npos) {
        out.has_fragment = true;
        rest = rest.substr(0, frag_pos);
    }

    // query
    auto query_pos = rest.find('?');
    if (query_pos != std::string::npos) {
        out.has_query = true;
        rest = rest.substr(0, query_pos);
    }

    // authority (host[:port]) и path
    std::string authority;
    auto slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) {
        authority = rest;
        out.path = "";
    } else {
        authority = rest.substr(0, slash_pos);
        out.path = rest.substr(slash_pos);
    }

    // userinfo
    auto at_pos = authority.find('@');
    if (at_pos != std::string::npos) {
        out.has_userinfo = true;
        authority = authority.substr(at_pos + 1);  // игнорируем user:pass
    }

    // hostname / port
    if (!authority.empty() && authority.front() == '[') {
        // IPv6 literal [::1]:80
        auto close = authority.find(']');
        if (close == std::string::npos) return false;
        out.hostname = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            out.port = authority.substr(close + 2);
        }
    } else {
        auto colon_pos = authority.rfind(':');
        if (colon_pos != std::string::npos) {
            out.hostname = authority.substr(0, colon_pos);
            out.port     = authority.substr(colon_pos + 1);
        } else {
            out.hostname = authority;
        }
    }

    if (out.hostname.empty()) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Проверка, является ли hostname литеральным IP
// ---------------------------------------------------------------------------
bool is_ip_literal(const std::string& hostname, bool& is_ipv6) {
    in_addr  v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, hostname.c_str(), &v4) == 1) {
        is_ipv6 = false;
        return true;
    }
    if (inet_pton(AF_INET6, hostname.c_str(), &v6) == 1) {
        is_ipv6 = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Проверка: loopback (127.0.0.0/8, ::1)
// ---------------------------------------------------------------------------
bool is_loopback_ip(const std::string& hostname) {
    in_addr  v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, hostname.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);
        return (h >> 24) == 127;
    }
    if (inet_pton(AF_INET6, hostname.c_str(), &v6) == 1) {
        // ::1
        static const uint8_t loop6[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        return std::memcmp(&v6, loop6, 16) == 0;
    }
    if (hostname == "localhost") return true;
    return false;
}

// ---------------------------------------------------------------------------
// Проверка: private / link-local / metadata IP
// ---------------------------------------------------------------------------
bool is_private_ip(const std::string& hostname) {
    in_addr  v4{};
    in6_addr v6{};

    if (inet_pton(AF_INET, hostname.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);

        // 10.0.0.0/8
        if ((h >> 24) == 10) return true;

        // 172.16.0.0/12
        if ((h >> 24) == 172 && ((h >> 16) & 0xFF) >= 16 && ((h >> 16) & 0xFF) <= 31)
            return true;

        // 192.168.0.0/16
        if ((h >> 24) == 192 && ((h >> 16) & 0xFF) == 168) return true;

        // 169.254.0.0/16 — link-local + AWS/GCP metadata
        if ((h >> 24) == 169 && ((h >> 16) & 0xFF) == 254) return true;

        // 100.64.0.0/10 — CGNAT
        if ((h >> 24) == 100 && ((h >> 16) & 0xFF) >= 64 && ((h >> 16) & 0xFF) <= 127)
            return true;

        // 0.0.0.0/8
        if ((h >> 24) == 0) return true;

        return false;
    }

    if (inet_pton(AF_INET6, hostname.c_str(), &v6) == 1) {
        // fc00::/7 — unique local
        if ((v6.s6_addr[0] & 0xFE) == 0xFC) return true;

        // fe80::/10 — link-local
        if (v6.s6_addr[0] == 0xFE && (v6.s6_addr[1] & 0xC0) == 0x80) return true;

        return false;
    }

    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Основная функция
// ---------------------------------------------------------------------------
std::string normalize_onec_publication_url(
    const std::string& raw_url,
    const OnecUrlNormalizeOptions& options)
{
    std::string value = trim(raw_url);

    if (value.empty() || value.size() > 1024) {
        throw OnecConnectionInputError("Укажите корректный адрес публикации 1С.");
    }

    ParsedUrl parsed;
    if (!parse_url(value, parsed)) {
        throw OnecConnectionInputError("Укажите полный адрес публикации 1С.");
    }

    if (parsed.has_userinfo || parsed.has_query || parsed.has_fragment) {
        throw OnecConnectionInputError(
            "Адрес 1С не должен содержать логин, пароль или параметры."
        );
    }

    // --- Блокировка private / loopback / link-local / metadata ---
    bool is_ipv6 = false;
    bool literal = is_ip_literal(parsed.hostname, is_ipv6);

    bool local_test_target =
        parsed.scheme == "http"
        && options.allow_insecure_http
        && is_loopback_ip(parsed.hostname);

    if (literal && is_private_ip(parsed.hostname) && !local_test_target) {
        throw OnecConnectionInputError(
            "Адрес 1С не должен указывать на внутреннюю сеть сервиса."
        );
    }

    // --- HTTP разрешён только для loopback при явном флаге ---
    if (parsed.scheme == "http"
        && !is_loopback_ip(parsed.hostname)
        && !options.allow_insecure_http)
    {
        throw OnecConnectionInputError(
            "Для внешней базы 1С требуется рабочий HTTPS."
        );
    }

    // --- Убираем trailing "/" из path ---
    std::string path = parsed.path;
    while (!path.empty() && path.back() == '/') path.pop_back();

    // --- Обрезаем суффиксы OData ---
    static const char* suffixes[] = {
        "/odata/standard.odata/$metadata",
        "/odata/standard.odata"
    };
    for (const char* suffix : suffixes) {
        if (ends_with_ci(path, suffix)) {
            path = path.substr(0, path.size() - std::strlen(suffix));
            break;
        }
    }

    // --- Собираем обратно ---
    std::ostringstream out;
    out << parsed.scheme << "://" << parsed.hostname;
    if (!parsed.port.empty()) out << ":" << parsed.port;
    out << path << "/";

    return out.str();
}
