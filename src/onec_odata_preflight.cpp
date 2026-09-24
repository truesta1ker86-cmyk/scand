#include "onec_odata_preflight.hpp"
#include "onec_url_normalizer.hpp"
#include "onec_raw_log.hpp"

#include <cpr/cpr.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>

namespace scand::onec::odata {

namespace {

std::string now_iso8601_utc() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Проверка: IP глобальный (не loopback/private/link-local/metadata/CGNAT).
// ---------------------------------------------------------------------------
bool is_global_ip(const std::string& ip) {
    in_addr v4{};
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);
        uint8_t a = (h >> 24) & 0xFF;
        uint8_t b = (h >> 16) & 0xFF;

        if (a == 10) return false;
        if (a == 172 && b >= 16 && b <= 31) return false;
        if (a == 192 && b == 168) return false;
        if (a == 127) return false;
        if (a == 169 && b == 254) return false;
        if (a == 100 && b >= 64 && b <= 127) return false;
        if (a == 0) return false;
        if (a >= 224 && a <= 239) return false;
        if (a >= 240) return false;
        return true;
    }

    in6_addr v6{};
    if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) {
        static const uint8_t loop6[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        if (std::memcmp(&v6, loop6, 16) == 0) return false;

        if ((v6.s6_addr[0] & 0xFE) == 0xFC) return false;
        if (v6.s6_addr[0] == 0xFE
            && (v6.s6_addr[1] & 0xC0) == 0x80) return false;

        static const uint8_t v4mapped[12] = {0,0,0,0, 0,0,0,0, 0,0,0xFF,0xFF};
        if (std::memcmp(&v6, v4mapped, 12) == 0) {
            std::string v4str;
            for (int i = 12; i < 16; ++i) {
                v4str += std::to_string(v6.s6_addr[i]);
                if (i < 15) v4str += ".";
            }
            return is_global_ip(v4str);
        }

        return true;
    }

    return false;
}

// NEW: проверка, что IP — private / loopback / link-local.
// Используется для allow_private_network: пропускаем, только если это
// внутренний адрес, но не metadata / multicast / reserved.
bool is_private_or_loopback(const std::string& ip) {
    in_addr v4{};
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);
        uint8_t a = (h >> 24) & 0xFF;
        uint8_t b = (h >> 16) & 0xFF;

        if (a == 10) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        if (a == 192 && b == 168) return true;
        if (a == 127) return true;
        if (a == 169 && b == 254) return true;
        if (a == 100 && b >= 64 && b <= 127) return true;
        return false;
    }

    in6_addr v6{};
    if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) {
        static const uint8_t loop6[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        if (std::memcmp(&v6, loop6, 16) == 0) return true;
        if ((v6.s6_addr[0] & 0xFE) == 0xFC) return true;
        if (v6.s6_addr[0] == 0xFE
            && (v6.s6_addr[1] & 0xC0) == 0x80) return true;
        return false;
    }

    return false;
}

std::vector<std::string> resolve_hostname(const std::string& host, int port) {
    std::vector<std::string> out;
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                    &hints, &res) != 0) {
        return out;
    }

    for (auto* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        void* addr = (p->ai_family == AF_INET)
            ? static_cast<void*>(&reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr)
            : static_cast<void*>(&reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr);

        if (inet_ntop(p->ai_family, addr, buf, sizeof(buf))) {
            out.emplace_back(buf);
        }
    }
    freeaddrinfo(res);
    return out;
}

bool is_loopback_hostname(const std::string& host) {
    if (host == "localhost") return true;
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        uint32_t h = ntohl(v4.s_addr);
        return (h >> 24) == 127;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        static const uint8_t loop6[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        return std::memcmp(&v6, loop6, 16) == 0;
    }
    return false;
}

struct HostPort { std::string host; int port = 80; };

HostPort parse_host_port(const std::string& url) {
    HostPort hp;
    auto pos = url.find("://");
    if (pos == std::string::npos) return hp;
    std::string rest = url.substr(pos + 3);
    hp.port = (url.rfind("https://", 0) == 0) ? 443 : 80;

    auto slash = rest.find('/');
    std::string authority = (slash == std::string::npos)
                            ? rest : rest.substr(0, slash);

    if (!authority.empty() && authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string::npos) return hp;
        hp.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            try { hp.port = std::stoi(authority.substr(close + 2)); } catch (...) {}
        }
        return hp;
    }

    auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
        hp.host = authority;
    } else {
        hp.host = authority.substr(0, colon);
        try { hp.port = std::stoi(authority.substr(colon + 1)); } catch (...) {}
    }
    return hp;
}

std::pair<size_t, std::vector<std::string>>
count_entity_sets(const std::string& xml) {
    std::regex re(R"(<(?:[A-Za-z0-9_]+:)?EntitySet\b[^>]*\bName=["']([A-Za-z0-9_.\-]{1,128})["'])",
                  std::regex::icase);

    std::vector<std::string> names;
    size_t count = 0;

    auto begin = std::sregex_iterator(xml.begin(), xml.end(), re);
    auto end   = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        ++count;
        if (names.size() < 24) names.push_back((*it)[1].str());
    }

    return {count, names};
}

