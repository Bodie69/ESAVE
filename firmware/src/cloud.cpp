#include "cloud.h"
#include <ArduinoJson.h>
#include "firebase_roots.h"

#include "provisioning.h"
#include "state.h"

// Internal helpers
static String urlEncode(const String &s);
static String firebaseBaseUrl();
static String hostFromUrl(const String &url);
static bool firebaseConfigLooksValid();
static void logNetworkDiagnostics(const char *host, bool force = false);
static bool probeTcp443(const char *host, String &resolvedIp);
static void configureFirebaseTls(WiFiClientSecure &client);
static bool probeTls443(const char *host);
static void runFirebaseTransportProbe(bool forceLog = true);
static void recoverFirebaseWifiTransport();
static void recordFirebaseTransportResult(bool ok);
static int httpsJsonRequest(const String &method, const String &url, const String &body, String &response, const String &contentType = "application/json", bool keepAlive = false);
static bool firebaseSignIn();
static bool firebaseRefreshAuth();
static bool ensureFirebaseAuth();

// Persistent keep-alive HTTPS connection to the RTDB host. Reusing it avoids a full
// TLS handshake for every command poll / state push. Used only by firebaseNetworkTask.
static WiFiClientSecure firebaseDbTls;
static HTTPClient firebaseDbHttp;
// Auth requests and the TLS probe reuse one client created at boot. Constructing a
// WiFiClientSecure allocates with throwing new, so creating one per request aborted the
// controller (std::bad_alloc -> terminate) once the heap was low.
static WiFiClientSecure firebaseAuthTls;
volatile bool firebaseAuthenticated = false;
static String firebaseIdToken;
static String firebaseRefreshToken;
static unsigned long firebaseTokenExpiresAtMs = 0;
unsigned long firebaseAuthFailures = 0;
unsigned long firebaseHttpFailures = 0;
unsigned long lastFirebaseOkAt = 0;
static unsigned long lastFirebaseAuthAttemptAt = 0;
unsigned long firebaseAuthRetryMs = FIREBASE_AUTH_RETRY_MIN_MS;
static unsigned long lastNetworkDiagAt = 0;
int lastHttpErrorCode = 0;
String lastHttpErrorText = "none";
String lastHttpHost = "none";
uint32_t firebaseTransportFailuresConsecutive = 0;
static unsigned long lastFirebaseTransportRecoveryAt = 0;
bool lastFirebaseDnsOk = false;
bool lastFirebaseTcp443Ok = false;
bool lastFirebaseTls443Ok = false;
String lastFirebaseResolvedIp = "0.0.0.0";
String lastFirebaseTlsError = "none";

static String urlEncode(const String &s) {
  const char *hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%'; out += hex[c >> 4]; out += hex[c & 0x0F];
    }
  }
  return out;
}

static String firebaseBaseUrl() {
  String b = DATABASE_URL;
  while (b.endsWith("/")) b.remove(b.length() - 1);
  return b;
}

static String hostFromUrl(const String &url) {
  int start = url.indexOf("://");
  start = start >= 0 ? start + 3 : 0;
  int end = url.indexOf('/', start);
  if (end < 0) end = url.length();
  return url.substring(start, end);
}

static bool firebaseConfigLooksValid() {
  String key = API_KEY;
  String db = DATABASE_URL;
  String email = USER_EMAIL;
  String pass = USER_PASSWORD;
  return key.length() > 20 && !key.startsWith("YOUR_") &&
         db.startsWith("https://") && db.indexOf("YOUR_PROJECT") < 0 &&
         email.indexOf('@') > 0 && email.indexOf("example.com") < 0 &&
         pass.length() >= 6 && pass.indexOf("YOUR_") < 0;
}

