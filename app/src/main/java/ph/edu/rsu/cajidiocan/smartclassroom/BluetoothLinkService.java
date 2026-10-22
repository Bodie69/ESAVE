package ph.edu.rsu.cajidiocan.smartclassroom;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.util.List;
import java.util.Locale;

public final class BluetoothLinkService extends Service implements BluetoothController.Listener {
    public interface Listener {
        void onBluetoothStatus(String message, boolean connected);
        void onState(SmartClassroomState state);
        void onAck(String action, boolean ok, String error);
        void onBluetoothError(String message);
        void onControllerBye(String reason);
    }

    private static final String PREFS = "persistent_bluetooth_link";
    private static final String KEY_KEEP = "keep_connected";
    private static final String KEY_ADDRESS = "device_address";
    private static final String KEY_NAME = "device_name";
    private static final String CHANNEL_ID = "rsu_bt_connection";
    private static final int NOTIFICATION_ID = 205;
    // While the controller is on the cloud its Bluetooth is off, so retrying every 30 s only
    // drains the battery. After a hand-off ("bye") or this many failed attempts, retry rarely:
    // the controller turns Bluetooth back on by itself if its Wi-Fi drops.
    private static final int FAST_RETRY_ATTEMPTS = 10;
    private static final long SLOW_RETRY_MS = 5 * 60_000L;
    private static final String ACTION_START = "ph.edu.rsu.cajidiocan.smartclassroom.BT_START";
    private static final String ACTION_DISCONNECT = "ph.edu.rsu.cajidiocan.smartclassroom.BT_DISCONNECT";

    public final class LocalBinder extends Binder {
        public BluetoothLinkService getService() { return BluetoothLinkService.this; }
    }

    private final IBinder binder = new LocalBinder();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Runnable reconnectTask = this::attemptReconnect;

    private BluetoothController controller;
    private SharedPreferences prefs;
    private volatile Listener listener;
    private volatile boolean connecting;
    private int reconnectAttempt;
    private volatile boolean cloudHandoff;
    private String lastStatus = "Bluetooth service ready";
    private boolean lastConnected;
    private SmartClassroomState lastState;

    /**
     * A connectedDevice foreground service cannot start without BLUETOOTH_CONNECT on Android 12+
     * (startForeground throws on Android 14+), so it is only started once that is granted.
     */
    static boolean hasConnectPermission(Context context) {
        return Build.VERSION.SDK_INT < 31
                || context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    public static void startPersistent(Context context) {
        if (!hasConnectPermission(context)) return;
        Intent i = new Intent(context, BluetoothLinkService.class).setAction(ACTION_START);
        try {
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(i);
            else context.startService(i);
        } catch (Exception ignored) {
            try { context.startService(i); } catch (Exception ignoredAgain) {}
        }
    }

    public static void startIfRemembered(Context context) {
        SharedPreferences p = context.getSharedPreferences(PREFS, MODE_PRIVATE);
        if (p.getBoolean(KEY_KEEP, false)) startPersistent(context);
    }

    @Override public void onCreate() {
        super.onCreate();
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        controller = new BluetoothController(this, this);
        createNotificationChannel();
        if (prefs.getBoolean(KEY_KEEP, false) && ensureForeground("Restoring ESP32 Bluetooth link...")) {
            scheduleReconnect(500L);
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? ACTION_START : intent.getAction();
        if (ACTION_DISCONNECT.equals(action)) {
            disconnectByUser();
            return START_NOT_STICKY;
        }
        if (prefs.getBoolean(KEY_KEEP, false)) {
            if (!ensureForeground(lastConnected ? "Connected to ESP32" : "Keeping ESP32 Bluetooth link active")) {
                stopSelf();
                return START_NOT_STICKY;
            }
            if (!isConnected() && !connecting) scheduleReconnect(250L);
            return START_STICKY;
        }
        // Nothing to keep alive (e.g. a sticky restart after the user disconnected). A
        // startForegroundService() caller still requires startForeground(), so satisfy it and
        // stop instead of leaving a permanent "Preparing" notification.
        ensureForeground("Bluetooth link idle");
        stopForegroundCompat();
        stopSelf();
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent intent) { return binder; }

    public void setListener(Listener listener) {
        this.listener = listener;
        if (listener != null) {
            listener.onBluetoothStatus(lastStatus, lastConnected);
            if (lastState != null) listener.onState(lastState);
        }
    }

    public void clearListener(Listener expected) {
        if (listener == expected) listener = null;
    }

    public BluetoothAdapter adapter() { return controller.adapter(); }
    public List<BluetoothDevice> pairedDevices() { return controller.pairedDevices(); }
    public boolean isConnected() { return controller != null && controller.isConnected(); }

    public void connect(BluetoothDevice device) {
        if (device == null) return;
        String address;
        String name;
        try {
            address = device.getAddress();
            name = device.getName();
        } catch (SecurityException e) {
            dispatchError("Nearby devices permission is required");
            return;
        }
        prefs.edit().putBoolean(KEY_KEEP, true).putString(KEY_ADDRESS, address)
                .putString(KEY_NAME, name == null ? BuildConfig.BLUETOOTH_DEVICE_NAME : name).apply();
        reconnectAttempt = 0;
        cloudHandoff = false;
        main.removeCallbacks(reconnectTask);
        ensureForeground("Connecting to " + safe(name) + "...");
        connecting = true;
        controller.connect(device);
    }

    public void disconnectByUser() {
        prefs.edit().putBoolean(KEY_KEEP, false).remove(KEY_ADDRESS).remove(KEY_NAME).apply();
        connecting = false;
        reconnectAttempt = 0;
        main.removeCallbacks(reconnectTask);
        if (controller != null) controller.disconnect();
        lastConnected = false;
        lastStatus = "Bluetooth disconnected by user";
        dispatchStatus(lastStatus, false);
        stopForegroundCompat();
        stopSelf();
    }

    private void stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE); else stopForeground(true);
    }

