#include "onec_url_normalizer.hpp"
#include "string_utils.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>

namespace {

using scand::str_utils::to_lower;
using scand::str_utils::trim;
using scand::str_utils::ends_with_ci;

// ---------------------------------------------------------------------------
// Разбор URL: scheme://host[:port]/path
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

    // authority + path
    std::string authority;
    auto slash_pos = rest.find('/');
    if (slash_pos == std::string::npos) {
        authority = rest;
        out.path  = "";
    } else {
        authority = rest.substr(0, slash_pos);
        out.path  = rest.substr(slash_pos);
    }

    // userinfo
    auto at_pos = authority.find('@');
    if (at_pos != std::string::npos) {
        out.has_userinfo = true;
        authority = authority.substr(at_pos + 1);
    }

    // hostname / port
    if (!authority.empty() && authority.front() == '[') {
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

    out.hostname = to_lower(out.hostname);

    if (!out.port.empty()) {
        for (char c : out.port) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        try {
            int p = std::stoi(out.port);
            if (p < 1 || p > 65535) return false;
        } catch (...) {
            return false;
        }
    }

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
// Loopback: 127.0.0.0/8, ::1, localhost
// ---------------------------------------------------------------------------
bool is_loopback_ip(const std::string& hostname) {
    in_addr  v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, hostname.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);
        return (h >> 24) == 127;
    }
    if (inet_pton(AF_INET6, hostname.c_str(), &v6) == 1) {
        static const uint8_t loop6[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        return std::memcmp(&v6, loop6, 16) == 0;
    }
    std::string h = to_lower(hostname);
    if (!h.empty() && h.back() == '.') h.pop_back();
    if (h == "localhost") return true;
    return false;
}

// ---------------------------------------------------------------------------
// Private / link-local / metadata / CGNAT
// ---------------------------------------------------------------------------
bool is_private_ip(const std::string& hostname) {
    in_addr  v4{};
    in6_addr v6{};

    if (inet_pton(AF_INET, hostname.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);

        if ((h >> 24) == 10) return true;                              // 10/8
        if ((h >> 24) == 172 && ((h >> 16) & 0xFF) >= 16
            && ((h >> 16) & 0xFF) <= 31) return true;                  // 172.16/12
        if ((h >> 24) == 192 && ((h >> 16) & 0xFF) == 168) return true; // 192.168/16
        if ((h >> 24) == 169 && ((h >> 16) & 0xFF) == 254) return true; // 169.254/16
        if ((h >> 24) == 100 && ((h >> 16) & 0xFF) >= 64
            && ((h >> 16) & 0xFF) <= 127) return true;                 // 100.64/10
        if ((h >> 24) == 0) return true;                               // 0/8
        return false;
    }

    if (inet_pton(AF_INET6, hostname.c_str(), &v6) == 1) {
        if ((v6.s6_addr[0] & 0xFE) == 0xFC) return true;               // fc00::/7
        if (v6.s6_addr[0] == 0xFE
            && (v6.s6_addr[1] & 0xC0) == 0x80) return true;            // fe80::/10
        return false;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Резолв hostname и проверка всех IP (DNS rebinding)
// ---------------------------------------------------------------------------
bool hostname_resolves_to_internal(const std::string& hostname) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &res) != 0)
        return true;

    bool bad = false;
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        void* addr = (p->ai_family == AF_INET)
            ? static_cast<void*>(&reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr)
            : static_cast<void*>(&reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr);

        if (!inet_ntop(p->ai_family, addr, buf, sizeof(buf))) continue;

        if (is_private_ip(buf) || is_loopback_ip(buf)) {
            bad = true;
            break;
        }
    }
    freeaddrinfo(res);
    return bad;
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
            "Адрес 1С не должен содержать логин, пароль или параметры.");
    }

    // --- Блокировка private / loopback / link-local / metadata ---
    bool is_ipv6 = false;
    bool literal = is_ip_literal(parsed.hostname, is_ipv6);

    bool local_test_target =
        parsed.scheme == "http"
        && options.allow_insecure_http
        && is_loopback_ip(parsed.hostname);

    // NEW: если включён allow_private_network — не блокируем private IP
    if (!options.allow_private_network
        && literal
        && is_private_ip(parsed.hostname)
        && !local_test_target) {
        throw OnecConnectionInputError(
            "Адрес 1С не должен указывать на внутреннюю сеть сервиса.");
    }

    // --- DNS rebinding protection ---
    // NEW: тоже уважает allow_private_network
    if (!options.allow_private_network
        && !literal
        && !local_test_target) {
        if (hostname_resolves_to_internal(parsed.hostname)) {
            throw OnecConnectionInputError(
                "Адрес 1С не должен указывать на внутреннюю сеть сервиса.");
        }
    }

    // --- HTTP разрешён только для loopback при явном флаге ---
    if (parsed.scheme == "http"
        && !is_loopback_ip(parsed.hostname)
        && !options.allow_insecure_http)
    {
        throw OnecConnectionInputError(
            "Для внешней базы 1С требуется рабочий HTTPS.");
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
