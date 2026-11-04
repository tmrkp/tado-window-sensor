#pragma once

#include <WiFiClient.h>

struct TlsClientState;

// Minimal TLS client on top of LibreTiny's WiFiClient (lwIP socket) and the
// SDK's mbedTLS, used instead of LibreTiny's MbedTLSClient, which:
//  - reports no data once the server closed the TCP socket, even though
//    decrypted bytes are still buffered in mbedTLS (HTTPClient then waits for
//    its read timeout or fails with "connection lost"),
//  - leaves members uninitialized and cannot be reused reliably.
//
// The server certificate is always verified against `caCert` (PEM) and the
// host name used to connect.
class TlsClient : public WiFiClient {
  public:
    explicit TlsClient(const char* caCert);
    ~TlsClient();
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    void setHandshakeTimeout(uint32_t ms) { handshakeTimeoutMs_ = ms; }

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char* host, uint16_t port) override;
    int connect(IPAddress ip, uint16_t port, int32_t timeout) override;
    int connect(const char* host, uint16_t port, int32_t timeout) override;

    size_t write(uint8_t b) override;
    size_t write(const uint8_t* buf, size_t size) override;

    int available() override;
    int read() override;
    int read(uint8_t* buf, size_t size) override;
    int peek() override;
    void flush() override {}
    void stop() override;
    uint8_t connected() override;

    using Print::write;

  private:
    // Pull the next TLS record into mbedTLS if the socket has data.
    // Returns the number of decrypted bytes buffered.
    int fill();
    void freeState();

    const char* caCert_;
    TlsClientState* s_ = nullptr;
    uint32_t handshakeTimeoutMs_ = 15000;
    int sockFd_ = -1;   // mbedtls_net_context is { int fd; }
    bool eof_ = false;  // server closed the connection (close_notify or TCP FIN)
    int peeked_ = -1;
};
