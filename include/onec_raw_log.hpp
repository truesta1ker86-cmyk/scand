#pragma once
#include <deque>
#include <mutex>
#include <string>

// Кольцевой буфер сырых логов обмена с 1С.
// Потокобезопасен. Хранит последние N записей.
class OnecRawLog {
public:
    static OnecRawLog& instance();

    void add(const std::string& entry);

    // Полный текст всех записей (для эндпоинта)
    std::string dump() const;

    // Последние N записей
    std::string tail(size_t n) const;

    void clear();

private:
    OnecRawLog() = default;
    mutable std::mutex        mutex_;
    std::deque<std::string>   entries_;
    static constexpr size_t   MAX_SIZE = 500;
};
