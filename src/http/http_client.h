#pragma once
#include <string>
#include "common/logger.h"

struct HttpPostResult {
    bool success = false;   // true only for a 2xx response
    int statusCode = 0;
    std::string error;      // set whenever success is false
};

// POSTs `jsonBody` (application/json) to `url` (http:// or https://). When
// `authToken` is non-empty, sends "Authorization: Bearer <authToken>" - the
// extension point for whatever agent<->server auth scheme gets decided later.
// Never throws; all failures are reported via the returned struct and logged.
HttpPostResult HttpPostJson(
    const std::string& url,
    const std::string& jsonBody,
    const std::string& authToken,
    int timeoutMs,
    Logger& logger);
