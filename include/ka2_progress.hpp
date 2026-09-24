#pragma once
#include "sse_broker.hpp"

#include <boost/json.hpp>
#include <string>

namespace scand::onec::ka2 {

inline void emit_progress(const std::string& stage,
                          long long processed,
                          long long total,
                          const std::string& status,
                          const std::string& message = "")
{
    namespace json = boost::json;

    double percent = 0.0;
    if (total > 0) {
        percent = 100.0 * static_cast<double>(processed)
                / static_cast<double>(total);
    }

    json::object body{
        {"source",    "ka2"},
        {"stage",     stage},
        {"processed", processed},
        {"total",     total},
        {"percent",   percent},
        {"status",    status},
    };
    if (!message.empty()) body["message"] = message;

    try {
        SseBroker::instance().broadcast("ka2_progress", body);
    } catch (...) {}
}

} // namespace scand::onec::ka2