    public void requestState() { if (controller != null && controller.isConnected()) controller.requestState(); }
    public void setMode(boolean automatic) { if (controller != null) controller.setMode(automatic); }
    public void setRelay(String device, boolean state) { if (controller != null) controller.setRelay(device, state); }
    public void allOff() { if (controller != null) controller.allOff(); }
    public void setCount(int count) { if (controller != null) controller.setCount(count); }
    public void calibrateIr() { if (controller != null) controller.calibrateIr(); }
    public void resetEnergy() { if (controller != null) controller.resetEnergy(); }
    public void startWifiSetup() { if (controller != null) controller.startWifiSetup(); }

    /**
     * Reconnects to the remembered controller after {@code delayMs}, starting the backoff over.
     * @return false when no controller is remembered (the user has to pick one).
     */
    public boolean reconnectSoon(long delayMs) {
        if (!prefs.getBoolean(KEY_KEEP, false) || prefs.getString(KEY_ADDRESS, "").isEmpty()) return false;
        if (isConnected()) return true;
        reconnectAttempt = 0;
        cloudHandoff = false;
        scheduleReconnect(delayMs);
        return true;
    }

    private void scheduleReconnect(long delayMs) {
        if (!prefs.getBoolean(KEY_KEEP, false)) return;
        main.removeCallbacks(reconnectTask);
        main.postDelayed(reconnectTask, Math.max(250L, delayMs));
    }

    private void attemptReconnect() {
        if (!prefs.getBoolean(KEY_KEEP, false) || isConnected() || connecting) return;
        if (Build.VERSION.SDK_INT >= 31 && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            lastStatus = "Bluetooth permission required to restore link";
            updateNotification(lastStatus);
            scheduleReconnect(30000L);
            return;
        }
        BluetoothAdapter adapter = controller.adapter();
        if (adapter == null) {
            lastStatus = "Bluetooth adapter unavailable";
            updateNotification(lastStatus);
            scheduleReconnect(30000L);
            return;
        }
        try {
            if (!adapter.isEnabled()) {
                lastStatus = "Bluetooth is off • reconnect waits until it is enabled";
                updateNotification(lastStatus);
                scheduleReconnect(15000L);
                return;
            }
            String address = prefs.getString(KEY_ADDRESS, "");
            if (address.isEmpty()) return;
            BluetoothDevice device = adapter.getRemoteDevice(address);
            reconnectAttempt++;
            connecting = true;
            lastStatus = cloudHandoff ? "Controller is on the cloud • checking Bluetooth"
                    : "Reconnecting to ESP32 • attempt " + reconnectAttempt;
            updateNotification(lastStatus);
            controller.connect(device);
        } catch (Exception e) {
            connecting = false;
            lastStatus = "Bluetooth reconnect waiting: " + safe(e.getMessage());
            updateNotification(lastStatus);
            scheduleReconnect(backoffMs());
        }
    }

    private long backoffMs() {
        if (cloudHandoff || reconnectAttempt >= FAST_RETRY_ATTEMPTS) return SLOW_RETRY_MS;
        int exp = Math.min(5, Math.max(0, reconnectAttempt));
        return Math.min(30000L, 1500L * (1L << exp));
    }

