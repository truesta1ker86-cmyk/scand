#pragma once
#include <stdexcept>
#include <string>

// Исключение для ошибок валидации URL 1С
class OnecConnectionInputError : public std::runtime_error {
public:
    explicit OnecConnectionInputError(const std::string& msg)
        : std::runtime_error(msg) {}
};

struct OnecUrlNormalizeOptions {
    bool allow_insecure_http   = false;
    bool allow_private_network = false;   // NEW: разрешить private IP (192.168/10/172.16-31)
};

// Нормализовать URL публикации 1С.
// Бросает OnecConnectionInputError при ошибке.
std::string normalize_onec_publication_url(
    const std::string& raw_url,
    const OnecUrlNormalizeOptions& options = {}
);