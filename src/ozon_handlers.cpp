#include "ozon_handlers.hpp"
#include <boost/json.hpp>
#include <iostream>

namespace json = boost::json;

// ---------------------------------------------------------------------------
// Извлекает message_type из тела уведомления
// ---------------------------------------------------------------------------
static std::string extract_message_type(const RequestContext& ctx) {
    if (ctx.body.is_object()) {
        const auto& obj = ctx.body.as_object();
        auto it = obj.find("message_type");
        if (it != obj.end() && it->value().is_string()) {
            return json::value_to<std::string>(it->value());
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Обработка конкретного типа уведомления
// ---------------------------------------------------------------------------
static void process_ozon_notification(const std::string& msg_type,
                                      const json::value& body,
                                      SyncService& sync) {
    // --- Проверка готовности сервиса ---
    if (msg_type == "TYPE_PING") {
        std::cout << "[OZON] Ping received" << std::endl;
        return;
    }

    // --- Отправления (FBS/rFBS) ---
    if (msg_type == "TYPE_NEW_POSTING") {
        std::cout << "[OZON] New posting created" << std::endl;
        return;
    }
    if (msg_type == "TYPE_POSTING_CANCELLED") {
        std::cout << "[OZON] Posting cancelled" << std::endl;
        return;
    }
    if (msg_type == "TYPE_STATE_CHANGED") {
        std::cout << "[OZON] Posting state changed" << std::endl;
        return;
    }
    if (msg_type == "TYPE_CUTOFF_DATE_CHANGED") {
        std::cout << "[OZON] Cutoff date changed" << std::endl;
        return;
    }
    if (msg_type == "TYPE_DELIVERY_DATE_CHANGED") {
        std::cout << "[OZON] Delivery date changed" << std::endl;
        return;
    }

    // --- Отправления (FBO) ---
    if (msg_type == "TYPE_FBO_POSTING_NEW") {
        std::cout << "[OZON] New FBO posting" << std::endl;
        return;
    }
    if (msg_type == "TYPE_FBO_POSTING_CANCELLED") {
        std::cout << "[OZON] FBO posting cancelled" << std::endl;
        return;
    }
    if (msg_type == "TYPE_FBO_POSTING_STATE_CHANGED") {
        std::cout << "[OZON] FBO posting state changed" << std::endl;
        return;
    }
    if (msg_type == "TYPE_FBO_POSTING_DELIVERY_DATE_CHANGED") {
        std::cout << "[OZON] FBO delivery date changed" << std::endl;
        return;
    }
    if (msg_type == "TYPE_FBO_STOCKS_CHANGED") {
        std::cout << "[OZON] FBO stocks changed" << std::endl;
        return;
    }

    // --- Заказы ---
    if (msg_type == "TYPE_ORDER_NEW") {
        std::cout << "[OZON] New order" << std::endl;
        return;
    }
    if (msg_type == "TYPE_ORDER_CANCELLED") {
        std::cout << "[OZON] Order cancelled" << std::endl;
        return;
    }
    if (msg_type == "TYPE_ORDER_STATE_CHANGED") {
        std::cout << "[OZON] Order state changed" << std::endl;
        return;
    }

    // --- Товары ---
    if (msg_type == "TYPE_CREATE_OR_UPDATE_ITEM") {
        std::cout << "[OZON] Product created or updated" << std::endl;
        return;
    }
    if (msg_type == "TYPE_CREATE_ITEM") {
        std::cout << "[OZON] Product created (deprecated)" << std::endl;
        return;
    }
    if (msg_type == "TYPE_UPDATE_ITEM") {
        std::cout << "[OZON] Product updated (deprecated)" << std::endl;
        return;
    }

    // --- Остатки ---
    if (msg_type == "TYPE_STOCKS_CHANGED") {
        std::cout << "[OZON] Stocks changed" << std::endl;
        return;
    }

    // --- Чат ---
    if (msg_type == "TYPE_NEW_MESSAGE") {
        std::cout << "[OZON] New chat message" << std::endl;
        return;
    }
    if (msg_type == "TYPE_UPDATE_MESSAGE") {
        std::cout << "[OZON] Chat message updated" << std::endl;
        return;
    }
    if (msg_type == "TYPE_MESSAGE_READ") {
        std::cout << "[OZON] Chat message read" << std::endl;
        return;
    }
    if (msg_type == "TYPE_CHAT_CLOSED") {
        std::cout << "[OZON] Chat closed" << std::endl;
        return;
    }

    std::cout << "[OZON] Unknown message_type: " << msg_type << std::endl;
}

// ---------------------------------------------------------------------------
// Регистрация эндпоинта
// ---------------------------------------------------------------------------
void register_ozon_handlers(Router& router, SyncService& sync) {
    router.post("/ozon/notify", [&sync](const RequestContext& ctx) -> Response {
        std::string msg_type = extract_message_type(ctx);

        if (msg_type.empty()) {
            std::cout << "[OZON] Empty message_type (likely ping)" << std::endl;
            Response res;
            res.body = json::object({{"status", "ok"}});
            return res;
        }

        process_ozon_notification(msg_type, ctx.body, sync);

        Response res;
        res.body = json::object({{"status", "ok"}});
        return res;
    });
}