bool looks_like_edmx(const std::string& content) {
    static const std::regex re(R"(<(?:[A-Za-z0-9_]+:)?Edmx\b)",
                               std::regex::icase);
    return std::regex_search(content, re);
}

} // namespace

// ===========================================================================
// assert_public_destination
// ===========================================================================
void assert_public_destination(
    const std::string& publication_url,
    bool allow_local_loopback,
    bool allow_private_network)
{
    HostPort hp = parse_host_port(publication_url);
    if (hp.host.empty()) {
        throw OnecConnectionInputError(
            "Не удалось определить сетевой адрес публикации 1С.");
    }

    if (allow_local_loopback && is_loopback_hostname(hp.host)) {
        return;
    }

    in_addr v4{};
    in6_addr v6{};
    bool is_literal =
        inet_pton(AF_INET, hp.host.c_str(), &v4) == 1 ||
        inet_pton(AF_INET6, hp.host.c_str(), &v6) == 1;

    std::vector<std::string> addresses;
    if (is_literal) {
        addresses.push_back(hp.host);
    } else {
        addresses = resolve_hostname(hp.host, hp.port);
    }

    if (addresses.empty()) {
        throw OnecConnectionInputError(
            "Не удалось определить сетевой адрес публикации 1С.");
    }

    for (const auto& addr : addresses) {
        if (is_global_ip(addr)) continue;

        // NEW: при allow_private_network пропускаем private / loopback
        if (allow_private_network && is_private_or_loopback(addr)) {
            continue;
        }

        throw OnecConnectionInputError(
            "Адрес 1С не должен указывать на внутреннюю сеть сервиса.");
    }
}

// ===========================================================================
// probe_onec_odata
// ===========================================================================
ProbeResult probe_onec_odata(
    const std::string& base_url,
    const std::string& username,
    const std::string& password,
    bool allow_insecure_http,
    int  timeout_ms,
    bool allow_private_network)
{
    ProbeResult result;

    auto started = std::chrono::steady_clock::now();
    auto elapsed_ms = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    };

    // 1. Нормализация URL
    OnecUrlNormalizeOptions url_opts;
    url_opts.allow_insecure_http   = allow_insecure_http;
    url_opts.allow_private_network = allow_private_network;

    std::string normalized;
    try {
        normalized = normalize_onec_publication_url(base_url, url_opts);
    } catch (const std::exception& e) {
        result.message = e.what();
        return result;
    }

    result.secure_transport = (normalized.rfind("https://", 0) == 0);
    result.base_url         = normalized;
    result.username         = username;

    // 2. SSRF-защита (DNS rebinding)
    try {
        assert_public_destination(normalized,
                                  allow_insecure_http,
                                  allow_private_network);
    } catch (const std::exception& e) {
        result.message         = e.what();
        result.last_checked_at = now_iso8601_utc();
        return result;
    }

    // 3. URL к $metadata
    std::string meta_url = normalized;
    if (!meta_url.empty() && meta_url.back() != '/') meta_url += '/';
    meta_url += "odata/standard.odata/$metadata";

    OnecRawLog::instance().add("KA2 PREFLIGHT GET " + meta_url);

    // 4. GET
    cpr::Response r = cpr::Get(
        cpr::Url{meta_url},
        cpr::Authentication{username, password, cpr::AuthMode::BASIC},
        cpr::Header{{"Accept", "application/xml, text/xml;q=0.9"},
                    {"User-Agent", "scand/1C-OData-Preflight"}},
        cpr::Timeout{timeout_ms});

    result.latency_ms      = elapsed_ms();
    result.status_code     = r.status_code;
    result.last_checked_at = now_iso8601_utc();

    if (r.status_code == 0) {
        result.message = "1С не ответила за " +
                         std::to_string(timeout_ms / 1000) + " секунд.";
        return result;
    }
    if (r.status_code == 401) {
        result.message = "1С не приняла логин или пароль.";
        return result;
    }
    if (r.status_code == 403) {
        result.message = "Пользователю 1С не разрешено чтение OData.";
        return result;
    }
    if (r.status_code == 404) {
        result.message = "Стандартный OData-интерфейс 1С не опубликован.";
        return result;
    }
    if (r.status_code >= 300 && r.status_code < 400) {
        result.message = "1С вернула перенаправление; укажите конечный URL.";
        return result;
    }
    if (r.status_code >= 400) {
        result.message = "1С вернула HTTP " +
                         std::to_string(r.status_code) + ".";
        return result;
    }

    // 5. Размер и парсинг
    if (r.text.size() > 25 * 1024 * 1024) {
        result.message = "Описание OData 1С превышает допустимый размер.";
        return result;
    }

    if (!looks_like_edmx(r.text)) {
        result.message = "1С вернула ответ, не похожий на OData metadata.";
        return result;
    }

    auto [count, names] = count_entity_sets(r.text);
    result.metadata_entity_sets = count;
    result.resource_preview     = std::move(names);

    if (count == 0) {
        result.message =
            "OData 1С доступна, но у пользователя нет доступных ресурсов.";
        return result;
    }

    result.configured = true;
    result.ready      = true;
    result.message    = result.secure_transport
        ? "Подключение к OData 1С работает."
        : "HTTP-режим: OData 1С доступна без шифрования.";
    result.message += " Доступно ресурсов: " + std::to_string(count) + ".";

    return result;
}

} // namespace scand::onec::odata
