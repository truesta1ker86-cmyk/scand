#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scand::onec::odata {

struct ProbeResult {
    bool                     configured           = false;
    bool                     ready                = false;
    bool                     secure_transport     = false;
    std::optional<long>      status_code;
    size_t                   metadata_entity_sets = 0;
    int64_t                  latency_ms           = 0;
    std::string              message;
    std::vector<std::string> resource_preview;
    std::string              base_url;
    std::string              username;
    std::string              last_checked_at;
};

// Нормализует URL, проверяет SSRF (DNS rebinding), делает GET к $metadata,
// парсит <EntitySet>. Один раз — на весь preflight.
//
// allow_private_network = true разрешает адрес 1С в частной сети
// (192.168/16, 10/8, 172.16/12, link-local, loopback).
ProbeResult probe_onec_odata(
    const std::string& base_url,
    const std::string& username,
    const std::string& password,
    bool allow_insecure_http,
    int  timeout_ms = 20000,
    bool allow_private_network = false);   // NEW

// SSRF-защита: резолвит hostname, проверяет что ВСЕ адреса глобальные.
// Бросает OnecConnectionInputError, если что-то внутреннее.
//
// allow_private_network = true разрешает частные адреса.
void assert_public_destination(
    const std::string& publication_url,
    bool allow_local_loopback,
    bool allow_private_network = false);   // NEW

} // namespace scand::onec::odata