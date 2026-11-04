// The Beken BDK mbedTLS port comments out mbedtls_net_set_nonblock(), but
// LibreTiny's MbedTLSClient (WiFiClientSecure) calls it. The BDK still ships
// net_set_nonblock(int fd), so forward to that.
//
// mbedtls_net_context is { int fd; }, and MbedTLSClient passes a pointer to
// its socket fd, so reading the first int is correct.

int net_set_nonblock(int fd);

typedef struct {
    int fd;
} mbedtls_net_context_fixup;

int mbedtls_net_set_nonblock(mbedtls_net_context_fixup* ctx) {
    return net_set_nonblock(ctx->fd);
}
