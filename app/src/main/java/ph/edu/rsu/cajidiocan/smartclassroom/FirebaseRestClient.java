package ph.edu.rsu.cajidiocan.smartclassroom;

import android.content.Context;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class FirebaseRestClient {
    private static final int TIMEOUT_MS = 6000;
    private static final long TOKEN_REFRESH_MARGIN_MS = 120_000L;
    private static final long ACK_TIMEOUT_MS = 8_000L;
    private static final long ACK_POLL_MS = 350L;
    private static final AtomicLong LAST_COMMAND_ID = new AtomicLong(System.currentTimeMillis());

    private final String apiKey = BuildConfig.FIREBASE_API_KEY;
    private final String dbUrl = trimSlash(BuildConfig.FIREBASE_DATABASE_URL);
    private final String roomBase = "/rooms/" + BuildConfig.ROOM_ID;
    private final SecureTokenStore tokenStore;
    private final Object tokenLock = new Object();
    private final Object refreshLock = new Object();
    private final Object streamLock = new Object();
    private volatile HttpURLConnection streamConnection;

    private String idToken = "";
    private String refreshToken = "";
    private long tokenExpiryMs = 0L;
    private long sessionGeneration = 0L;

    public FirebaseRestClient(Context context) {
        tokenStore = new SecureTokenStore(context.getSharedPreferences("firebase_session", Context.MODE_PRIVATE));
        refreshToken = tokenStore.load();
    }

    private static String trimSlash(String s) {
        if (s == null) return "";
        while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
        return s;
    }

    public boolean hasSavedSession() {
        synchronized (tokenLock) {
            return !refreshToken.isEmpty();
        }
    }

    public void clearSession() {
        synchronized (tokenLock) {
            sessionGeneration++;
            idToken = "";
            refreshToken = "";
            tokenExpiryMs = 0L;
        }
        tokenStore.clear();
    }

    public boolean signIn(String email, String password) throws Exception {
        final long generationAtStart;
        synchronized (tokenLock) {
            generationAtStart = sessionGeneration;
        }

        JSONObject body = new JSONObject();
        body.put("email", email);
        body.put("password", password);
        body.put("returnSecureToken", true);
        HttpResult r = requestRaw("POST",
                "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + apiKey,
                body.toString(), "application/json; charset=utf-8");
        if (r.code != 200) {
            throw new Exception(extractFirebaseError(r.body, "Firebase sign-in failed (HTTP " + r.code + ")"));
        }

        JSONObject j = new JSONObject(r.body);
        String newIdToken = j.getString("idToken");
        String newRefreshToken = j.getString("refreshToken");
        long expires = parsePositiveLong(j.optString("expiresIn", "3600"), 3600L);

        synchronized (tokenLock) {
            if (generationAtStart != sessionGeneration) {
                throw new Exception("Firebase sign-in was cancelled");
            }
            idToken = newIdToken;
            refreshToken = newRefreshToken;
            tokenExpiryMs = System.currentTimeMillis() + expires * 1000L;
        }
        tokenStore.save(newRefreshToken);
        return true;
    }

    private boolean refresh() throws Exception {
        synchronized (refreshLock) {
            final String tokenToRefresh;
            final long generationAtStart;
            synchronized (tokenLock) {
                if (!idToken.isEmpty() && System.currentTimeMillis() < tokenExpiryMs - TOKEN_REFRESH_MARGIN_MS) {
                    return true;
                }
                tokenToRefresh = refreshToken;
                generationAtStart = sessionGeneration;
            }
            if (tokenToRefresh.isEmpty()) return false;

            String form = "grant_type=refresh_token&refresh_token=" + URLEncoder.encode(tokenToRefresh, "UTF-8");
            HttpResult r = requestRaw("POST",
                    "https://securetoken.googleapis.com/v1/token?key=" + apiKey,
                    form, "application/x-www-form-urlencoded; charset=utf-8");

            if (r.code != 200) {
                synchronized (tokenLock) {
                    if (generationAtStart == sessionGeneration && tokenToRefresh.equals(refreshToken)) {
                        sessionGeneration++;
                        idToken = "";
                        refreshToken = "";
                        tokenExpiryMs = 0L;
                        tokenStore.clear();
                    }
                }
                throw new Exception(extractFirebaseError(r.body, "Firebase session expired"));
            }

            JSONObject j = new JSONObject(r.body);
            String newIdToken = j.getString("id_token");
            String newRefreshToken = j.getString("refresh_token");
            long expires = parsePositiveLong(j.optString("expires_in", "3600"), 3600L);

            synchronized (tokenLock) {
                if (generationAtStart != sessionGeneration || !tokenToRefresh.equals(refreshToken)) {
                    throw new Exception("Firebase session changed while refreshing");
                }
                idToken = newIdToken;
                refreshToken = newRefreshToken;
                tokenExpiryMs = System.currentTimeMillis() + expires * 1000L;
            }
            tokenStore.save(newRefreshToken);
            return true;
        }
    }

    private void ensureToken() throws Exception {
        synchronized (tokenLock) {
            if (!idToken.isEmpty() && System.currentTimeMillis() < tokenExpiryMs - TOKEN_REFRESH_MARGIN_MS) {
                return;
            }
            if (refreshToken.isEmpty()) throw new Exception("Firebase login required");
        }
        if (!refresh()) throw new Exception("Firebase login required");
    }

    private String currentIdToken() {
        synchronized (tokenLock) {
            return idToken;
        }
    }

    private void invalidateIdToken() {
        synchronized (tokenLock) {
            idToken = "";
            tokenExpiryMs = 0L;
        }
    }

    private HttpResult dbRequest(String method, String path, String json) throws Exception {
        ensureToken();
        String token = currentIdToken();
        String url = dbUrl + path + ".json?auth=" + URLEncoder.encode(token, "UTF-8");
        HttpResult r = requestRaw(method, url, json, "application/json; charset=utf-8");
        if (r.code == 401 || r.code == 403) {
            invalidateIdToken();
            ensureToken();
            token = currentIdToken();
            url = dbUrl + path + ".json?auth=" + URLEncoder.encode(token, "UTF-8");
            r = requestRaw(method, url, json, "application/json; charset=utf-8");
        }
        if (r.code < 200 || r.code >= 300) {
            throw new Exception("RTDB HTTP " + r.code + ": " + safeBody(r.body));
        }
        return r;
    }


    public interface StateStreamListener {
        void onOpen();
        void onState(SmartClassroomState state);
    }

    /**
     * Firebase Realtime Database REST streaming (Server-Sent Events). This blocks on a
     * background executor until cancelled. Because the ESP32 PUTs the complete /state node,
     * root PUT events are authoritative hardware snapshots and can update the UI immediately.
     */
    public void streamState(AtomicBoolean stopSignal, StateStreamListener listener) throws Exception {
        ensureToken();
        String token = currentIdToken();
        String target = dbUrl + roomBase + "/state.json?auth=" + URLEncoder.encode(token, "UTF-8");
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(target).openConnection();
            c.setRequestMethod("GET");
            c.setConnectTimeout(TIMEOUT_MS);
            c.setReadTimeout(0);
            c.setUseCaches(false);
            c.setRequestProperty("Accept", "text/event-stream");
            c.setRequestProperty("Cache-Control", "no-cache");
            synchronized (streamLock) { streamConnection = c; }

            int code = c.getResponseCode();
            if (code == 401 || code == 403) {
                invalidateIdToken();
                throw new Exception("Firebase stream authorization expired");
            }
            if (code != 200) {
                InputStream es = c.getErrorStream();
                String body = readAll(es);
                throw new Exception("Firebase stream HTTP " + code + ": " + safeBody(body));
            }
            if (listener != null) listener.onOpen();

            try (BufferedReader br = new BufferedReader(new InputStreamReader(c.getInputStream(), StandardCharsets.UTF_8))) {
                String event = "";
                StringBuilder data = new StringBuilder();
                String line;
                while (!stopSignal.get() && (line = br.readLine()) != null) {
                    if (line.startsWith("event:")) {
                        event = line.substring(6).trim();
                    } else if (line.startsWith("data:")) {
                        if (data.length() > 0) data.append('\n');
                        data.append(line.substring(5).trim());
                    } else if (line.isEmpty()) {
                        if (("put".equals(event) || "patch".equals(event)) && data.length() > 0) {
                            JSONObject envelope = new JSONObject(data.toString());
                            String path = envelope.optString("path", "");
                            Object payload = envelope.opt("data");
                            if (("/".equals(path) || path.isEmpty()) && payload instanceof JSONObject && listener != null) {
                                listener.onState(SmartClassroomState.fromJson((JSONObject) payload));
                            }
                        } else if ("cancel".equals(event) || "auth_revoked".equals(event)) {
                            invalidateIdToken();
                            throw new Exception("Firebase stream cancelled/revoked: " + data);
                        }
                        event = "";
                        data.setLength(0);
                    }
                }
            }
            if (!stopSignal.get()) throw new Exception("Firebase stream closed by server");
        } finally {
            synchronized (streamLock) {
                if (streamConnection == c) streamConnection = null;
            }
            if (c != null) c.disconnect();
        }
    }

    public void closeStateStream() {
        HttpURLConnection c;
        synchronized (streamLock) {
            c = streamConnection;
            streamConnection = null;
        }
        if (c != null) c.disconnect();
    }

    private static String readAll(InputStream is) throws Exception {
        if (is == null) return "";
        StringBuilder sb = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(is, StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) sb.append(line);
        }
        return sb.toString();
    }

    public SmartClassroomState getState() throws Exception {
        HttpResult r = dbRequest("GET", roomBase + "/state", null);
        if (r.body == null || r.body.trim().isEmpty() || r.body.trim().equals("null")) {
            throw new Exception("Controller state does not exist yet");
        }
        return SmartClassroomState.fromJson(new JSONObject(r.body));
    }

    public long setMode(boolean automatic) throws Exception {
        JSONObject c = command("android_firebase");
        c.put("mode", automatic ? "auto" : "manual");
        dbRequest("PUT", roomBase + "/commands/mode", c.toString());
        return c.getLong("command_id");
    }

    public long setRelay(String device, boolean state) throws Exception {
        JSONObject c = command("android_firebase");
        c.put("state", state);
        dbRequest("PUT", roomBase + "/commands/devices/" + device, c.toString());
        return c.getLong("command_id");
    }

    public long allOff() throws Exception {
        JSONObject c = command("android_firebase");
        dbRequest("PUT", roomBase + "/commands/all_off", c.toString());
        return c.getLong("command_id");
    }

    public long setCount(int count) throws Exception {
        JSONObject c = command("android_firebase");
        c.put("count", count);
        dbRequest("PUT", roomBase + "/commands/set_count", c.toString());
        return c.getLong("command_id");
    }

    public long calibrateIr() throws Exception {
        JSONObject c = command("android_firebase");
        dbRequest("PUT", roomBase + "/commands/calibrate_ir", c.toString());
        return c.getLong("command_id");
    }

    /** Firmware V25.20+: the controller stops Firebase and turns Bluetooth on for a local session. */
    public long bluetoothSession() throws Exception {
        JSONObject c = command("android_firebase");
        dbRequest("PUT", roomBase + "/commands/bluetooth_session", c.toString());
        return c.getLong("command_id");
    }

    public long resetEnergy() throws Exception {
        JSONObject c = command("android_firebase");
        dbRequest("PUT", roomBase + "/commands/reset_energy", c.toString());
        return c.getLong("command_id");
    }

    public FirebaseAck waitForAck(long commandId) throws Exception {
        long deadline = System.currentTimeMillis() + ACK_TIMEOUT_MS;
        String ownSlot = roomBase + "/acks/by_id/" + commandId;
        while (System.currentTimeMillis() < deadline) {
            // Firmware V25.16+ writes a per-command ACK, which other phones cannot overwrite.
            FirebaseAck ack = parseAck(dbRequest("GET", ownSlot, null), commandId);
            if (ack != null) {
                try { dbRequest("DELETE", ownSlot, null); } catch (Exception ignored) {} // tidy up; harmless if it fails
                return ack;
            }
            // Older firmware only writes the shared acks/last slot.
            ack = parseAck(dbRequest("GET", roomBase + "/acks/last", null), commandId);
            if (ack != null) return ack;
            try { Thread.sleep(ACK_POLL_MS); }
            catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new Exception("Interrupted while waiting for ESP32 ACK");
            }
        }
        throw new Exception("ESP32 ACK timeout. Check controller online status and Firebase command path.");
    }

    /** @return the ACK in {@code r} if it belongs to {@code commandId}, else null */
    private static FirebaseAck parseAck(HttpResult r, long commandId) throws Exception {
        String body = r.body == null ? "" : r.body.trim();
        if (body.isEmpty() || "null".equals(body)) return null;
        JSONObject j = new JSONObject(body);
        if (j.optLong("command_id", 0L) != commandId) return null;
        return new FirebaseAck(commandId, j.optBoolean("ok", false),
                j.optString("action", "command"), j.optString("detail", ""));
    }

    private static JSONObject command(String source) throws Exception {
        JSONObject c = new JSONObject();
        long now = System.currentTimeMillis();
        long id = LAST_COMMAND_ID.updateAndGet(previous -> Math.max(now, previous + 1L));
        c.put("command_id", id);
        c.put("source", source);
        c.put("requested_at_ms", now);
        return c;
    }

    public static final class FirebaseAck {
        public final long commandId;
        public final boolean ok;
        public final String action;
        public final String detail;
        FirebaseAck(long commandId, boolean ok, String action, String detail) {
            this.commandId = commandId;
            this.ok = ok;
            this.action = action == null ? "command" : action;
            this.detail = detail == null ? "" : detail;
        }
    }

    private static String extractFirebaseError(String raw, String fallback) {
        try {
            if (raw == null || raw.trim().isEmpty()) return fallback;
            JSONObject root = new JSONObject(raw);
            JSONObject e = root.optJSONObject("error");
            if (e != null) return e.optString("message", fallback);
        } catch (Exception ignored) {}
        return fallback;
    }

    private static long parsePositiveLong(String raw, long fallback) {
        try {
            long value = Long.parseLong(raw);
            return value > 0 ? value : fallback;
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private static String safeBody(String body) {
        if (body == null || body.trim().isEmpty()) return "empty response";
        String compact = body.replace('\n', ' ').replace('\r', ' ').trim();
        return compact.length() > 500 ? compact.substring(0, 500) + "…" : compact;
    }

    private static HttpResult requestRaw(String method, String target, String body, String contentType) throws Exception {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(target).openConnection();
            c.setRequestMethod(method);
            c.setConnectTimeout(TIMEOUT_MS);
            c.setReadTimeout(TIMEOUT_MS);
            c.setUseCaches(false);
            c.setRequestProperty("Accept", "application/json");
            c.setRequestProperty("Cache-Control", "no-cache");
            // Do not force Connection: close. Android HttpURLConnection can reuse HTTPS
            // connections, reducing TLS overhead for the fallback refresh and ACK polling.
            if (body != null) {
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", contentType);
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                c.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream os = c.getOutputStream()) {
                    os.write(bytes);
                    os.flush();
                }
            }

            int code = c.getResponseCode();
            InputStream is = code >= 200 && code < 400 ? c.getInputStream() : c.getErrorStream();
            StringBuilder sb = new StringBuilder();
            if (is != null) {
                try (BufferedReader br = new BufferedReader(new InputStreamReader(is, StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line);
                }
            }
            return new HttpResult(code, sb.toString());
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static final class HttpResult {
        final int code;
        final String body;
        HttpResult(int code, String body) {
            this.code = code;
            this.body = body;
        }
    }
}
