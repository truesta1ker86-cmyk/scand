#pragma once
#include "sync_checkpoint.hpp"
#include "sse_broker.hpp"
#include <memory>
#include <string>
#include <vector>

class Notifier {
public:
    virtual ~Notifier() = default;
    virtual void notify(const SyncProgress& progress) = 0;
};

class WebhookNotifier : public Notifier {
public:
    explicit WebhookNotifier(std::string url);
    void notify(const SyncProgress& progress) override;
private:
    std::string url_;
};

class LogNotifier : public Notifier {
public:
    void notify(const SyncProgress& progress) override;
};

// --- SseNotifier: раздаёт прогресс через SseBroker ---
class SseNotifier : public Notifier {
public:
    SseNotifier() = default;
    void notify(const SyncProgress& progress) override;
};

class CompositeNotifier : public Notifier {
public:
    void add(std::shared_ptr<Notifier> n);
    void notify(const SyncProgress& progress) override;
private:
    std::vector<std::shared_ptr<Notifier>> notifiers_;
};
