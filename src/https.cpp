#include "https.h"

#include <HTTPClient.h>
#include <StreamString.h>

#include "log.h"
#include "root_ca.h"
#include "tls_client.h"

HttpResponse httpsRequest(const char* method, const String& url, const String& body,
                          const char* contentType, const String& bearerToken) {
    HttpResponse res{-1, String()};

    // Fresh TLS client per request (verifies against the tado root CAs).
    TlsClient tls(TADO_ROOT_CA);

    HTTPClient http;
    if (!http.begin(tls, url)) {
        LOG("https: begin failed");
        return res;
    }
    http.setReuse(false);  // one request per connection
    http.setTimeout(15000);
    if (contentType) {
        http.addHeader("Content-Type", contentType);
    }
    if (bearerToken.length()) {
        http.addHeader("Authorization", "Bearer " + bearerToken);
    }

    uint32_t start = millis();
    res.status = http.sendRequest(method, body);
    if (res.status > 0) {
        // No body for 204/304 or Content-Length: 0.
        bool hasBody = res.status != 204 && res.status != 304 && http.getSize() != 0;
        if (hasBody) {
            // writeToStream() handles chunked transfer encoding.
            StreamString out;
            http.writeToStream(&out);
            res.body = out;
        }
        LOG("https: %s %s -> %d (%u bytes, %lu ms)", method, url.c_str(), res.status,
            res.body.length(), (unsigned long)(millis() - start));
    } else {
        LOG("https: %s %s failed: %s", method, url.c_str(),
            HTTPClient::errorToString(res.status).c_str());
    }

    http.end();
    return res;
}
