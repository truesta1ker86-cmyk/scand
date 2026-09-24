#pragma once
#include <string>

namespace scand::http {

struct RawResponse {
    long        status_code = 0;
    std::string body;
    std::string error;
    std::string url;

    bool ok() const { return status_code >= 200 && status_code < 300; }
};

}
