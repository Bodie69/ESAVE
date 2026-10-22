#include "provisioning.h"
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "cloud.h"
#include "state.h"

// Internal helpers
static void loadCredentials();
static void startStation();
static bool stationRetryPaused(unsigned long now);
static void openPortal(const char *reason);
static void closePortal();
static void servicePortal(unsigned long now, bool connected);
static String htmlEscape(const String &s);
static const char *stationStatusText();
static void sendPage(int code, const String &body, int refreshSeconds = 0);
static void handleRoot();
static void handleScan();
static void handleSave();
static void handleForget();
static void handleNotFound();

volatile bool wifiPortalActive = false;
volatile bool wifiConfigured = false;
String wifiSsid;

static Preferences wifiPrefs;           // separate NVS namespace "rsu-wifi"
static String stationPassword;          // never shown in the portal or sent to the app
static WebServer portalServer(80);
static DNSServer portalDns;
static bool portalRoutesAdded = false;
static const IPAddress PORTAL_IP(192, 168, 4, 1);

static volatile bool portalRequested = false;
static const char *volatile portalRequestReason = "";

static bool wasConnected = false;
static unsigned long offlineSince = 0;
static unsigned long lastStationAttemptAt = 0;
static unsigned long stationRetryMs = WIFI_RETRY_MS;
static unsigned long portalLastActivityAt = 0;
static unsigned long lastSaveAt = 0;

static void loadCredentials() {
  // isKey() first: getString() on a missing key logs an NVS error on every first boot.
  String ssid = wifiPrefs.isKey("ssid") ? wifiPrefs.getString("ssid", "") : String();
  stationPassword = wifiPrefs.isKey("pass") ? wifiPrefs.getString("pass", "") : String();
  { StateLock lock; wifiSsid = ssid; }
  wifiConfigured = ssid.length() > 0;
}

// Caller must not hold stateMutex: Wi-Fi driver calls stay outside the lock like all other I/O.
static void startStation() {
  String ssid;
  { StateLock lock; ssid = wifiSsid; }
  if (ssid.length() == 0) return;
  lastStationAttemptAt = millis();
  resetFirebaseConnection(false);
  WiFi.disconnect(false, false);  // station only; the setup AP stays up
  WiFi.begin(ssid.c_str(), stationPassword.length() ? stationPassword.c_str() : nullptr);
  Serial.printf("[WIFI] connecting to %s\n", ssid.c_str());
}

// Every reconnect attempt scans channels, which drops phones connected to the setup AP. While
// someone is on the portal, retry only right after they saved new credentials.
static bool stationRetryPaused(unsigned long now) {
  if (!wifiPortalActive || WiFi.softAPgetStationNum() == 0) return false;
  return !(lastSaveAt && now - lastSaveAt < WIFI_PORTAL_SAVE_RETRY_WINDOW_MS);
}

void beginWifi() {
  wifiPrefs.begin("rsu-wifi", false);
  loadCredentials();

  WiFi.persistent(false);         // credentials live in our own NVS namespace only
  WiFi.setAutoReconnect(false);   // maintainWifi() reconnects with backoff instead
  WiFi.mode(WIFI_STA);
  // Modem sleep must stay ON: with Bluetooth Classic running, the Wi-Fi driver aborts
  // ("Should enable WiFi modem sleep when both WiFi and Bluetooth are enabled") without it.
  WiFi.setSleep(true);

  if (wifiConfigured) {
    startStation();
  } else {
    Serial.println("[WIFI] no saved network; opening setup access point");
    openPortal("not configured");
  }
}

bool restartWifiStation() {
  if (!wifiConfigured || stationRetryPaused(millis())) return false;
  startStation();
  return true;
}

void requestWifiSetupPortal(const char *reason) {
  portalRequestReason = reason;
  portalRequested = true;
}

void maintainWifi() {
  unsigned long now = millis();

  if (portalRequested) {
    portalRequested = false;
    openPortal(portalRequestReason);
  }

  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    if (!wasConnected) {
      Serial.printf("[WIFI] connected IP=%s RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      StateLock lock;
      markStateDirty("wifi_connected");
    }
    wasConnected = true;
    offlineSince = 0;
    stationRetryMs = WIFI_RETRY_MS;
  } else {
    if (wasConnected) {
      Serial.println("[WIFI] connection lost; controller keeps running offline");
      wasConnected = false;
      lastStationAttemptAt = now;  // give the first retry a moment instead of scanning at once
      StateLock lock;
      markStateDirty("wifi_lost");
    }
    if (!offlineSince) offlineSince = now ? now : 1;

    // Back off 8 s -> 60 s: constant scanning offline competes with Bluetooth for the radio.
    if (wifiConfigured && now - lastStationAttemptAt >= stationRetryMs && !stationRetryPaused(now)) {
      startStation();
      stationRetryMs = min(WIFI_RETRY_MAX_MS, stationRetryMs * 2UL);
    }

    // Saved network unreachable for a while (moved router, changed password): offer the setup
    // AP so staff can fix it on site. The station keeps retrying in the background.
    if (!wifiPortalActive && wifiConfigured && now - offlineSince >= WIFI_PORTAL_AFTER_OFFLINE_MS) {
      openPortal("saved network unreachable");
    }
  }

  if (wifiPortalActive) servicePortal(now, connected);
}

