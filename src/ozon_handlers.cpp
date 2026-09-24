#include "ozon_handlers.hpp"
#include "webhook_store.hpp"
#include "sse_broker.hpp"
#include "sync_service.hpp"
#include <boost/json.hpp>
#include <iostream>

namespace json = boost::json;

namespace {

std::string extract_message_type(const RequestContext& ctx) {
    if (!ctx.body.is_object()) return "";
    const auto& obj = ctx.body.as_object();

    // Пробуем оба поля: message_type (Ozon) и event (fallback).
    for (const char* key : {"message_type", "event"}) {
        auto it = obj.find(key);
        if (it != obj.end() && it->value().is_string()) {
            return json::value_to<std::string>(it->value());
        }
    }
    return "";
}

void handle_event(const std::string& msg_type, SyncService& sync) {
    if (msg_type == "TYPE_PING") return;

    if (msg_type == "TYPE_CREATE_OR_UPDATE_ITEM"
        || msg_type == "TYPE_CREATE_ITEM"
        || msg_type == "TYPE_UPDATE_ITEM") {
        std::cout << "[OZON] Item changed -> trigger sync\n";
        sync.trigger_ozon_sync();
        return;
    }

    if (msg_type == "TYPE_STOCKS_CHANGED"
        || msg_type == "TYPE_FBO_STOCKS_CHANGED") {
        std::cout << "[OZON] Stocks changed -> trigger sync\n";
        sync.trigger_ozon_sync();
        return;
    }

    std::cout << "[OZON] Event: " << msg_type << "\n";
}

} // namespace

void register_ozon_handlers(Router& router, SyncService& sync) {
    router.post("/ozon/notify", [&sync](const RequestContext& ctx) -> Response {
        // TODO: проверка подписи Ozon (X-Ozon-Signature) — см. README.
        std::string msg_type = extract_message_type(ctx);

        // ВСЕГДА сохраняем в WebhookStore — для аудита.
        WebhookEntry e;
        e.source       = "ozon";
        e.message_type = msg_type;
        e.body         = json::serialize(ctx.body);
        WebhookStore::instance().add(std::move(e));

        // Дублируем в SSE для live-UI.
        auto last = WebhookStore::instance().last(1);
        if (!last.empty()) {
            SseBroker::instance().broadcast("hook", last[0]);
        }

        // Реагируем на событие.
        handle_event(msg_type, sync);

        Response res;
        res.body = json::object({{"status", "ok"}});
        return res;
    });
}
