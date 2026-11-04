#pragma once

#include <Arduino.h>

struct HttpResponse {
    int status;   // HTTP status code, or a negative HTTPClient error code
    String body;
};

// Perform an HTTPS request, verifying the server against TADO_ROOT_CA.
// `contentType` and `bearerToken` may be nullptr/empty.
HttpResponse httpsRequest(const char* method, const String& url, const String& body = String(),
                          const char* contentType = nullptr, const String& bearerToken = String());