// ----------------------------------------------------------------------------- Setup AP

static void openPortal(const char *reason) {
  portalLastActivityAt = millis();
  if (wifiPortalActive) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(PORTAL_IP, PORTAL_IP, IPAddress(255, 255, 255, 0));
  // An AP+STA radio has one channel: follow the station when it is connected.
  int channel = WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1;
  if (!WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD, channel, 0, WIFI_SETUP_AP_MAX_CLIENTS)) {
    Serial.println("[WIFI] ERROR: setup access point failed to start");
    WiFi.mode(WIFI_STA);
    return;
  }

  portalDns.setErrorReplyCode(DNSReplyCode::NoError);
  portalDns.start(53, "*", PORTAL_IP);  // captive portal: every name resolves to the controller

  if (!portalRoutesAdded) {
    portalServer.on("/", HTTP_GET, handleRoot);
    portalServer.on("/scan", HTTP_POST, handleScan);
    portalServer.on("/save", HTTP_POST, handleSave);
    portalServer.on("/forget", HTTP_POST, handleForget);
    portalServer.onNotFound(handleNotFound);
    portalRoutesAdded = true;
  }
  portalServer.begin();
  WiFi.scanNetworks(true);  // async; results shown on the setup page

  wifiPortalActive = true;
  if (strncmp(SETUP_AP_PASSWORD, "YOUR_", 5) == 0) {
    Serial.println("[WIFI] WARNING: SETUP_AP_PASSWORD is still the example placeholder; set a real one in secrets.h");
  }
  Serial.printf("[WIFI] setup AP \"%s\" open (%s): join it and browse to http://%s/\n",
                SETUP_AP_SSID, reason, PORTAL_IP.toString().c_str());
  StateLock lock;
  markStateDirty("wifi_setup_open");
}

static void closePortal() {
  portalServer.stop();
  portalDns.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  wifiPortalActive = false;
  Serial.println("[WIFI] setup AP closed");
  StateLock lock;
  markStateDirty("wifi_setup_closed");
}

static void servicePortal(unsigned long now, bool connected) {
  portalDns.processNextRequest();
  portalServer.handleClient();
  if (WiFi.softAPgetStationNum() > 0) portalLastActivityAt = now;
  // Close only once the controller is online and nobody has used the portal for a while.
  if (connected && wifiConfigured && now - portalLastActivityAt >= WIFI_PORTAL_IDLE_CLOSE_MS) closePortal();
}

// ----------------------------------------------------------------------------- Setup page

static String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

static const char *stationStatusText() {
  if (!wifiConfigured) return "No network saved";
  switch (WiFi.status()) {
    case WL_CONNECTED: return "Connected";
    case WL_NO_SSID_AVAIL: return "Network not found";
    case WL_CONNECT_FAILED: return "Connection failed - check the password";
    case WL_CONNECTION_LOST: return "Connection lost - retrying";
    default: return "Not connected - retrying";
  }
}

static void sendPage(int code, const String &body, int refreshSeconds) {
  String html;
  html.reserve(body.length() + 1200);
  html += F("<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>");
  if (refreshSeconds > 0) html += "<meta http-equiv=refresh content='" + String(refreshSeconds) + ";url=/'>";
  html += F("<title>Room Wi-Fi setup</title><style>"
            "body{font-family:sans-serif;max-width:28rem;margin:0 auto;padding:1rem;background:#f4f6f8;color:#1b1f24}"
            "h1{font-size:1.25rem}.card{background:#fff;border-radius:8px;padding:1rem;margin:.75rem 0;box-shadow:0 1px 3px #0002}"
            "label{display:block;margin:.6rem 0 .2rem;font-weight:600}input{width:100%;box-sizing:border-box;padding:.6rem;font-size:1rem}"
            "button{width:100%;padding:.7rem;margin-top:.8rem;font-size:1rem;border:0;border-radius:6px;background:#1565c0;color:#fff}"
            "button.alt{background:#5f6b76}.net{display:block;width:100%;text-align:left;background:#eef2f6;color:#1b1f24;margin:.3rem 0}"
            ".muted{color:#5f6b76;font-size:.9rem}.err{color:#b00020}</style></head><body>");
  html += body;
  html += F("</body></html>");
  portalServer.sendHeader("Cache-Control", "no-store");
  portalServer.send(code, "text/html", html);
}

