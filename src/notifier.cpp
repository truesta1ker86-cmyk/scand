#include "notifier.hpp"
#include <boost/json.hpp>
#include <cpr/cpr.h>
#include <iomanip>
#include <iostream>

namespace json = boost::json;

// ---------------------------------------------------------------------------
// WebhookNotifier
// ---------------------------------------------------------------------------
WebhookNotifier::WebhookNotifier(std::string url)
    : url_(std::move(url)) {}

void WebhookNotifier::notify(const SyncProgress& p) {
    if (url_.empty()) return;

    json::object body{
        {"source",    p.source},
        {"processed", p.processed},
        {"total",     p.total},
        {"percent",   p.percent},
        {"status",    p.status}
    };

    cpr::Response r = cpr::Post(
        cpr::Url{url_},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json::serialize(body)},
        cpr::Timeout{5000}
    );

    if (r.status_code != 200 && r.status_code != 204) {
        std::cerr << "[NOTIFY] Webhook returned HTTP "
                  << r.status_code
                  << " (" << r.error.message << ")" << std::endl;
    }
}

// ---------------------------------------------------------------------------
// LogNotifier
// ---------------------------------------------------------------------------
void LogNotifier::notify(const SyncProgress& p) {
    std::cout << "[NOTIFY] " << p.source
              << " " << p.processed << "/" << p.total
              << " (" << std::fixed << std::setprecision(1)
              << p.percent << "%)"
              << " " << p.status
              << std::endl;
}

// ---------------------------------------------------------------------------
// SseNotifier
// ---------------------------------------------------------------------------
void SseNotifier::notify(const SyncProgress& p) {
    json::object body{
        {"source",    p.source},
        {"processed", p.processed},
        {"total",     p.total},
        {"percent",   p.percent},
        {"status",    p.status}
    };

    SseBroker::instance().broadcast("progress", body);
}

// ---------------------------------------------------------------------------
// CompositeNotifier
// ---------------------------------------------------------------------------
void CompositeNotifier::add(std::shared_ptr<Notifier> n) {
    notifiers_.push_back(std::move(n));
}

void CompositeNotifier::notify(const SyncProgress& p) {
    for (auto& n : notifiers_) {
        if (n) n->notify(p);
    }
}
