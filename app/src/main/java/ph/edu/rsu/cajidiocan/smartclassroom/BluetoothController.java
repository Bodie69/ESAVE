package ph.edu.rsu.cajidiocan.smartclassroom;

import android.Manifest;
import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class BluetoothController {
    public interface Listener {
        void onBluetoothStatus(String message, boolean connected);
        void onState(SmartClassroomState state);
        void onAck(String action, boolean ok, String error);
        void onBluetoothError(String message);
        /** Firmware 25.20+: the controller is about to switch Bluetooth off (e.g. "cloud_resume"). */
        void onControllerBye(String reason);
    }

    private static final UUID SPP_UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");

    private final Context context;
    private final Listener listener;
    private final Object connectionLock = new Object();
    private final ExecutorService connectionIo = Executors.newSingleThreadExecutor();
    private final ExecutorService writerIo = Executors.newSingleThreadExecutor();

    private BluetoothSocket socket;
    private OutputStream output;
    private volatile boolean connected;
    private volatile boolean shuttingDown;
    private long connectionGeneration;

    public BluetoothController(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
    }

    public BluetoothAdapter adapter() {
        return BluetoothAdapter.getDefaultAdapter();
    }

    private boolean canConnect() {
        return Build.VERSION.SDK_INT < 31 ||
                context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    public List<BluetoothDevice> pairedDevices() throws SecurityException {
        BluetoothAdapter a = adapter();
        List<BluetoothDevice> out = new ArrayList<>();
        if (a == null) return out;
        if (!canConnect()) throw new SecurityException("Nearby devices permission is required");
        Set<BluetoothDevice> bonded = a.getBondedDevices();
        if (bonded != null) out.addAll(bonded);
        out.sort(Comparator.comparing(this::safeDeviceName, String.CASE_INSENSITIVE_ORDER));
        return out;
    }

    public boolean isConnected() {
        return connected;
    }

    // BLUETOOTH_CONNECT is verified by canConnect() inside the worker before any socket call,
    // and SecurityException is caught there; lint cannot see through the helper.
    @SuppressLint("MissingPermission")
    public void connect(BluetoothDevice device) {
        if (device == null || shuttingDown) return;

        final long generation;
        final BluetoothSocket oldSocket;
        synchronized (connectionLock) {
            connectionGeneration++;
            generation = connectionGeneration;
            connected = false;
            oldSocket = socket;
            socket = null;
            output = null;
        }
        closeQuietly(oldSocket);

        listener.onBluetoothStatus("Connecting to " + safeDeviceName(device) + "...", false);

        connectionIo.execute(() -> {
            BluetoothSocket localSocket = null;
            boolean reachedConnectedState = false;
            try {
                if (shuttingDown || !isGenerationCurrent(generation)) return;
                if (!canConnect()) throw new SecurityException("Nearby devices permission is required");

                BluetoothAdapter a = adapter();
                if (a == null) throw new Exception("This phone has no Bluetooth adapter");
                if (!a.isEnabled()) throw new Exception("Bluetooth is turned off");

                localSocket = device.createRfcommSocketToServiceRecord(SPP_UUID);
                synchronized (connectionLock) {
                    if (shuttingDown || generation != connectionGeneration) {
                        closeQuietly(localSocket);
                        return;
                    }
                    socket = localSocket;
                }

                localSocket.connect();
                OutputStream localOutput = localSocket.getOutputStream();
                synchronized (connectionLock) {
                    if (shuttingDown || generation != connectionGeneration || socket != localSocket) {
                        closeQuietly(localSocket);
                        return;
                    }
                    output = localOutput;
                    connected = true;
                }
                reachedConnectedState = true;
                listener.onBluetoothStatus("Bluetooth connected: " + safeDeviceName(device), true);

                sendJsonNow(command("get_state"), generation);
                readLoop(localSocket, generation);

                if (isGenerationCurrent(generation)) {
                    disconnectGeneration(generation, localSocket, "Bluetooth disconnected", false);
                }
            } catch (Exception e) {
                if (!isGenerationCurrent(generation)) {
                    closeQuietly(localSocket);
                    return;
                }
                String detail = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                disconnectGeneration(generation, localSocket, null, false);
                if (reachedConnectedState) {
                    listener.onBluetoothStatus("Bluetooth disconnected", false);
                    listener.onBluetoothError("Bluetooth link lost: " + detail);
                } else {
                    listener.onBluetoothError("Bluetooth connection failed: " + detail);
                }
            }
        });
    }

    private void readLoop(BluetoothSocket localSocket, long generation) throws Exception {
        BufferedReader br = new BufferedReader(new InputStreamReader(localSocket.getInputStream(), StandardCharsets.UTF_8));
        String line;
        while (isConnectionCurrent(generation, localSocket) && (line = br.readLine()) != null) {
            line = line.trim();
            if (line.isEmpty()) continue;
            try {
                JSONObject j = new JSONObject(line);
                String type = j.optString("type", "");
                if ("state".equals(type)) {
                    listener.onState(SmartClassroomState.fromJson(j));
                } else if ("ack".equals(type)) {
                    listener.onAck(j.optString("action", "command"), j.optBoolean("ok", false), j.optString("error", ""));
                } else if ("bye".equals(type)) {
                    listener.onControllerBye(j.optString("reason", ""));
                }
            } catch (Exception ignored) {
                // Firmware may also print debug lines. Only newline-delimited JSON protocol lines are consumed.
            }
        }
    }

    public void disconnect() {
        if (shuttingDown) return;
        BluetoothSocket toClose;
        boolean wasActive;
        synchronized (connectionLock) {
            wasActive = connected || socket != null;
            connectionGeneration++;
            connected = false;
            toClose = socket;
            socket = null;
            output = null;
        }
        closeQuietly(toClose);
        if (wasActive) listener.onBluetoothStatus("Bluetooth disconnected", false);
    }

    public void requestState() {
        send(command("get_state"));
    }

    public void setMode(boolean automatic) {
        JSONObject j = command("set_mode");
        try { j.put("mode", automatic ? "auto" : "manual"); } catch (Exception ignored) {}
        send(j);
    }

    public void setRelay(String device, boolean state) {
        JSONObject j = command("set_relay");
        try {
            j.put("device", device);
            j.put("state", state);
        } catch (Exception ignored) {}
        send(j);
    }

    public void allOff() {
        send(command("all_off"));
    }

    public void setCount(int count) {
        JSONObject j = command("set_count");
        try { j.put("count", count); } catch (Exception ignored) {}
        send(j);
    }

    public void calibrateIr() {
        send(command("calibrate_ir"));
    }

    public void resetEnergy() {
        send(command("reset_energy"));
    }

    /** Firmware V25.18+: opens the controller's Wi-Fi setup access point (Bluetooth only). */
    public void startWifiSetup() {
        send(command("wifi_setup"));
    }

    private static JSONObject command(String cmd) {
        JSONObject j = new JSONObject();
        try {
            j.put("cmd", cmd);
            j.put("id", System.currentTimeMillis());
        } catch (Exception ignored) {}
        return j;
    }

    private void send(JSONObject j) {
        if (shuttingDown) return;
        final long generation;
        synchronized (connectionLock) {
            generation = connectionGeneration;
        }
        writerIo.execute(() -> {
            try {
                sendJsonNow(j, generation);
            } catch (Exception e) {
                if (!isGenerationCurrent(generation)) return;
                String detail = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
                BluetoothSocket current;
                synchronized (connectionLock) {
                    current = socket;
                }
                disconnectGeneration(generation, current, null, false);
                listener.onBluetoothError("Bluetooth send failed: " + detail);
            }
        });
    }

    private void sendJsonNow(JSONObject j, long generation) throws Exception {
        final OutputStream localOutput;
        synchronized (connectionLock) {
            if (shuttingDown || generation != connectionGeneration || !connected || output == null) {
                throw new Exception("Bluetooth is not connected");
            }
            localOutput = output;
        }
        byte[] b = (j.toString() + "\n").getBytes(StandardCharsets.UTF_8);
        synchronized (localOutput) {
            localOutput.write(b);
            localOutput.flush();
        }
    }

    private boolean isGenerationCurrent(long generation) {
        synchronized (connectionLock) {
            return !shuttingDown && generation == connectionGeneration;
        }
    }

    private boolean isConnectionCurrent(long generation, BluetoothSocket localSocket) {
        synchronized (connectionLock) {
            return !shuttingDown && generation == connectionGeneration && connected && socket == localSocket;
        }
    }

    private void disconnectGeneration(long generation, BluetoothSocket expectedSocket, String status, boolean reportError) {
        BluetoothSocket toClose;
        synchronized (connectionLock) {
            if (generation != connectionGeneration) {
                closeQuietly(expectedSocket);
                return;
            }
            connectionGeneration++;
            connected = false;
            toClose = socket != null ? socket : expectedSocket;
            socket = null;
            output = null;
        }
        closeQuietly(toClose);
        if (status != null && !shuttingDown) listener.onBluetoothStatus(status, false);
        if (reportError && !shuttingDown) listener.onBluetoothError(status == null ? "Bluetooth disconnected" : status);
    }

    private String safeDeviceName(BluetoothDevice device) {
        if (device == null) return "Bluetooth device";
        try {
            String name = device.getName();
            return name == null || name.trim().isEmpty() ? "Bluetooth device" : name;
        } catch (SecurityException e) {
            return "Bluetooth device";
        }
    }

    private static void closeQuietly(BluetoothSocket s) {
        if (s == null) return;
        try { s.close(); } catch (Exception ignored) {}
    }

    public void shutdown() {
        shuttingDown = true;
        BluetoothSocket toClose;
        synchronized (connectionLock) {
            connectionGeneration++;
            connected = false;
            toClose = socket;
            socket = null;
            output = null;
        }
        closeQuietly(toClose);
        connectionIo.shutdownNow();
        writerIo.shutdownNow();
    }
}
