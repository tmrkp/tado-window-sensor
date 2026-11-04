#include "storage.h"

#include <flashdb.h>

#include "log.h"

namespace {

constexpr const char* KEY_REFRESH = "tado_refresh";
constexpr const char* KEY_ACCESS = "tado_access";
constexpr const char* KEY_ACCESS_EXP = "tado_access_exp";
constexpr const char* KEY_SENT_STATE = "sent_state";
constexpr const char* KEY_WIFI_CACHE = "wifi_cache";
// The access token is a JWT (~1-2 KB); leave headroom.
constexpr size_t MAX_VALUE_LEN = 4096;

struct fdb_kvdb kvdb;
bool ready = false;

String getString(const char* key) {
    if (!Storage::begin()) return String();

    struct fdb_blob blob = {};  // saved.len is only written if the key exists
    fdb_kv_get_blob(&kvdb, key, fdb_blob_make(&blob, nullptr, 0));
    size_t len = blob.saved.len;
    if (len == 0 || len > MAX_VALUE_LEN) return String();

    char* buf = (char*)malloc(len + 1);
    if (!buf) return String();
    size_t read = fdb_kv_get_blob(&kvdb, key, fdb_blob_make(&blob, buf, len));
    buf[read] = '\0';
    String value(buf);
    free(buf);
    return value;
}

bool getRaw(const char* key, void* data, size_t len) {
    if (!Storage::begin()) return false;
    struct fdb_blob blob = {};
    return fdb_kv_get_blob(&kvdb, key, fdb_blob_make(&blob, data, len)) == len;
}

bool setBlob(const char* key, const void* data, size_t len) {
    if (!Storage::begin()) return false;
    struct fdb_blob blob;
    fdb_err_t err = fdb_kv_set_blob(&kvdb, key, fdb_blob_make(&blob, data, len));
    if (err != FDB_NO_ERR) {
        LOG("storage: saving %s failed, err=%d", key, (int)err);
        return false;
    }
    return true;
}

bool del(const char* key) {
    if (!Storage::begin()) return false;
    fdb_err_t err = fdb_kv_del(&kvdb, key);
    return err == FDB_NO_ERR || err == FDB_KV_NAME_ERR;  // missing key is fine
}

}  // namespace

namespace Storage {

bool begin() {
    if (ready) return true;
    fdb_err_t err = fdb_kvdb_init(&kvdb, "tado", "kvs", nullptr, nullptr);
    if (err != FDB_NO_ERR) {
        LOG("storage: init failed, err=%d", (int)err);
        return false;
    }
    ready = true;
    return true;
}

String loadRefreshToken() {
    return getString(KEY_REFRESH);
}

bool saveRefreshToken(const String& token) {
    return setBlob(KEY_REFRESH, token.c_str(), token.length());
}

bool clearRefreshToken() {
    return del(KEY_REFRESH);
}

bool loadAccessToken(String& token, uint32_t& expiresAt) {
    uint32_t exp = 0;
    if (!getRaw(KEY_ACCESS_EXP, &exp, sizeof(exp))) return false;

    String value = getString(KEY_ACCESS);
    if (value.length() == 0) return false;

    token = value;
    expiresAt = exp;
    return true;
}

bool saveAccessToken(const String& token, uint32_t expiresAt) {
    // Write the token first, then its expiry. If power fails in between, a new
    // token is paired with the old expiry, which at worst costs a 401 + refresh.
    return setBlob(KEY_ACCESS, token.c_str(), token.length()) &&
           setBlob(KEY_ACCESS_EXP, &expiresAt, sizeof(expiresAt));
}

bool clearAccessToken() {
    bool a = del(KEY_ACCESS_EXP);
    bool b = del(KEY_ACCESS);
    return a && b;
}

WindowState loadSentState() {
    uint8_t value = 0;
    if (!getRaw(KEY_SENT_STATE, &value, sizeof(value))) return WindowState::Unknown;
    if (value > (uint8_t)WindowState::Open) return WindowState::Unknown;
    return (WindowState)value;
}

bool saveSentState(WindowState state) {
    uint8_t value = (uint8_t)state;
    return setBlob(KEY_SENT_STATE, &value, sizeof(value));
}

bool loadWifiCache(WifiCache& cache) {
    if (!getRaw(KEY_WIFI_CACHE, &cache, sizeof(cache))) return false;
    cache.ssid[sizeof(cache.ssid) - 1] = '\0';
    return true;
}

bool saveWifiCache(const WifiCache& cache) {
    return setBlob(KEY_WIFI_CACHE, &cache, sizeof(cache));
}

bool clearWifiCache() {
    return del(KEY_WIFI_CACHE);
}

}  // namespace Storage
