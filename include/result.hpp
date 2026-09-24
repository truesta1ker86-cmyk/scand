#pragma once
#include <optional>
#include <string>
#include <utility>

namespace scand {

template<class T>
struct Result {
    std::optional<T> value;
    std::string      error;

    bool ok() const { return value.has_value(); }

    static Result success(T v)            { return { std::move(v), {} }; }
    static Result failure(std::string e)  { return { std::nullopt, std::move(e) }; }
};

struct VoidResult {
    bool        success = true;
    std::string error;

    static VoidResult ok()                 { return {}; }
    static VoidResult fail(std::string e)  { return { false, std::move(e) }; }
};

} // namespace scand