static void handleRoot() {
  portalLastActivityAt = millis();
  String ssid;
  { StateLock lock; ssid = wifiSsid; }
  bool connected = WiFi.status() == WL_CONNECTED;

  String b;
  b += "<h1>" + String(BT_NAME) + " Wi-Fi setup</h1>";
  b += F("<div class=card><b>Status:</b> ");
  b += stationStatusText();
  if (ssid.length()) b += "<br><b>Saved network:</b> " + htmlEscape(ssid);
  if (connected) b += "<br><b>IP:</b> " + WiFi.localIP().toString();
  b += F("<p class=muted>The classroom keeps working without Wi-Fi: sensors, AUTO mode and Bluetooth "
         "control do not need the network.</p></div>");

  b += F("<div class=card><b>Nearby networks</b>");
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    b += F("<p class=muted>Scanning... this page refreshes.</p>");
  } else if (n <= 0) {
    b += F("<p class=muted>No networks found yet.</p>");
  } else {
    for (int i = 0; i < n && i < 20; i++) {
      String name = WiFi.SSID(i);
      if (name.length() == 0) continue;  // hidden network: type its name below
      String esc = htmlEscape(name);
      b += "<button type=button class=net data-s=\"" + esc + "\" onclick=\"document.getElementById('s').value=this.dataset.s;"
           "document.getElementById('p').focus()\">" + esc + " <span class=muted>(" + String(WiFi.RSSI(i)) + " dBm"
           + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? ", open" : "") + ")</span></button>";
    }
  }
  b += F("<form method=post action=/scan><button class=alt>Scan again</button></form></div>");

  b += F("<div class=card><form method=post action=/save>"
         "<label for=s>Network name (SSID)</label><input id=s name=ssid maxlength=32 required value=\"");
  b += htmlEscape(ssid);
  b += F("\"><label for=p>Password</label><input id=p name=password type=password maxlength=64 "
         "placeholder='leave empty for an open network'>"
         "<button>Save and connect</button></form></div>");

  if (ssid.length()) {
    b += F("<div class=card><form method=post action=/forget "
           "onsubmit=\"return confirm('Forget the saved network?')\">"
           "<button class=alt>Forget saved network</button></form></div>");
  }
  sendPage(200, b, n == WIFI_SCAN_RUNNING ? 4 : 0);
}

static void handleScan() {
  portalLastActivityAt = millis();
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  sendPage(200, F("<div class=card>Scanning for networks...</div>"), 4);
}

static void handleSave() {
  unsigned long now = millis();
  portalLastActivityAt = now;
  String ssid = portalServer.arg("ssid");
  String pass = portalServer.arg("password");

  // 802.11 limits: SSID 1-32 bytes; WPA2 passphrase 8-63 characters or a 64-hex-digit key.
  if (ssid.length() == 0 || ssid.length() > 32) {
    sendPage(400, F("<div class=card><p class=err>The network name must be 1-32 characters.</p><a href=/>Back</a></div>"));
    return;
  }
  if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 64)) {
    sendPage(400, F("<div class=card><p class=err>The password must be 8-63 characters (or empty for an open network).</p><a href=/>Back</a></div>"));
    return;
  }

  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  stationPassword = pass;
  { StateLock lock; wifiSsid = ssid; markStateDirty("wifi_saved"); }
  wifiConfigured = true;
  lastSaveAt = now;
  stationRetryMs = WIFI_RETRY_MS;
  Serial.printf("[WIFI] new network saved from setup page: %s\n", ssid.c_str());

  // Reply before reconnecting: joining the router may move the radio to another channel and
  // briefly drop the phone from the setup AP. The page refreshes to show the result.
  sendPage(200, "<div class=card>Saved. Connecting to <b>" + htmlEscape(ssid) +
                "</b>...<p class=muted>If your phone drops off this network, reconnect to " + String(SETUP_AP_SSID) +
                " and open http://192.168.4.1/ to see the result.</p></div>", 8);
  delay(100);
  startStation();
}

static void handleForget() {
  portalLastActivityAt = millis();
  wifiPrefs.remove("ssid");
  wifiPrefs.remove("pass");
  stationPassword = "";
  { StateLock lock; wifiSsid = ""; markStateDirty("wifi_forgotten"); }
  wifiConfigured = false;
  resetFirebaseConnection(true);
  WiFi.disconnect(false, true);
  Serial.println("[WIFI] saved network forgotten from setup page");
  sendPage(200, F("<div class=card>Saved network removed.</div>"), 3);
}

// Captive portal: phones probe a well-known URL; redirecting it opens the setup page.
static void handleNotFound() {
  portalLastActivityAt = millis();
  portalServer.sendHeader("Location", String("http://") + PORTAL_IP.toString() + "/", true);
  portalServer.send(302, "text/plain", "");
}