static void logNetworkDiagnostics(const char *host, bool force) {
  unsigned long now = millis();
  if (!force && lastNetworkDiagAt && now - lastNetworkDiagAt < NETWORK_DIAG_INTERVAL_MS) return;
  lastNetworkDiagAt = now;

  wl_status_t st = WiFi.status();
  if (st != WL_CONNECTED) {
    Serial.printf("[NET] WiFi NOT connected status=%d\n", (int)st);
    return;
  }

  IPAddress resolved;
  int dnsOk = WiFi.hostByName(host, resolved);
  Serial.printf("[NET] WiFi OK IP=%s RSSI=%d dBm GW=%s DNS=%s host=%s resolve=%s %s\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
                host, dnsOk == 1 ? "OK" : "FAIL",
                dnsOk == 1 ? resolved.toString().c_str() : "0.0.0.0");
}

static bool probeTcp443(const char *host, String &resolvedIp) {
  IPAddress resolved;
  lastFirebaseDnsOk = WiFi.hostByName(host, resolved) == 1;
  if (!lastFirebaseDnsOk) {
    resolvedIp = "0.0.0.0";
    { StateLock lock; lastFirebaseResolvedIp = resolvedIp; }
    lastFirebaseTcp443Ok = false;
    return false;
  }
  resolvedIp = resolved.toString();
  { StateLock lock; lastFirebaseResolvedIp = resolvedIp; }

  WiFiClient tcp;
  int ok = tcp.connect(resolved, 443, FIREBASE_TRANSPORT_PROBE_TIMEOUT_MS);
  lastFirebaseTcp443Ok = ok == 1;
  tcp.stop();
  return lastFirebaseTcp443Ok;
}

// Every Firebase connection verifies the server certificate against the pinned Google roots.
// Certificate validity is checked against the clock, so callers wait for NTP time first.
static void configureFirebaseTls(WiFiClientSecure &client) {
#if FIREBASE_TLS_INSECURE
  client.setInsecure();
#else
  client.setCACert(FIREBASE_ROOT_CA_BUNDLE);
#endif
}

static bool probeTls443(const char *host) {
  WiFiClientSecure &tls = firebaseAuthTls;
  firebaseDbTls.stop(); // one TLS session at a time (see httpsJsonRequest)
  tls.stop();
  configureFirebaseTls(tls);
  tls.setHandshakeTimeout(8);
  int ok = tls.connect(host, 443, FIREBASE_TRANSPORT_PROBE_TIMEOUT_MS);
  char err[160] = {0};
  int errCode = tls.lastError(err, sizeof(err));
  lastFirebaseTls443Ok = ok == 1;
  String tlsError = (err[0] != '\0') ? String(err) : (lastFirebaseTls443Ok ? "none" : ("code " + String(errCode)));
  { StateLock lock; lastFirebaseTlsError = tlsError; }
  tls.stop();
  return lastFirebaseTls443Ok;
}

// A TLS handshake needs one ~16 KB block (mbedTLS record buffer) plus room for the
// certificate chain. Below that the attempt can only fail, and the failed allocations
// starve Bluetooth (its controller asserts when it cannot allocate), so it is skipped.
bool tlsHeapAvailable() {
  return ESP.getMaxAllocHeap() >= TLS_MIN_MAX_ALLOC_BYTES && ESP.getFreeHeap() >= TLS_MIN_FREE_HEAP_BYTES;
}

// Wi-Fi and Bluetooth traffic fragment the heap until no two ~16.7 KB holes are left for the
// mbedTLS input/output record buffers, even with 45 KB free (mbedtls_ssl_setup then fails with
// "SSL - Memory allocation failed"). Right after Wi-Fi joins there is still one ~35 KB hole, so
// maintainTlsReserve() grabs room for both buffers then, frees it right before each TLS handshake
// so the buffers land in it, and grabs it again whenever no TLS session is open. It is never held
// while Wi-Fi is down: joining a network needs that heap (a reserve held from boot kept Wi-Fi from
// connecting). Only firebaseNetworkTask touches it.
// Input + output record buffers are ~16,717 bytes each (16 KB content + header/MAC/padding).
// Kept just above that: the hole left after Wi-Fi joins measured 34,804 bytes, too small for 34 KB.
static const size_t TLS_RESERVE_FULL_BYTES = 33600;
static const size_t TLS_RESERVE_HALF_BYTES = 17 * 1024; // fallback: one buffer
static void *tlsReserve = nullptr;