    @Override public void onBluetoothStatus(String message, boolean connected) {
        lastStatus = message == null ? (connected ? "Bluetooth connected" : "Bluetooth disconnected") : message;
        // A drop right after "bye" keeps the hand-off status instead of "disconnected".
        if (!connected && cloudHandoff) lastStatus = "Controller is back on the cloud • Bluetooth retries every 5 min";
        lastConnected = connected;
        if (connected) {
            connecting = false;
            reconnectAttempt = 0;
            cloudHandoff = false;
            main.removeCallbacks(reconnectTask);
            ensureForeground("Connected to " + prefs.getString(KEY_NAME, BuildConfig.BLUETOOTH_DEVICE_NAME));
        } else if (!lastStatus.toLowerCase(Locale.ROOT).startsWith("connecting")) {
            connecting = false;
            if (prefs.getBoolean(KEY_KEEP, false)) scheduleReconnect(backoffMs());
        }
        updateNotification(lastStatus);
        dispatchStatus(lastStatus, connected);
    }

    @Override public void onState(SmartClassroomState state) {
        lastState = state;
        Listener l = listener;
        if (l != null) l.onState(state);
    }

    @Override public void onAck(String action, boolean ok, String error) {
        Listener l = listener;
        if (l != null) l.onAck(action, ok, error);
    }

    @Override public void onControllerBye(String reason) {
        cloudHandoff = true;
        lastStatus = "Controller is back on the cloud • Bluetooth retries every 5 min";
        updateNotification(lastStatus);
        Listener l = listener;
        if (l != null) l.onControllerBye(reason);
    }

    @Override public void onBluetoothError(String message) {
        connecting = false;
        lastConnected = false;
        lastStatus = message == null ? "Bluetooth link lost" : message;
        updateNotification(lastStatus);
        dispatchError(lastStatus);
        if (prefs.getBoolean(KEY_KEEP, false)) scheduleReconnect(backoffMs());
    }

    private void dispatchStatus(String message, boolean connected) {
        Listener l = listener;
        if (l != null) l.onBluetoothStatus(message, connected);
    }

    private void dispatchError(String message) {
        Listener l = listener;
        if (l != null) l.onBluetoothError(message);
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel c = new NotificationChannel(CHANNEL_ID, "ESP32 Bluetooth connection", NotificationManager.IMPORTANCE_LOW);
            c.setDescription("Keeps the ESAVE ESP32 Bluetooth link active when the app is closed.");
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) nm.createNotificationChannel(c);
        }
    }

    private Notification buildNotification(String text) {
        Intent open = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent openPi = PendingIntent.getActivity(this, 205, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Intent disconnect = new Intent(this, BluetoothLinkService.class).setAction(ACTION_DISCONNECT);
        PendingIntent disconnectPi = PendingIntent.getService(this, 206, disconnect,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder b = Build.VERSION.SDK_INT >= 26 ? new Notification.Builder(this, CHANNEL_ID) : new Notification.Builder(this);
        return b.setSmallIcon(R.drawable.ic_menu_white)
                .setContentTitle("ESAVE • Bluetooth")
                .setContentText(text)
                .setContentIntent(openPi)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(R.drawable.ic_menu_white, "Disconnect", disconnectPi)
                .build();
    }

    /** @return false when Android refused foreground mode (e.g. Bluetooth permission revoked). */
    private boolean ensureForeground(String text) {
        Notification n = buildNotification(text);
        try {
            if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            else startForeground(NOTIFICATION_ID, n);
            return true;
        } catch (RuntimeException e) {
            // SecurityException without BLUETOOTH_CONNECT on Android 14+, or
            // ForegroundServiceStartNotAllowedException when started from the background.
            lastStatus = "Bluetooth background link unavailable: " + safe(e.getMessage());
            dispatchError(lastStatus);
            return false;
        }
    }

    private void updateNotification(String text) {
        if (!prefs.getBoolean(KEY_KEEP, false)) return;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null) nm.notify(NOTIFICATION_ID, buildNotification(text));
    }

    @Override public void onTaskRemoved(Intent rootIntent) {
        // Intentionally do not disconnect. The user's explicit Disconnect action is authoritative.
        if (prefs.getBoolean(KEY_KEEP, false) && !ensureForeground(lastConnected ? "Connected to ESP32" : "Keeping ESP32 Bluetooth link active")) {
            stopSelf();
        }
        super.onTaskRemoved(rootIntent);
    }

    @Override public void onDestroy() {
        main.removeCallbacks(reconnectTask);
        if (controller != null) controller.shutdown();
        super.onDestroy();
    }

    private static String safe(String s) { return s == null || s.trim().isEmpty() ? "ESP32" : s; }
}
