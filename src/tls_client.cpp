#include "tls_client.h"

#include <WiFi.h>

extern "C" {
#include <mbedtls/net.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
}

#include "log.h"

// src/ must see the same mbedTLS config as the SDK library (custom_options in
// platformio.ini), otherwise the struct layouts below would not match.
#if !defined(MBEDTLS_SSL_SERVER_NAME_INDICATION) || !defined(MBEDTLS_ECDSA_C)
#error "mbedTLS config overrides from platformio.ini are not applied to src/"
#endif

struct TlsClientState {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
};

namespace {

int rng(void*, unsigned char* out, size_t len) {
    lt_rand_bytes(out, len);
    return 0;
}

}  // namespace

TlsClient::TlsClient(const char* caCert) : WiFiClient(), caCert_(caCert) {}

TlsClient::~TlsClient() {
    stop();
}

void TlsClient::freeState() {
    if (!s_) return;
    mbedtls_ssl_free(&s_->ssl);
    mbedtls_ssl_config_free(&s_->conf);
    mbedtls_x509_crt_free(&s_->ca);
    free(s_);
    s_ = nullptr;
}

void TlsClient::stop() {
    if (s_ && !eof_) {
        mbedtls_ssl_close_notify(&s_->ssl);  // best effort
    }
    freeState();
    WiFiClient::stop();
    sockFd_ = -1;
    eof_ = false;
    peeked_ = -1;
}

int TlsClient::connect(IPAddress ip, uint16_t port) {
    return 0;  // a host name is required to verify the certificate
}

int TlsClient::connect(IPAddress ip, uint16_t port, int32_t timeout) {
    return 0;
}

int TlsClient::connect(const char* host, uint16_t port) {
    return connect(host, port, (int32_t)5000);
}

int TlsClient::connect(const char* host, uint16_t port, int32_t timeout) {
    stop();

    IPAddress addr = WiFi.hostByName(host);
    if (!(uint32_t)addr) {
        LOG("tls: DNS lookup for %s failed", host);
        return 0;
    }
    if (WiFiClient::connect(addr, port, timeout) <= 0) {
        LOG("tls: TCP connect to %s:%u failed", host, port);
        return 0;
    }

    s_ = (TlsClientState*)calloc(1, sizeof(TlsClientState));
    if (!s_) {
        LOG("tls: out of memory");
        WiFiClient::stop();
        return 0;
    }
    mbedtls_ssl_init(&s_->ssl);
    mbedtls_ssl_config_init(&s_->conf);
    mbedtls_x509_crt_init(&s_->ca);

    int ret = mbedtls_x509_crt_parse(&s_->ca, (const unsigned char*)caCert_, strlen(caCert_) + 1);
    if (ret < 0) {
        LOG("tls: parsing CA certificates failed: -0x%04X", -ret);
        stop();
        return 0;
    }

    ret = mbedtls_ssl_config_defaults(&s_->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        LOG("tls: config failed: -0x%04X", -ret);
        stop();
        return 0;
    }
    mbedtls_ssl_conf_authmode(&s_->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s_->conf, &s_->ca, nullptr);
    mbedtls_ssl_conf_rng(&s_->conf, rng, nullptr);

    if ((ret = mbedtls_ssl_setup(&s_->ssl, &s_->conf)) != 0 ||
        (ret = mbedtls_ssl_set_hostname(&s_->ssl, host)) != 0) {
        LOG("tls: setup failed: -0x%04X", -ret);
        stop();
        return 0;
    }

    // LwIPClient::connect() leaves the socket in *blocking* mode. Switch it to
    // non-blocking, otherwise available() -> mbedtls_ssl_read() blocks in
    // recv() whenever no data is pending (e.g. HTTPClient checks connected()
    // before sending the request, while the server waits for that request).
    // (sockFd_ is laid out like mbedtls_net_context; see src/mbedtls_fixup.c.)
    sockFd_ = fd();
    mbedtls_net_set_nonblock((mbedtls_net_context*)&sockFd_);
    mbedtls_ssl_set_bio(&s_->ssl, &sockFd_, mbedtls_net_send, mbedtls_net_recv, nullptr);

    uint32_t start = millis();
    while ((ret = mbedtls_ssl_handshake(&s_->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&s_->ssl);
            LOG("tls: handshake with %s failed: -0x%04X (verify flags 0x%lx)", host, -ret,
                (unsigned long)flags);
            stop();
            return 0;
        }
        if (millis() - start > handshakeTimeoutMs_) {
            LOG("tls: handshake with %s timed out", host);
            stop();
            return 0;
        }
        delay(2);
    }
    LOG("tls: connected to %s in %lu ms (%s)", host, (unsigned long)(millis() - start),
        mbedtls_ssl_get_ciphersuite(&s_->ssl));
    return 1;
}

size_t TlsClient::write(uint8_t b) {
    return write(&b, 1);
}

size_t TlsClient::write(const uint8_t* buf, size_t size) {
    if (!s_ || eof_) return 0;
    size_t written = 0;
    uint32_t start = millis();
    while (written < size) {
        int ret = mbedtls_ssl_write(&s_->ssl, buf + written, size - written);
        if (ret > 0) {
            written += ret;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (millis() - start > 15000) {
                LOG("tls: write timed out");
                break;
            }
            delay(2);
        } else {
            LOG("tls: write failed: -0x%04X", -ret);
            break;
        }
    }
    return written;
}

int TlsClient::fill() {
    if (!s_) return 0;
    int avail = (int)mbedtls_ssl_get_bytes_avail(&s_->ssl);
    if (avail > 0 || eof_) return avail;

    // Reading 0 bytes makes mbedTLS process the next record, if one arrived.
    int ret = mbedtls_ssl_read(&s_->ssl, nullptr, 0);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF) {
        eof_ = true;  // server closed the connection; buffered data stays readable
    } else if (ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
        LOG("tls: read failed: -0x%04X", -ret);
        eof_ = true;
    }
    return (int)mbedtls_ssl_get_bytes_avail(&s_->ssl);
}

int TlsClient::available() {
    return fill() + (peeked_ >= 0 ? 1 : 0);
}

int TlsClient::read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
}

int TlsClient::read(uint8_t* buf, size_t size) {
    if (!buf || size == 0) return 0;
    size_t n = 0;
    if (peeked_ >= 0) {
        buf[n++] = (uint8_t)peeked_;
        peeked_ = -1;
        if (n == size) return n;
    }
    if (fill() <= 0) return n > 0 ? (int)n : -1;
    int ret = mbedtls_ssl_read(&s_->ssl, buf + n, size - n);
    if (ret > 0) return n + ret;
    return n > 0 ? (int)n : -1;
}

int TlsClient::peek() {
    // Non-blocking; Stream::timedPeek() does the waiting.
    if (peeked_ < 0) {
        uint8_t b;
        if (read(&b, 1) == 1) peeked_ = b;
    }
    return peeked_;
}

uint8_t TlsClient::connected() {
    if (!s_) return 0;
    // Still "connected" while decrypted data is buffered, even if the server
    // has already closed the TCP connection.
    if (peeked_ >= 0 || mbedtls_ssl_get_bytes_avail(&s_->ssl) > 0) return 1;
    if (eof_) return 0;
    return WiFiClient::connected();
}