static void reserveTlsHeap() {
  // Dynamic buffers need no 33 KB hole, and a reserve held while Bluetooth is on makes Bluetooth
  // abort on an incoming connection (see firebaseNetworkTask).
  if (BT_CLOUD_COEXIST || tlsReserve) return;
  uint32_t maxBefore = ESP.getMaxAllocHeap();
  size_t got = TLS_RESERVE_FULL_BYTES;
  tlsReserve = malloc(TLS_RESERVE_FULL_BYTES);
  if (!tlsReserve) {
    got = TLS_RESERVE_HALF_BYTES;
    tlsReserve = malloc(TLS_RESERVE_HALF_BYTES);
  }
  static size_t lastLogged = 1;
  size_t held = tlsReserve ? got : 0;
  if (held != lastLogged) {
    lastLogged = held;
    Serial.printf("[TLS] reserve %u bytes held (maxAlloc before=%u)\n", (unsigned)held, (unsigned)maxBefore);
  }
}

static void releaseTlsHeap() {
  free(tlsReserve);
  tlsReserve = nullptr;
}

void maintainTlsReserve(bool wifiConnected) {
  // Wi-Fi status flickers right after joining; dropping the reserve then lost the only hole big
  // enough to take it back. Free it only once Wi-Fi has really been down for a while.
  static unsigned long wifiDownSince = 0;
  if (wifiConnected) {
    wifiDownSince = 0;
    if (!firebaseDbTls.connected()) reserveTlsHeap();
  } else if (!wifiDownSince) {
    wifiDownSince = millis();
  } else if (millis() - wifiDownSince >= 10000UL) {
    // Also frees the keep-alive session's buffers before Bluetooth restarts (after 30 s down).
    firebaseDbTls.stop();
    releaseTlsHeap();
  }
}

