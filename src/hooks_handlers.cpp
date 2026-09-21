#include "hooks_handlers.hpp"
#include "sse_broker.hpp"
#include "webhook_store.hpp"
#include <boost/json.hpp>
#include <iostream>

namespace json = boost::json;

// ---------------------------------------------------------------------------
// Определяем источник по message_type
// ---------------------------------------------------------------------------
static std::string detect_source(const std::string& msg_type) {
    if (msg_type.empty()) return "custom";
    if (msg_type.rfind("TYPE_", 0) == 0) return "ozon";
    if (msg_type == "SIGNAL" || msg_type == "product_updated"
        || msg_type == "product_created") return "onec";
    return "custom";
}

// ---------------------------------------------------------------------------
// Регистрация эндпоинтов (только JSON, без HTML)
// ---------------------------------------------------------------------------
void register_hooks_handlers(Router& router) {

    // --- Приём вебхуков ---
    router.post("/hooks", [](const RequestContext& ctx) -> Response {
        WebhookEntry e;

        if (ctx.body.is_object()) {
            const auto& obj = ctx.body.as_object();

            auto it_src = obj.find("source");
            if (it_src != obj.end() && it_src->value().is_string())
                e.source = json::value_to<std::string>(it_src->value());

            auto it_type = obj.find("message_type");
            if (it_type != obj.end() && it_type->value().is_string())
                e.message_type = json::value_to<std::string>(it_type->value());

            auto it_event = obj.find("event");
            if (it_event != obj.end() && it_event->value().is_string()
                && e.message_type.empty())
                e.message_type = json::value_to<std::string>(it_event->value());
        }

        if (e.source.empty()) e.source = detect_source(e.message_type);
        e.body = json::serialize(ctx.body);

        WebhookStore::instance().add(std::move(e));

        auto last = WebhookStore::instance().last(1);
        if (!last.empty()) {
            SseBroker::instance().broadcast("hook", last[0]);
        }

        Response res;
        res.body = json::object({{"status", "ok"}});
        return res;
    });

    // --- API: список хуков ---
    router.get("/api/hooks", [](const RequestContext&) -> Response {
        Response res;
        res.body = json::object({
            {"items", WebhookStore::instance().last(200)}
        });
        return res;
    });

    // --- API: очистка ---
    router.del("/api/hooks", [](const RequestContext&) -> Response {
        WebhookStore::instance().clear();
        SseBroker::instance().broadcast("cleared", json::object());

        Response res;
        res.body = json::object({{"status", "cleared"}});
        return res;
    });
}
