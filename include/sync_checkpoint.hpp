#pragma once
#include <string>

struct SyncCheckpoint {
    std::string source;
    std::string last_cursor;
    int         last_page    = 0;
    long long   total_synced = 0;
    int         dirty_pages  = 0;
    long long   total_expected = 0;
    int         last_notified_percent = -1;

    // Статус последней синхронизации:
    //   ""         — не начиналась
    //   "running"  — идёт
    //   "done"     — завершилась успешно
    //   "stopped"  — остановлена пользователем
    //   "retry"    — оборвалась по сетевой ошибке
    std::string last_status;
};

struct SyncProgress {
    std::string source;
    long long   processed = 0;
    long long   total     = 0;
    double      percent   = 0.0;
    std::string status;
};