void stopFirebaseForBluetooth() {
  bool wasSignedIn = firebaseAuthenticated;
  resetFirebaseConnection(true); // closes the RTDB session and drops the tokens
  releaseTlsHeap();
  if (wasSignedIn) {
    Serial.printf("[FIREBASE] signed out for Bluetooth: heap=%u maxAlloc=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}

static void runFirebaseTransportProbe(bool forceLog) {
  if (WiFi.status() != WL_CONNECTED) {
    lastFirebaseDnsOk = false;
    lastFirebaseTcp443Ok = false;
    lastFirebaseTls443Ok = false;
    if (forceLog) Serial.println("[NETTEST] WiFi is not connected; Firebase transport probe skipped");
    return;
  }
  firebaseDbTls.stop();
  releaseTlsHeap();
  if (!tlsHeapAvailable()) {
    reserveTlsHeap();
    return;
  }

  String resolved;
  bool tcpOk = probeTcp443("identitytoolkit.googleapis.com", resolved);
  bool tlsOk = false;
  if (tcpOk) tlsOk = probeTls443("identitytoolkit.googleapis.com");
  reserveTlsHeap();

  if (forceLog) {
    Serial.printf("[NETTEST] FirebaseAuth DNS=%s IP=%s TCP443=%s TLS443=%s RSSI=%d heap=%u maxAlloc=%u tlsErr=%s\n",
                  lastFirebaseDnsOk ? "OK" : "FAIL", resolved.c_str(),
                  lastFirebaseTcp443Ok ? "OK" : "FAIL",
                  lastFirebaseTls443Ok ? "OK" : "FAIL",
                  WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), lastFirebaseTlsError.c_str());
  }
}

static void recoverFirebaseWifiTransport() {
  unsigned long now = millis();
  if (lastFirebaseTransportRecoveryAt && now - lastFirebaseTransportRecoveryAt < FIREBASE_WIFI_RECOVERY_COOLDOWN_MS) return;
  lastFirebaseTransportRecoveryAt = now;

  // Skipped while someone is using the setup portal: a reconnect would drop their phone.
  if (!restartWifiStation()) return;
  Serial.println("[NET][RECOVERY] Reconnecting WiFi after repeated Firebase transport failures; sensors/Bluetooth remain active");
  resetFirebaseConnection(true);
}

static void recordFirebaseTransportResult(bool ok) {
  if (ok) {
    firebaseTransportFailuresConsecutive = 0;
    return;
  }
  if (firebaseTransportFailuresConsecutive < 0xFFFFFFFFUL) firebaseTransportFailuresConsecutive++;
  if (firebaseTransportFailuresConsecutive >= FIREBASE_TRANSPORT_FAILURES_BEFORE_WIFI_RECOVERY) {
    runFirebaseTransportProbe(true);
    recoverFirebaseWifiTransport();
    firebaseTransportFailuresConsecutive = 0;
  }
}

// Drops the RTDB keep-alive connection and forces a new sign-in (Wi-Fi reconnects, setup AP).
void resetFirebaseConnection(bool dropTokens) {
  // No reserveTlsHeap() here: this also runs from setup() while Bluetooth is still on, and
  // maintainTlsReserve() takes the reserve back once Bluetooth is off.
  firebaseDbTls.stop();
  firebaseAuthenticated = false;
  if (dropTokens) {
    firebaseIdToken = "";
    firebaseRefreshToken = "";
  }
  lastFirebaseAuthAttemptAt = 0;
  firebaseAuthRetryMs = FIREBASE_AUTH_RETRY_MIN_MS;
}

// keepAlive=true uses the persistent RTDB connection (firebaseNetworkTask only); auth hosts
// use a one-shot connection.
static int httpsJsonRequest(const String &method, const String &url, const String &body, String &response, const String &contentType, bool keepAlive) {
  String host = hostFromUrl(url);
  // Only one TLS session fits in the heap next to Bluetooth Classic: an auth request (sign-in or
  // hourly token refresh) first drops the RTDB keep-alive session; the next RTDB request reconnects.
  if (!keepAlive) firebaseDbTls.stop();
  // An open keep-alive connection already holds its TLS buffers; only a new handshake needs the heap.
  bool needsHandshake = !(keepAlive && firebaseDbTls.connected());
  if (needsHandshake) {
    releaseTlsHeap();
    Serial.printf("[TLS] handshake %s heap=%u maxAlloc=%u\n", host.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  if (needsHandshake && !tlsHeapAvailable()) {
    {
      StateLock lock;
      lastHttpHost = host;
      lastHttpErrorCode = HTTP_ERROR_LOW_MEMORY;
      lastHttpErrorText = "low memory: TLS skipped";
    }
    Serial.printf("[HTTP] %s %s skipped: low memory heap=%u maxAlloc=%u (needs %u/%u)\n",
                  method.c_str(), host.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                  (unsigned)TLS_MIN_FREE_HEAP_BYTES, (unsigned)TLS_MIN_MAX_ALLOC_BYTES);
    if (keepAlive) firebaseDbTls.stop();
    reserveTlsHeap();
    return HTTP_ERROR_LOW_MEMORY;
  }

  HTTPClient oneShotHttp;
  WiFiClientSecure &client = keepAlive ? firebaseDbTls : firebaseAuthTls;
  HTTPClient &http = keepAlive ? firebaseDbHttp : oneShotHttp;
  if (!keepAlive) client.stop();

  configureFirebaseTls(client);
  client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SEC);

  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_REQUEST_TIMEOUT_MS);
  http.setReuse(keepAlive);

  { StateLock lock; lastHttpHost = host; }
  if (!http.begin(client, url)) {
    {
      StateLock lock;
      lastHttpErrorCode = -1000;
      lastHttpErrorText = "http.begin failed";
    }
    Serial.printf("[HTTP] begin failed host=%s\n", host.c_str());
    if (keepAlive) client.stop();
    reserveTlsHeap();
    return -1000;
  }

  http.addHeader("Content-Type", contentType);
  // With reuse enabled HTTPClient sends "Connection: keep-alive" itself.
  if (!keepAlive) http.addHeader("Connection", "close");

  int code = -1;
  if (method == "GET") code = http.GET();
  else if (method == "POST") code = http.POST(body);
  else if (method == "PUT") code = http.PUT(body);
  else code = http.sendRequest(method.c_str(), (uint8_t*)body.c_str(), body.length());

  response = code > 0 ? http.getString() : String();

  if (code < 0) {
    String errorText = HTTPClient::errorToString(code);
    {
      StateLock lock;
      lastHttpErrorCode = code;
      lastHttpErrorText = errorText;
    }
    char sslError[160] = {0};
    int sslCode = client.lastError(sslError, sizeof(sslError));
    Serial.printf("[HTTP] %s %s failed code=%d (%s) ssl=%d (%s) wifi=%d rssi=%d ip=%s heap=%u\n",
                  method.c_str(), host.c_str(), code, errorText.c_str(),
                  sslCode, sslError, (int)WiFi.status(), WiFi.RSSI(),
                  WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
    logNetworkDiagnostics(host.c_str(), true);
  } else {
    StateLock lock;
    lastHttpErrorCode = code;
    lastHttpErrorText = "HTTP " + String(code);
  }

  http.end();
  // A failed keep-alive connection is dropped so the next request starts a fresh handshake.
  if (!keepAlive || code < 0) client.stop();
  // A kept-alive RTDB session keeps using the buffers; otherwise take the reserve back.
  if (!(keepAlive && client.connected())) reserveTlsHeap();
  return code;
}

static bool firebaseSignIn() {
  if (WiFi.status() != WL_CONNECTED) return false;

  unsigned long now = millis();
  if (lastFirebaseAuthAttemptAt && now - lastFirebaseAuthAttemptAt < firebaseAuthRetryMs) return false;
  lastFirebaseAuthAttemptAt = now;

  if (!firebaseConfigLooksValid()) {
    firebaseAuthenticated = false;
    firebaseAuthFailures++;
    firebaseAuthRetryMs = FIREBASE_AUTH_RETRY_MAX_MS;
    Serial.println("[FIREBASE] CONFIG INVALID: check secrets.h API_KEY, DATABASE_URL, USER_EMAIL, USER_PASSWORD");
    return false;
  }

  logNetworkDiagnostics("identitytoolkit.googleapis.com");

  JsonDocument req;
  req["email"] = USER_EMAIL;
  req["password"] = USER_PASSWORD;
  req["returnSecureToken"] = true;
  String body;
  serializeJson(req, body);

  String response;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + API_KEY;
  int code = httpsJsonRequest("POST", url, body, response);
  if (code != 200) {
    firebaseAuthenticated = false;
    firebaseAuthFailures++;

    if (code == HTTP_ERROR_LOW_MEMORY) {
      firebaseAuthRetryMs = FIREBASE_AUTH_RETRY_MAX_MS;
    } else if (code < 0) {
      recordFirebaseTransportResult(false);
      firebaseAuthRetryMs = min(FIREBASE_AUTH_RETRY_MAX_MS,
                                max(FIREBASE_AUTH_RETRY_MIN_MS, firebaseAuthRetryMs * 2UL));
      Serial.printf("[FIREBASE] sign-in TRANSPORT failed HTTP %d (%s); retry in %lus | DNS=%s TCP443=%s TLS443=%s heap=%u maxAlloc=%u\n",
                    code, lastHttpErrorText.c_str(), firebaseAuthRetryMs / 1000UL,
                    lastFirebaseDnsOk ? "OK" : "?", lastFirebaseTcp443Ok ? "OK" : "?", lastFirebaseTls443Ok ? "OK" : "?",
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    } else {
      firebaseAuthRetryMs = FIREBASE_AUTH_RETRY_MIN_MS;
      String apiMessage = response;
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, response)) {
        const char *m = errDoc["error"]["message"] | "";
        if (m && strlen(m)) apiMessage = m;
      }
      if (apiMessage.length() > 120) apiMessage = apiMessage.substring(0, 120);
      Serial.printf("[FIREBASE] sign-in rejected HTTP %d: %s\n", code, apiMessage.c_str());
    }
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    firebaseAuthenticated = false;
    firebaseAuthFailures++;
    Serial.println("[FIREBASE] sign-in response JSON parse failed");
    return false;
  }

  firebaseIdToken = doc["idToken"] | "";
  firebaseRefreshToken = doc["refreshToken"] | "";
  unsigned long expiresSec = String((const char*)(doc["expiresIn"] | "3600")).toInt();
  firebaseTokenExpiresAtMs = millis() + max(300UL, expiresSec) * 1000UL;
  firebaseAuthenticated = firebaseIdToken.length() > 100;
  if (firebaseAuthenticated) {
    lastFirebaseOkAt = millis();
    firebaseAuthRetryMs = FIREBASE_AUTH_RETRY_MIN_MS;
    recordFirebaseTransportResult(true);
    Serial.println("[FIREBASE] authenticated");
  } else {
    Serial.println("[FIREBASE] sign-in response missing valid ID token");
  }
  return firebaseAuthenticated;
}

static bool firebaseRefreshAuth() {
  if (WiFi.status() != WL_CONNECTED || firebaseRefreshToken.length() == 0) return firebaseSignIn();
  String form = "grant_type=refresh_token&refresh_token=" + urlEncode(firebaseRefreshToken);
  String response;
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + API_KEY;
  int code = httpsJsonRequest("POST", url, form, response, "application/x-www-form-urlencoded");
  if (code != 200) return firebaseSignIn();
  JsonDocument doc;
  if (deserializeJson(doc, response)) return firebaseSignIn();
  firebaseIdToken = doc["id_token"] | "";
  const char* refreshedToken = doc["refresh_token"] | "";
  if (refreshedToken != nullptr && strlen(refreshedToken) > 0) firebaseRefreshToken = refreshedToken;
  unsigned long expiresSec = String((const char*)(doc["expires_in"] | "3600")).toInt();
  firebaseTokenExpiresAtMs = millis() + max(300UL, expiresSec) * 1000UL;
  firebaseAuthenticated = firebaseIdToken.length() > 100;
  if (firebaseAuthenticated) lastFirebaseOkAt = millis();
  return firebaseAuthenticated;
}

static bool ensureFirebaseAuth() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!firebaseAuthenticated || firebaseIdToken.length() == 0) return firebaseSignIn();
  // Refresh five minutes before expiry. millis overflow is safe for ordinary uptime windows here.
  if ((long)(firebaseTokenExpiresAtMs - millis()) < 300000L) return firebaseRefreshAuth();
  return true;
}

bool firebaseRequest(const String &method, const String &path, const String &body, String &response, bool retryAuth) {
  if (!ensureFirebaseAuth()) return false;
  String url = firebaseBaseUrl() + path + ".json?auth=" + urlEncode(firebaseIdToken);
  int code = httpsJsonRequest(method, url, body, response, "application/json", true);
  // A kept-alive connection may have been closed by the server while idle. The failed
  // client was stopped, so one immediate retry opens a fresh connection. GET/PUT are idempotent.
  if (code < 0 && code != HTTP_ERROR_LOW_MEMORY) code = httpsJsonRequest(method, url, body, response, "application/json", true);
  if (code == 200 || code == 204) {
    lastFirebaseOkAt = millis();
    return true;
  }
  if ((code == 401 || code == 403) && retryAuth) {
    firebaseAuthenticated = false;
    if (firebaseRefreshAuth()) return firebaseRequest(method, path, body, response, false);
  }
  firebaseHttpFailures++;
  Serial.printf("[FIREBASE] %s %s failed HTTP %d\n", method.c_str(), path.c_str(), code);
  return false;
}
