package ph.edu.rsu.cajidiocan.smartclassroom;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.ComponentName;
import android.content.ServiceConnection;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.IBinder;
import android.provider.Settings;
import android.text.InputType;
import android.animation.ObjectAnimator;
import android.animation.ValueAnimator;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.LinearInterpolator;
import android.view.WindowInsets;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.TextView;
import android.widget.Toast;
import android.app.Dialog;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.InsetDrawable;
import android.text.method.HideReturnsTransformationMethod;
import android.text.method.PasswordTransformationMethod;
import android.util.Patterns;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.ImageButton;
import android.util.TypedValue;

import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends Activity implements BluetoothLinkService.Listener {
    private enum Transport { FIREBASE, BLUETOOTH }
    private Transport transport = Transport.FIREBASE;
//my own code not ai
    private static final int SCREEN_DASHBOARD = 0;
    private static final int SCREEN_DEVICES = 1;
    private static final int SCREEN_ENERGY = 2;
    private static final int SCREEN_SETTINGS = 3;
    private static final int SCREEN_ACCOUNT = 4;
    private static final int REQ_BLUETOOTH_CONNECT = 501;
    private static final int REQ_ENABLE_BLUETOOTH = 502;
    private static final int REQ_POST_NOTIFICATIONS = 503;
    // The ESP32 pushes state every 500 ms; only ask explicitly when pushes stop arriving.
    private static final long BLUETOOTH_STATE_STALE_MS = 2000L;
    private static final String STATE_SCREEN = "selected_screen";
    private static final String STATE_TRANSPORT = "selected_transport";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService network = Executors.newFixedThreadPool(2);
    private final ExecutorService commands = Executors.newSingleThreadExecutor();
    private final AtomicBoolean firebasePollInProgress = new AtomicBoolean(false);
    private final AtomicBoolean firebaseStreamStop = new AtomicBoolean(true);
    private final AtomicBoolean firebaseStreamTaskActive = new AtomicBoolean(false);
    private final AtomicBoolean firebaseStreamRunning = new AtomicBoolean(false);

    private FirebaseRestClient firebase;
    private BluetoothLinkService bluetooth;
    private boolean bluetoothBound;

    private final ServiceConnection bluetoothConnection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            BluetoothLinkService.LocalBinder binder = (BluetoothLinkService.LocalBinder) service;
            bluetooth = binder.getService();
            bluetoothBound = true;
            bluetooth.setListener(MainActivity.this);
            updateTransportStatus();
            updateSessionUi();
            if (transport == Transport.BLUETOOTH && bluetooth.isConnected()) bluetooth.requestState();
        }

        @Override public void onServiceDisconnected(ComponentName name) {
            bluetoothBound = false;
            bluetooth = null;
            updateTransportStatus();
            updateSessionUi();
        }
    };
    // Written by network threads (login, session errors) and read on the UI thread.
    private volatile boolean firebaseReady;
    private boolean notificationPermissionAsked;
    private long lastBluetoothStateAt;
    private boolean pollingActive;
    private SmartClassroomState currentState;
    private Transport currentStateTransport;
    private long lastChartTimestamp = Long.MIN_VALUE;

    private TextView txtHeaderMode;
    private TextView txtTransportStatus, txtSettingsTransportStatus, txtDeviceStatus, txtConnectivity, txtLastSource, txtDiagnostics, txtWifiSetupStatus;
    private TextView txtPeople, txtBeams, txtOccupancyValidity, txtSensorQuality;
    private TextView txtVoltage, txtCurrent, txtPower, txtEnergy, txtFrequencyPf, txtEnergyFreshness;
    private TextView txtMode, txtMessage, txtRelaySummary, txtManualHint;
    private TextView txtDashboardPower, txtDashboardEnergy, txtDashboardRelaySummary, txtDashboardAlert, txtDashboardMode, txtDashboardUpdated;
    private TextView txtSystemFirebase, txtSystemBluetooth, txtAppVersion;
    private RadioButton radioFirebase, radioBluetooth;
    // CH1-CH6 tap targets: bulbs for light1-4, fans for fan1-2 (same order as deviceKeys).
    private View[] relayTiles;
    private ImageView[] relayIcons;
    // Per fan: the running animation (spin-up, steady spin or coast-down), or null when still.
    private final android.animation.Animator[] fanSpins = new android.animation.Animator[2];
    private final boolean[] fanOn = new boolean[2];
    // Per bulb: breathing animation of the yellow glow while the light is ON.
    private final ValueAnimator[] bulbPulses = new ValueAnimator[4];
    private View[] bulbGlows;
    // Steady spin speed. Spin-up (accelerate over 180 deg) and coast-down (decelerate over 240 deg)
    // are timed to start/end at exactly this speed, so there is no jerk between phases.
    private static final long FAN_TURN_MS = 200L; // 5 turns/s; ic_fan_blur keeps it smooth
    private EnergyChartView energyChart;

    private View rootMain;
    private LinearLayout bottomNavigation;

    private View[] screens;
    private LinearLayout[] navItems;
    private ImageView[] navIcons;
    private TextView[] navLabels;
    private int selectedScreen = SCREEN_DASHBOARD;

    private final int[] navActiveIcons = {
            R.drawable.nav_home_active, R.drawable.nav_devices_active, R.drawable.nav_energy_active,
            R.drawable.nav_settings_active, R.drawable.nav_account_active
    };
    private final int[] navInactiveIcons = {
            R.drawable.nav_home_inactive, R.drawable.nav_devices_inactive, R.drawable.nav_energy_inactive,
            R.drawable.nav_settings_inactive, R.drawable.nav_account_inactive
    };

    private final String[] deviceKeys = {"light1","light2","light3","light4","fan1","fan2"};

    private final Runnable pollTask = new Runnable() {
        @Override public void run() {
            if (!pollingActive) return;
            if (transport == Transport.FIREBASE && firebaseReady && !firebaseStreamTaskActive.get()) {
                startFirebaseRealtimeStream();
            }
            if (transport == Transport.FIREBASE && firebaseReady && !firebaseStreamRunning.get()
                    && firebasePollInProgress.compareAndSet(false, true)) {
                // Fallback GET only while the RTDB realtime stream is reconnecting.
                network.execute(() -> {
                    try {
                        SmartClassroomState state = firebase.getState();
                        ui.post(() -> { if (transport == Transport.FIREBASE && firebaseReady && pollingActive) renderState(state, true); });
                    } catch (Exception e) {
                        ui.post(() -> { if (transport == Transport.FIREBASE && firebaseReady && pollingActive) showFirebasePollFailure(e.getMessage()); });
                    } finally {
                        firebasePollInProgress.set(false);
                    }
                });
            } else if (transport == Transport.BLUETOOTH && isBluetoothConnected()
                    && System.currentTimeMillis() - lastBluetoothStateAt >= BLUETOOTH_STATE_STALE_MS) {
                bluetooth.requestState();
            }
            if (pollingActive) {
                long nextPollMs = transport == Transport.FIREBASE ? 1000L : 500L;
                ui.postDelayed(this, nextPollMs);
            }
        }
    };

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        bindViews();
        // Tablets and landscape: keep each screen's content at most content_max_width wide and
        // centered instead of stretching cards and buttons across the whole display.
        findViewById(R.id.screenHost).addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r - l != or - ol) applyContentWidth(r - l);
        });
        setupSystemBarsAndSafeBottomNavigation();

        firebase = new FirebaseRestClient(this);
        firebaseReady = firebase.hasSavedSession();
        BluetoothLinkService.startIfRemembered(this);
        bindService(new Intent(this, BluetoothLinkService.class), bluetoothConnection, Context.BIND_AUTO_CREATE);

        setupNavigation();
        setupTransportControls();
        setupClassroomControls();
        setupUtilityActions();
        updateTransportStatus();
        updateSessionUi();
        setRelaySwitchesEnabled(false);
        txtAppVersion.setText("v" + BuildConfig.VERSION_NAME);

        int restoredScreen = savedInstanceState != null
                ? savedInstanceState.getInt(STATE_SCREEN, SCREEN_DASHBOARD) : SCREEN_DASHBOARD;
        String restoredTransport = savedInstanceState != null
                ? savedInstanceState.getString(STATE_TRANSPORT, Transport.FIREBASE.name()) : Transport.FIREBASE.name();
        if (Transport.BLUETOOTH.name().equals(restoredTransport)) radioBluetooth.setChecked(true);
        else radioFirebase.setChecked(true);
        showScreen(restoredScreen);
    }

    private void applyContentWidth(int hostWidth) {
        int maxWidth = getResources().getDimensionPixelSize(R.dimen.content_max_width);
        int minSide = getResources().getDimensionPixelSize(R.dimen.screen_padding);
        int side = Math.max(minSide, (hostWidth - maxWidth) / 2);
        int[] screens = {R.id.screenDashboard, R.id.screenDevices, R.id.screenEnergy, R.id.screenSettings, R.id.screenAccount};
        for (int id : screens) {
            View content = ((ViewGroup) findViewById(id)).getChildAt(0);
            if (content.getPaddingLeft() != side || content.getPaddingRight() != side) {
                content.setPadding(side, content.getPaddingTop(), side, content.getPaddingBottom());
            }
        }
    }

    private void bindViews() {
        txtHeaderMode = findViewById(R.id.txtHeaderMode);
        txtTransportStatus = findViewById(R.id.txtTransportStatus);
        txtSettingsTransportStatus = findViewById(R.id.txtSettingsTransportStatus);
        txtDeviceStatus = findViewById(R.id.txtDeviceStatus);
        txtConnectivity = findViewById(R.id.txtConnectivity);
        txtWifiSetupStatus = findViewById(R.id.txtWifiSetupStatus);
        txtLastSource = findViewById(R.id.txtLastSource);
        txtDiagnostics = findViewById(R.id.txtDiagnostics);
        txtPeople = findViewById(R.id.txtPeople);
        txtBeams = findViewById(R.id.txtBeams);
        txtOccupancyValidity = findViewById(R.id.txtOccupancyValidity);
        txtSensorQuality = findViewById(R.id.txtSensorQuality);
        txtVoltage = findViewById(R.id.txtVoltage);
        txtCurrent = findViewById(R.id.txtCurrent);
        txtPower = findViewById(R.id.txtPower);
        txtEnergy = findViewById(R.id.txtEnergy);
        txtFrequencyPf = findViewById(R.id.txtFrequencyPf);
        txtEnergyFreshness = findViewById(R.id.txtEnergyFreshness);
        txtMode = findViewById(R.id.txtMode);
        txtMessage = findViewById(R.id.txtMessage);
        txtRelaySummary = findViewById(R.id.txtRelaySummary);
        txtManualHint = findViewById(R.id.txtManualHint);
        txtDashboardPower = findViewById(R.id.txtDashboardPower);
        txtDashboardEnergy = findViewById(R.id.txtDashboardEnergy);
        txtDashboardRelaySummary = findViewById(R.id.txtDashboardRelaySummary);
        txtDashboardAlert = findViewById(R.id.txtDashboardAlert);
        txtDashboardMode = findViewById(R.id.txtDashboardMode);
        txtDashboardUpdated = findViewById(R.id.txtDashboardUpdated);
        txtSystemFirebase = findViewById(R.id.txtSystemFirebase);
        txtSystemBluetooth = findViewById(R.id.txtSystemBluetooth);
        txtAppVersion = findViewById(R.id.txtAppVersion);
        radioFirebase = findViewById(R.id.radioFirebase);
        radioBluetooth = findViewById(R.id.radioBluetooth);
        energyChart = findViewById(R.id.energyChart);
        rootMain = findViewById(R.id.rootMain);
        bottomNavigation = findViewById(R.id.bottomNavigation);

        relayTiles = new View[] {
                findViewById(R.id.tileLight1), findViewById(R.id.tileLight2),
                findViewById(R.id.tileLight3), findViewById(R.id.tileLight4),
                findViewById(R.id.tileFan1), findViewById(R.id.tileFan2)
        };
        relayIcons = new ImageView[] {
                findViewById(R.id.imgLight1), findViewById(R.id.imgLight2),
                findViewById(R.id.imgLight3), findViewById(R.id.imgLight4),
                findViewById(R.id.imgFan1), findViewById(R.id.imgFan2)
        };
        bulbGlows = new View[] {
                findViewById(R.id.glowLight1), findViewById(R.id.glowLight2),
                findViewById(R.id.glowLight3), findViewById(R.id.glowLight4)
        };
        for (int i = 0; i < bulbPulses.length; i++) {
            // The glow breathes in brightness and size together.
            View glowView = bulbGlows[i];
            ValueAnimator pulse = ValueAnimator.ofFloat(0f, 1f);
            pulse.setDuration(1100L);
            pulse.setRepeatCount(ValueAnimator.INFINITE);
            pulse.setRepeatMode(ValueAnimator.REVERSE);
            pulse.setInterpolator(new android.view.animation.AccelerateDecelerateInterpolator());
            pulse.addUpdateListener(a -> {
                float t = (float) a.getAnimatedValue();
                glowView.setAlpha(0.6f + 0.3f * t);
                glowView.setScaleX(0.92f + 0.08f * t);
                glowView.setScaleY(0.92f + 0.08f * t);
            });
            bulbPulses[i] = pulse;
        }
        for (int i = 0; i < relayTiles.length; i++) showRelayState(i, false);

        screens = new View[] {
                findViewById(R.id.screenDashboard), findViewById(R.id.screenDevices),
                findViewById(R.id.screenEnergy), findViewById(R.id.screenSettings),
                findViewById(R.id.screenAccount)
        };
        navItems = new LinearLayout[] {
                findViewById(R.id.navDashboard), findViewById(R.id.navDevices),
                findViewById(R.id.navEnergy), findViewById(R.id.navSettings),
                findViewById(R.id.navAccount)
        };
        navIcons = new ImageView[] {
                findViewById(R.id.imgNavDashboard), findViewById(R.id.imgNavDevices),
                findViewById(R.id.imgNavEnergy), findViewById(R.id.imgNavSettings),
                findViewById(R.id.imgNavAccount)
        };
        navLabels = new TextView[] {
                findViewById(R.id.lblNavDashboard), findViewById(R.id.lblNavDevices),
                findViewById(R.id.lblNavEnergy), findViewById(R.id.lblNavSettings),
                findViewById(R.id.lblNavAccount)
        };
    }

    private void setupSystemBarsAndSafeBottomNavigation() {
        // Android 15 / targetSdk 35 can draw app content edge-to-edge. Keep this app
        // navigation physically above the phone gesture/3-button navigation area.
        getWindow().setNavigationBarColor(Color.WHITE);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            getWindow().getDecorView().setSystemUiVisibility(
                    getWindow().getDecorView().getSystemUiVisibility() | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        }
        rootMain.setOnApplyWindowInsetsListener((v, insets) -> {
            int topInset = insets.getSystemWindowInsetTop();
            int bottomInset = insets.getSystemWindowInsetBottom();
            // Insets keep the app header below the status bar and lift the fixed app
            // bottom navigation above the phone's own navigation/gesture region.
            v.setPadding(v.getPaddingLeft(), topInset, v.getPaddingRight(), bottomInset);
            if (bottomNavigation != null) {
                bottomNavigation.setContentDescription(
                        "App navigation. Positioned above the phone navigation bar. Safe inset " + bottomInset + " pixels.");
            }
            return insets;
        });
        rootMain.requestApplyInsets();
    }

    private void setupNavigation() {
        // Explicit one-to-one mapping keeps every bottom tab on its correct real screen.
        bindNavDestination(R.id.navDashboard, SCREEN_DASHBOARD);
        bindNavDestination(R.id.navDevices, SCREEN_DEVICES);
        bindNavDestination(R.id.navEnergy, SCREEN_ENERGY);
        bindNavDestination(R.id.navSettings, SCREEN_SETTINGS);
        bindNavDestination(R.id.navAccount, SCREEN_ACCOUNT);

        // Auto-size the five labels so all tabs remain visible on narrow phones without scrolling.
        for (TextView label : navLabels) {
            label.setSingleLine(true);
            label.setAutoSizeTextTypeUniformWithConfiguration(8, 11, 1, TypedValue.COMPLEX_UNIT_SP);
        }
    }

    private void bindNavDestination(int viewId, int destination) {
        View nav = findViewById(viewId);
        nav.setOnClickListener(v -> showScreen(destination));
    }

    private void showScreen(int screen) {
        if (screen < SCREEN_DASHBOARD || screen > SCREEN_ACCOUNT) {
            screen = SCREEN_DASHBOARD;
        }
        selectedScreen = screen;
        for (int i = 0; i < screens.length; i++) {
            boolean active = i == screen;
            screens[i].setVisibility(active ? View.VISIBLE : View.GONE);
            navItems[i].setSelected(active);
            navItems[i].setActivated(active);
            navItems[i].setBackgroundResource(active ? R.drawable.bg_nav_item_active : R.drawable.bg_nav_item_touch);
            navIcons[i].setImageResource(active ? navActiveIcons[i] : navInactiveIcons[i]);
            navIcons[i].setAlpha(active ? 1.0f : 0.92f);
            navLabels[i].setTextColor(getColor(active ? R.color.primary : R.color.nav_inactive));
            navLabels[i].setTypeface(android.graphics.Typeface.create(
                    "sans", active ? android.graphics.Typeface.BOLD : android.graphics.Typeface.NORMAL));
            navItems[i].setContentDescription(navLabels[i].getText().toString() + (active ? ", selected" : ", navigation item"));
        }

        // A bottom-navigation tap opens the destination from the top so the selected screen is immediately obvious.
        View selected = screens[screen];
        if (selected instanceof android.widget.ScrollView) {
            selected.post(() -> ((android.widget.ScrollView) selected).smoothScrollTo(0, 0));
        }

        if (screen == SCREEN_ACCOUNT) updateSessionUi();
    }

    private void setupTransportControls() {
        radioFirebase.setOnCheckedChangeListener((button, checked) -> {
            if (checked) {
                transport = Transport.FIREBASE;
                setRelaySwitchesEnabled(false);
                txtHeaderMode.setText(firebaseReady ? "SYNCING" : "NO AUTH");
                updateTransportStatus();
                txtMessage.setText("Firebase mode selected. Live RTDB stream will show sensor changes immediately.");
                if (firebaseReady) {
                    startFirebaseRealtimeStream();
                    requestCurrentState();
                }
            }
        });
        radioBluetooth.setOnCheckedChangeListener((button, checked) -> {
            if (checked) {
                stopFirebaseRealtimeStream();
                transport = Transport.BLUETOOTH;
                setRelaySwitchesEnabled(false);
                updateTransportStatus();
                if (isBluetoothConnected()) {
                    txtHeaderMode.setText("SYNCING");
                    bluetooth.requestState();
                } else {
                    txtHeaderMode.setText("BT OFF");
                    txtDeviceStatus.setText("Not connected");
                }
                txtMessage.setText("Bluetooth local mode selected. Internet is not required.");
            }
        });

        // setOnClickListener, unlike the change listener above, runs only for a user tap, not when
        // the saved selection is restored at start-up.
        radioBluetooth.setOnClickListener(v -> {
            if (firebaseReady && !isBluetoothConnected()) requestControllerBluetooth();
        });

        findViewById(R.id.btnFirebaseLogin).setOnClickListener(v -> showFirebaseLogin());
        findViewById(R.id.btnFirebaseLogout).setOnClickListener(v -> {
            stopFirebaseRealtimeStream();
            firebase.clearSession();
            firebaseReady = false;
            if (transport == Transport.FIREBASE) {
                setRelaySwitchesEnabled(false);
                txtHeaderMode.setText("NO AUTH");
                txtDeviceStatus.setText("Signed out");
            }
            updateTransportStatus();
            updateSessionUi();
            txtMessage.setText("Firebase session signed out from this phone.");
        });
        findViewById(R.id.btnBluetoothConnect).setOnClickListener(v -> chooseBluetoothDevice());
        findViewById(R.id.btnBluetoothDisconnect).setOnClickListener(v -> {
            if (bluetooth != null) bluetooth.disconnectByUser();
            setRelaySwitchesEnabled(false);
            txtHeaderMode.setText("BT OFF");
            txtDeviceStatus.setText("Disconnected");
            txtMessage.setText("Bluetooth disconnect requested.");
            if (transport == Transport.BLUETOOTH && firebaseReady) {
                radioFirebase.setChecked(true);
                txtMessage.setText("Bluetooth disconnected. Back to the cloud: the controller reconnects to Firebase in about a minute.");
            }
            ui.postDelayed(() -> {
                updateTransportStatus();
                updateSessionUi();
            }, 250L);
        });
        findViewById(R.id.btnBluetoothSettings).setOnClickListener(v -> {
            try { startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)); }
            catch (Exception e) { startActivity(new Intent(Settings.ACTION_SETTINGS)); }
        });
    }

    private void setupClassroomControls() {
        findViewById(R.id.btnAuto).setOnClickListener(v -> sendMode(true));
        findViewById(R.id.btnManual).setOnClickListener(v -> sendMode(false));
        findViewById(R.id.btnAllOff).setOnClickListener(v -> confirmAllOff());
        findViewById(R.id.btnDashboardAuto).setOnClickListener(v -> sendMode(true));
        findViewById(R.id.btnDashboardManual).setOnClickListener(v -> sendMode(false));
        findViewById(R.id.btnDashboardAllOff).setOnClickListener(v -> confirmAllOff());

        findViewById(R.id.btnCorrectCount).setOnClickListener(v -> showCountDialog());
        findViewById(R.id.btnCalibrateIr).setOnClickListener(v -> confirmCalibrateIr());
        findViewById(R.id.btnResetEnergy).setOnClickListener(v -> confirmResetEnergy());
        findViewById(R.id.btnWifiSetup).setOnClickListener(v -> confirmWifiSetup());

        for (int i = 0; i < relayTiles.length; i++) {
            final int index = i;
            relayTiles[i].setOnClickListener(v -> onRelayTapped(index));
        }
    }

    private void setupUtilityActions() {
        findViewById(R.id.btnDashboardRefresh).setOnClickListener(v -> requestCurrentState());
        findViewById(R.id.btnSystemRefresh).setOnClickListener(v -> requestCurrentState());
        findViewById(R.id.btnCopyDiagnostics).setOnClickListener(v -> copyText("ESAVE Diagnostics", buildDiagnosticsText()));
        findViewById(R.id.btnOpenAppSettings).setOnClickListener(v -> {
            Intent i = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + getPackageName()));
            try { startActivity(i); }
            catch (Exception e) { toast("Unable to open Android app settings"); }
        });
    }

    private void confirmAllOff() {
        new AlertDialog.Builder(this)
                .setTitle("Turn everything OFF?")
                .setMessage("This sends the real ALL OFF command to the selected transport. The controller should enter MANUAL mode so AUTO does not immediately restore loads.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("ALL OFF", (d, w) -> sendAllOff())
                .show();
    }

    private void confirmCalibrateIr() {
        new AlertDialog.Builder(this)
                .setTitle("Calibrate IR sensors")
                .setMessage("Make sure BOTH Entrance and Exit beams are CLEAR. The ESP32 will save the present raw levels as the normal clear-beam state.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Calibrate", (d, w) -> sendCalibrateIr())
                .show();
    }

    private void confirmResetEnergy() {
        new AlertDialog.Builder(this)
                .setTitle("Reset PZEM energy counter?")
                .setMessage("Only the accumulated kWh counter is reset. Voltage, current and power readings continue normally.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Reset kWh", (d, w) -> sendResetEnergy())
                .show();
    }

    /**
     * Asks the controller (through Firebase) to stop its cloud connection and turn Bluetooth on,
     * then connects to it. Bluetooth and Firebase do not fit in the controller's memory together.
     */
    private void requestControllerBluetooth() {
        txtMessage.setText("Asking the controller to switch to Bluetooth...");
        commands.execute(() -> {
            String problem = null;
            try {
                FirebaseRestClient.FirebaseAck ack = firebase.waitForAck(firebase.bluetoothSession());
                if (!ack.ok) problem = ack.detail;
            } catch (Exception e) {
                problem = safeMessage(e);
            }
            String finalProblem = problem;
            ui.post(() -> {
                if (transport != Transport.BLUETOOTH) return;
                // Even without an ACK the controller may already be on Bluetooth (it is offline),
                // so try to connect either way.
                txtMessage.setText(finalProblem == null
                        ? "Controller switched to Bluetooth • connecting..."
                        : "Controller did not confirm (" + finalProblem + ") • trying Bluetooth anyway...");
                // The controller starts Bluetooth a moment after its ACK, once Firebase is stopped.
                if (bluetooth == null || !bluetooth.reconnectSoon(3000L)) chooseBluetoothDevice();
            });
        });
    }

    /** Must match SETUP_AP_SSID in firmware/include/config.h. */
    static final String WIFI_SETUP_AP_SSID = "ESAVE-Room206-Setup";

    private void confirmWifiSetup() {
        // The firmware accepts wifi_setup only over Bluetooth: joining the AP needs you in the room anyway.
        if (transport != Transport.BLUETOOTH || !isBluetoothConnected()) {
            toast("Wi-Fi setup needs a Bluetooth connection to the controller");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Open controller Wi-Fi setup?")
                .setMessage("The ESP32 opens the Wi-Fi network \"" + WIFI_SETUP_AP_SSID + "\".\n\n"
                        + "1. Join it with the setup password from the installer.\n"
                        + "2. The setup page opens, or browse to http://192.168.4.1/\n"
                        + "3. Choose the classroom Wi-Fi and enter its password.\n\n"
                        + "Lights, fans and Bluetooth keep working during setup.")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Open setup", (d, w) -> {
                    bluetooth.startWifiSetup();
                    txtMessage.setText("Wi-Fi setup request sent by Bluetooth...");
                })
                .show();
    }

    static String wifiSetupStatusText(SmartClassroomState s) {
        if (!s.wifiSetupSupported) return "Needs firmware 25.18+";
        String status;
        if (!s.wifiConfigured) status = "No network saved";
        else if (s.wifi) status = s.wifiSsid + "  ·  " + s.ip;
        else status = s.wifiSsid + "  ·  not connected";
        if (s.wifiSetupAp) status += "\nSetup: join " + WIFI_SETUP_AP_SSID + " → 192.168.4.1";
        return status;
    }

    /** Material 3 style login sheet (layout/dialog_firebase_login.xml) with a show-password toggle. */
    private void showFirebaseLogin() {
        View view = getLayoutInflater().inflate(R.layout.dialog_firebase_login, null);
        EditText email = view.findViewById(R.id.loginEmail);
        EditText password = view.findViewById(R.id.loginPassword);
        View emailField = view.findViewById(R.id.loginEmailField);
        View passwordField = view.findViewById(R.id.loginPasswordField);
        ImageButton toggle = view.findViewById(R.id.btnTogglePassword);
        TextView error = view.findViewById(R.id.loginError);
        TextView login = view.findViewById(R.id.btnLogin);

        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setContentView(view);
        Window window = dialog.getWindow();
        if (window != null) {
            // Transparent window so the sheet's own rounded corners show; at most 460dp wide.
            int margin = dp(20);
            window.setBackgroundDrawable(new InsetDrawable(new ColorDrawable(Color.TRANSPARENT), margin));
            int width = Math.min(getResources().getDisplayMetrics().widthPixels, dp(460) + 2 * margin);
            window.setLayout(width, WindowManager.LayoutParams.WRAP_CONTENT);
            window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                    | WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE);
        }

        toggle.setOnClickListener(v -> {
            boolean show = password.getTransformationMethod() instanceof PasswordTransformationMethod;
            int cursor = password.getSelectionEnd();
            password.setTransformationMethod(show ? HideReturnsTransformationMethod.getInstance()
                    : PasswordTransformationMethod.getInstance());
            password.setSelection(Math.max(0, cursor));
            toggle.setImageResource(show ? R.drawable.ic_visibility_off : R.drawable.ic_visibility);
            toggle.setContentDescription(getString(show ? R.string.hide_password : R.string.show_password));
        });

        Runnable submit = () -> {
            if (!login.isEnabled()) return;
            String e = email.getText().toString().trim();
            String pw = password.getText().toString();
            emailField.setActivated(false);
            passwordField.setActivated(false);
            if (!Patterns.EMAIL_ADDRESS.matcher(e).matches()) {
                showLoginError(error, emailField, "Enter a valid email address.");
                return;
            }
            if (pw.isEmpty()) {
                showLoginError(error, passwordField, "Enter your password.");
                return;
            }
            error.setVisibility(View.GONE);
            setLoginBusy(view, true);
            setTransportText("Signing in…");
            network.execute(() -> {
                try {
                    firebase.signIn(e, pw);
                    firebaseReady = true;
                    ui.post(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        if (dialog.isShowing()) dialog.dismiss();
                        radioFirebase.setChecked(true);
                        setTransportText("Signed in · waiting…");
                        txtMessage.setText("Firebase login successful.");
                        updateSessionUi();
                        startFirebaseRealtimeStream();
                        requestCurrentState();
                    });
                } catch (Exception ex) {
                    ui.post(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        setTransportText("Login failed: " + safeMessage(ex));
                        txtMessage.setText("Firebase login failed. Check account, internet connection, and Firebase Authentication settings.");
                        if (!dialog.isShowing()) return;
                        setLoginBusy(view, false);
                        String message = friendlyLoginError(ex);
                        boolean credentials = message.startsWith("Wrong");
                        showLoginError(error, credentials ? passwordField : null, message);
                    });
                }
            });
        };
        login.setOnClickListener(v -> submit.run());
        password.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId != EditorInfo.IME_ACTION_DONE) return false;
            submit.run();
            return true;
        });
        view.findViewById(R.id.btnLoginCancel).setOnClickListener(v -> dialog.dismiss());
        dialog.show();
        email.requestFocus();
    }

    private static void showLoginError(TextView error, View field, String message) {
        error.setText(message);
        error.setVisibility(View.VISIBLE);
        if (field != null) field.setActivated(true);
    }

    private void setLoginBusy(View sheet, boolean busy) {
        TextView login = sheet.findViewById(R.id.btnLogin);
        login.setEnabled(!busy);
        login.setText(busy ? R.string.login_signing_in : R.string.firebase_login);
        sheet.findViewById(R.id.loginEmail).setEnabled(!busy);
        sheet.findViewById(R.id.loginPassword).setEnabled(!busy);
    }

    /** Firebase Auth error codes (e.g. INVALID_LOGIN_CREDENTIALS) as plain sentences. */
    private static String friendlyLoginError(Exception ex) {
        if (ex instanceof java.net.UnknownHostException || ex instanceof java.net.SocketTimeoutException
                || ex instanceof java.net.ConnectException) {
            return "No internet connection. Check Wi-Fi or mobile data and try again.";
        }
        String m = safeMessage(ex).toUpperCase(Locale.ROOT);
        if (m.contains("INVALID_LOGIN_CREDENTIALS") || m.contains("INVALID_PASSWORD") || m.contains("EMAIL_NOT_FOUND")) {
            return "Wrong email or password.";
        }
        if (m.contains("TOO_MANY_ATTEMPTS")) return "Too many attempts. Wait a few minutes and try again.";
        if (m.contains("USER_DISABLED")) return "This account has been disabled.";
        if (m.contains("INVALID_EMAIL")) return "Enter a valid email address.";
        return "Login failed: " + safeMessage(ex);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private boolean isBluetoothConnected() {
        return bluetooth != null && bluetooth.isConnected();
    }

    private boolean hasBtPermission() {
        return Build.VERSION.SDK_INT < 31 || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }

    // BLUETOOTH_CONNECT is checked via hasBtPermission() before any adapter call below.
    @SuppressLint("MissingPermission")
    private void chooseBluetoothDevice() {
        BluetoothAdapter a = bluetooth != null ? bluetooth.adapter() : BluetoothAdapter.getDefaultAdapter();
        if (a == null) { toast("This phone does not support Bluetooth"); return; }
        if (Build.VERSION.SDK_INT >= 31 && !hasBtPermission()) {
            requestPermissions(new String[]{Manifest.permission.BLUETOOTH_CONNECT}, REQ_BLUETOOTH_CONNECT);
            return;
        }
        // Android 13+ hides the persistent-link notification (and its Disconnect action) unless
        // notifications are allowed. Ask once; a denial does not block connecting.
        if (Build.VERSION.SDK_INT >= 33 && !notificationPermissionAsked
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            notificationPermissionAsked = true;
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_POST_NOTIFICATIONS);
            return;
        }
        if (!a.isEnabled()) {
            try { startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_ENABLE_BLUETOOTH); }
            catch (Exception e) { toast("Turn on Bluetooth in Settings"); }
            return;
        }
        List<BluetoothDevice> devices;
        try { if (bluetooth == null) { toast("Bluetooth service is starting. Try again in a moment."); return; } devices = bluetooth.pairedDevices(); }
        catch (SecurityException e) { toast("Nearby devices permission required"); return; }
        if (devices.isEmpty()) {
            new AlertDialog.Builder(this).setTitle("No paired devices")
                    .setMessage("Pair the ESP32 named " + BuildConfig.BLUETOOTH_DEVICE_NAME + " in Android Bluetooth Settings first. " + pairingPinHint() + ".")
                    .setPositiveButton("Open Settings", (d,w) -> startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)))
                    .setNegativeButton("Cancel", null).show();
            return;
        }
        String[] names = new String[devices.size()];
        int preferred = -1;
        for (int i = 0; i < devices.size(); i++) {
            BluetoothDevice dev = devices.get(i);
            String name;
            try { name = dev.getName(); } catch (SecurityException e) { name = "Bluetooth device"; }
            if (name == null) name = "Unknown";
            names[i] = name + "\n" + dev.getAddress();
            if (BuildConfig.BLUETOOTH_DEVICE_NAME.equals(name)) preferred = i;
        }
        final int recommended = preferred;
        AlertDialog dlg = new AlertDialog.Builder(this)
                .setTitle("Select paired ESP32")
                .setSingleChoiceItems(names, preferred, null)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Connect", null)
                .create();
        dlg.setOnShowListener(x -> dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            int which = dlg.getListView().getCheckedItemPosition();
            if (which < 0 && recommended >= 0) which = recommended;
            if (which < 0) { toast("Select a device"); return; }
            BluetoothDevice dev = devices.get(which);
            dlg.dismiss();
            radioBluetooth.setChecked(true);
            BluetoothLinkService.startPersistent(this);
            bluetooth.connect(dev);
        }));
        dlg.show();
    }

    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_BLUETOOTH_CONNECT) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                toast("Nearby devices permission granted");
                chooseBluetoothDevice();
            } else toast("Bluetooth connection permission was denied");
        } else if (requestCode == REQ_POST_NOTIFICATIONS) {
            if (grantResults.length == 0 || grantResults[0] != PackageManager.PERMISSION_GRANTED) {
                toast("Notifications off: the background Bluetooth link will run without a visible notice");
            }
            chooseBluetoothDevice();
        }
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ENABLE_BLUETOOTH) {
            BluetoothAdapter adapter = bluetooth != null ? bluetooth.adapter() : BluetoothAdapter.getDefaultAdapter();
            if (adapter != null && adapter.isEnabled()) chooseBluetoothDevice();
            else toast("Bluetooth is still turned off");
        }
    }

    private interface FirebaseAction { long run() throws Exception; }

    private void runFirebaseAction(String label, FirebaseAction action) {
        if (!firebaseReady) { toast("Login to Firebase first"); showScreen(SCREEN_SETTINGS); return; }
        txtMessage.setText(label + " queued • waiting for ESP32 confirmation...");
        commands.execute(() -> {
            try {
                long commandId = action.run();
                FirebaseRestClient.FirebaseAck ack = null;
                String ackProblem = "";
                try {
                    ack = firebase.waitForAck(commandId);
                } catch (Exception ackError) {
                    ackProblem = safeMessage(ackError);
                }

                // Always read the controller's state after a command. Even if RTDB rules do not
                // expose /acks/last, the UI still reflects the real ESP32-reported state instead
                // of assuming that a write to /commands succeeded physically.
                SmartClassroomState confirmedState = firebase.getState();
                FirebaseRestClient.FirebaseAck finalAck = ack;
                String finalAckProblem = ackProblem;
                ui.post(() -> {
                    if (transport == Transport.FIREBASE && firebaseReady && pollingActive) {
                        renderState(confirmedState, true);
                    }
                    if (finalAck != null && finalAck.ok) {
                        txtMessage.setText("ESP32 CONFIRMED • " + safe(finalAck.action) +
                                (finalAck.detail.isEmpty() ? "" : " • " + finalAck.detail));
                    } else if (finalAck != null) {
                        // Firmware V25.13+ rejects commands older than 2 minutes by NTP time.
                        String hint = "COMMAND_EXPIRED".equals(finalAck.detail)
                                ? " (command arrived too late; check the phone's date/time is automatic)" : "";
                        txtMessage.setText("ESP32 REJECTED • " + safe(finalAck.action) +
                                (finalAck.detail.isEmpty() ? "" : " • " + finalAck.detail) + hint);
                    } else {
                        txtMessage.setText("ESP32 state refreshed; command ACK unavailable • " + safe(finalAckProblem));
                    }
                });
            } catch (Exception e) {
                ui.post(() -> {
                    if (isSessionError(e.getMessage())) {
                        firebaseReady = false;
                        if (transport == Transport.FIREBASE) {
                            setRelaySwitchesEnabled(false);
                            txtHeaderMode.setText("NO AUTH");
                        }
                        updateTransportStatus();
                    }
                    txtMessage.setText(label + " not confirmed: " + safeMessage(e));
                });
            }
        });
    }

    private void sendMode(boolean automatic) {
        if (transport == Transport.FIREBASE) runFirebaseAction("Mode command", () -> firebase.setMode(automatic));
        else if (isBluetoothConnected()) {
            bluetooth.setMode(automatic);
            txtMessage.setText("Mode command sent by Bluetooth; waiting for ESP32 ACK/state...");
        } else { toast("Connect Bluetooth first"); showScreen(SCREEN_SETTINGS); }
    }

    private void sendRelay(String device, boolean state) {
        if (transport == Transport.FIREBASE) runFirebaseAction(device + " command", () -> firebase.setRelay(device, state));
        else if (isBluetoothConnected()) {
            bluetooth.setRelay(device, state);
            txtMessage.setText(device + " command sent by Bluetooth...");
        } else { toast("Connect Bluetooth first"); showScreen(SCREEN_SETTINGS); }
    }

    private void sendAllOff() {
        if (transport == Transport.FIREBASE) runFirebaseAction("ALL OFF", firebase::allOff);
        else if (isBluetoothConnected()) {
            bluetooth.allOff();
            txtMessage.setText("ALL OFF sent by Bluetooth...");
        } else { toast("Connect Bluetooth first"); showScreen(SCREEN_SETTINGS); }
    }

    private void showCountDialog() {
        EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_NUMBER);
        input.setHint("Actual people inside");
        if (currentState != null) input.setText(String.valueOf(currentState.people));

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Correct occupancy count")
                .setMessage("Use this after controller reboot or when the physical count no longer matches the sensor count.")
                .setView(input)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Set", null)
                .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            try {
                String raw = input.getText().toString().trim();
                if (raw.isEmpty()) throw new NumberFormatException();
                int count = Integer.parseInt(raw);
                if (count < 0 || count > 999) throw new NumberFormatException();
                if (transport == Transport.FIREBASE) {
                    if (!firebaseReady) {
                        toast("Login to Firebase first");
                        showScreen(SCREEN_SETTINGS);
                        return;
                    }
                    runFirebaseAction("Count correction", () -> firebase.setCount(count));
                } else if (isBluetoothConnected()) {
                    bluetooth.setCount(count);
                    txtMessage.setText("Count correction sent by Bluetooth...");
                } else {
                    toast("Connect Bluetooth first");
                    showScreen(SCREEN_SETTINGS);
                    return;
                }
                dialog.dismiss();
            } catch (NumberFormatException e) {
                toast("Enter a number from 0 to 999");
            }
        }));
        dialog.show();
    }

    /** Oldest firmware this app is written for (per-command ACKs, verified TLS). */
    static final int MIN_FIRMWARE_MAJOR = 25, MIN_FIRMWARE_MINOR = 16;

    private void sendCalibrateIr() {
        if (transport == Transport.FIREBASE) runFirebaseAction("IR calibration", firebase::calibrateIr);
        else if (isBluetoothConnected()) { bluetooth.calibrateIr(); txtMessage.setText("IR calibration sent by Bluetooth..."); }
        else { toast("Connect Bluetooth first"); showScreen(SCREEN_SETTINGS); }
    }

    private void sendResetEnergy() {
        if (transport == Transport.FIREBASE) runFirebaseAction("PZEM reset", firebase::resetEnergy);
        else if (isBluetoothConnected()) { bluetooth.resetEnergy(); txtMessage.setText("PZEM reset sent by Bluetooth..."); }
        else { toast("Connect Bluetooth first"); showScreen(SCREEN_SETTINGS); }
    }

    private void requestCurrentState() {
        if (transport == Transport.FIREBASE) {
            if (!firebaseReady) {
                toast("Login to Firebase first");
                showScreen(SCREEN_SETTINGS);
                return;
            }
            if (!firebasePollInProgress.compareAndSet(false, true)) {
                txtMessage.setText("A Firebase refresh is already in progress...");
                return;
            }
            txtMessage.setText("Refreshing controller state from Firebase...");
            network.execute(() -> {
                try {
                    SmartClassroomState state = firebase.getState();
                    ui.post(() -> {
                        if (transport == Transport.FIREBASE && firebaseReady && pollingActive) {
                            renderState(state, true);
                            txtMessage.setText("Live Firebase state refreshed.");
                        }
                    });
                } catch (Exception e) {
                    ui.post(() -> { if (transport == Transport.FIREBASE && firebaseReady && pollingActive) showFirebasePollFailure(e.getMessage()); });
                } finally {
                    firebasePollInProgress.set(false);
                }
            });
        } else if (isBluetoothConnected()) {
            txtMessage.setText("Requesting live state from ESP32 Bluetooth...");
            bluetooth.requestState();
        } else {
            toast("Connect Bluetooth first");
            showScreen(SCREEN_SETTINGS);
        }
    }

    private void startFirebaseRealtimeStream() {
        if (!pollingActive || transport != Transport.FIREBASE || !firebaseReady) return;
        if (!firebaseStreamTaskActive.compareAndSet(false, true)) return;
        firebaseStreamStop.set(false);
        network.execute(() -> {
            long retryMs = 1000L;
            while (!firebaseStreamStop.get() && firebaseReady && pollingActive && transport == Transport.FIREBASE) {
                try {
                    firebase.streamState(firebaseStreamStop, new FirebaseRestClient.StateStreamListener() {
                        @Override public void onOpen() {
                            firebaseStreamRunning.set(true);
                            ui.post(() -> {
                                if (transport == Transport.FIREBASE && firebaseReady && pollingActive) {
                                    setTransportText("Cloud · live");
                                    txtMessage.setText("Realtime Firebase stream connected. Sensor updates are live.");
                                }
                            });
                        }

                        @Override public void onState(SmartClassroomState state) {
                            ui.post(() -> {
                                if (transport == Transport.FIREBASE && firebaseReady && pollingActive) {
                                    renderState(state, true);
                                }
                            });
                        }
                    });
                    retryMs = 1000L;
                } catch (Exception e) {
                    firebaseStreamRunning.set(false);
                    if (firebaseStreamStop.get()) break;
                    String problem = safeMessage(e);
                    ui.post(() -> {
                        if (transport == Transport.FIREBASE && firebaseReady && pollingActive) {
                            setTransportText("Cloud · reconnecting");
                            txtMessage.setText("Realtime stream reconnecting: " + problem);
                        }
                    });
                    try { Thread.sleep(retryMs); }
                    catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); break; }
                    retryMs = Math.min(8000L, retryMs * 2L);
                } finally {
                    firebaseStreamRunning.set(false);
                }
            }
            firebaseStreamRunning.set(false);
            firebaseStreamTaskActive.set(false);
        });
    }

    private void stopFirebaseRealtimeStream() {
        firebaseStreamStop.set(true);
        firebaseStreamRunning.set(false);
        if (firebase != null) firebase.closeStateStream();
    }

    private void showFirebasePollFailure(String message) {
        if (isSessionError(message)) firebaseReady = false;
        setRelaySwitchesEnabled(false);
        setTransportText("Cloud · " + safe(message));
        txtDeviceStatus.setText("No data");
        txtHeaderMode.setText("NO DATA");
        txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_warning);
        txtDashboardAlert.setText("Refresh failed: " + safe(message));
        txtMessage.setText("State refresh failed: " + safe(message));
        updateSessionUi();
    }

    private void renderState(SmartClassroomState s, boolean fromFirebase) {
        if (s == null) return;
        if (fromFirebase && (transport != Transport.FIREBASE || !firebaseReady)) return;
        if (!fromFirebase && transport != Transport.BLUETOOTH) return;
        currentState = s;
        currentStateTransport = fromFirebase ? Transport.FIREBASE : Transport.BLUETOOTH;
        long ageSec = s.serverTimestampMs > 0 ? Math.max(0L, (System.currentTimeMillis() - s.serverTimestampMs) / 1000L) : -1L;
        boolean controllerOnline = !fromFirebase || (ageSec >= 0 && ageSec <= 8);
        long effectivePzemAgeSec = s.pzemLastGoodAgeSeconds < 0 ? -1L
                : s.pzemLastGoodAgeSeconds + (fromFirebase && ageSec > 0 ? ageSec : 0L);
        boolean energyFreshForUi = controllerOnline && s.energyValid
                && effectivePzemAgeSec >= 0L && effectivePzemAgeSec <= 6L;

        if (fromFirebase) {
            setTransportText((firebaseStreamRunning.get() ? "Cloud · live" : "Cloud · polling")
                    + (ageSec >= 0 ? " · " + ageSec + "s" : ""));
            txtDeviceStatus.setText(controllerOnline ? "Online"
                    : (s.cloudPaused && s.wifi ? "Local Bluetooth session" : "Offline"));
        } else {
            setTransportText(isBluetoothConnected() ? "Bluetooth · direct" : "Bluetooth · disconnected");
            txtDeviceStatus.setText("Online");
        }

        txtHeaderMode.setText(controllerOnline ? s.mode.toUpperCase(Locale.US) : "OFFLINE");
        txtConnectivity.setText("Wi-Fi " + yesNo(s.wifi) + "  ·  Cloud " + yesNo(s.firebase) + "  ·  BT " + yesNo(s.bluetooth)
                + (s.wifi ? "  ·  " + s.rssi + " dBm" : ""));
        txtLastSource.setText("Last: " + safe(s.lastSource));
        txtWifiSetupStatus.setText(wifiSetupStatusText(s));
        // Full counters and detector details are in COPY DIAGNOSTICS (buildDiagnosticsText).
        txtDiagnostics.setText("Up " + formatUptime(s.uptimeMs) + "  ·  Heap " + (s.freeHeap / 1024) + " KB  ·  FW " + safe(s.schemaVersion));

        txtPeople.setText(String.valueOf(s.people));
        txtBeams.setText("Entrance " + (s.entranceBroken ? "blocked" : "clear") + "  ·  Exit " + (s.exitBroken ? "blocked" : "clear"));
        txtOccupancyValidity.setText(s.occupancyValid
                ? "Count OK  ·  in " + s.entranceEvents + "  ·  out " + s.exitEvents
                : "Count needs correction");
        if (txtSensorQuality != null) {
            txtSensorQuality.setText(s.noiseGuardActive
                    ? "Noise guard on  ·  " + (s.entranceSensorNoisy ? "entrance" : "") + (s.entranceSensorNoisy && s.exitSensorNoisy ? " + " : "") + (s.exitSensorNoisy ? "exit" : "")
                    : "Sensors stable");
        }

        if (energyFreshForUi) {
            txtVoltage.setText(String.format(Locale.US, "%.1f V", s.voltage));
            txtCurrent.setText(String.format(Locale.US, "%.3f A", s.current));
            txtPower.setText(String.format(Locale.US, "%.1f W", s.power));
            txtEnergy.setText(String.format(Locale.US, "%.3f kWh", s.energy));
            String hz = Double.isFinite(s.frequency) ? String.format(Locale.US, "%.1f Hz", s.frequency) : "-- Hz";
            String pf = Double.isFinite(s.powerFactor) ? String.format(Locale.US, "%.2f", s.powerFactor) : "--";
            txtFrequencyPf.setText(hz + "  ·  PF " + pf);
            txtEnergyFreshness.setText("Live · " + effectivePzemAgeSec + "s ago");
            txtDashboardPower.setText(String.format(Locale.US, "%.1f W", s.power));
            txtDashboardEnergy.setText(String.format(Locale.US, "%.3f kWh", s.energy));
            long chartSampleKey = s.pzemSampleSequence > 0 ? s.pzemSampleSequence : s.serverTimestampMs;
            if (chartSampleKey <= 0 || chartSampleKey != lastChartTimestamp) {
                energyChart.addSample(s.power);
                lastChartTimestamp = chartSampleKey;
            }
        } else {
            txtVoltage.setText("-- V");
            txtCurrent.setText("-- A");
            txtPower.setText("-- W");
            txtEnergy.setText("-- kWh");
            txtFrequencyPf.setText("Check meter power and wiring");
            txtEnergyFreshness.setText("No meter data");
            txtDashboardPower.setText("-- W");
            txtDashboardEnergy.setText("-- kWh");
        }

        txtMode.setText(s.mode);
        txtDashboardMode.setText(s.mode);
        txtDashboardUpdated.setText(ageSec >= 0 && fromFirebase && !firebaseStreamRunning.get() ? ageSec + "s ago" : "Live");

        boolean manual = "MANUAL".equalsIgnoreCase(s.mode);
        // The controller's current mode glows (bg_button_mode); neither does while it is offline.
        boolean autoSelected = controllerOnline && "AUTO".equalsIgnoreCase(s.mode);
        boolean manualSelected = controllerOnline && manual;
        findViewById(R.id.btnAuto).setSelected(autoSelected);
        findViewById(R.id.btnDashboardAuto).setSelected(autoSelected);
        findViewById(R.id.btnManual).setSelected(manualSelected);
        findViewById(R.id.btnDashboardManual).setSelected(manualSelected);
        boolean controlLinkReady = controllerOnline && currentStateTransport == transport &&
                (fromFirebase ? firebaseReady : (isBluetoothConnected()));
        int activeRelays = 0;
        int lightsOn = 0;
        int fansOn = 0;
        int gpioVerified = 0;
        for (int i = 0; i < relayTiles.length; i++) {
            showRelayState(i, s.relays[i]);
            // Full brightness in AUTO and MANUAL; dimmed only without a live link. In AUTO a tap
            // is still accepted so onRelayTapped() can explain that MANUAL is needed.
            relayTiles[i].setEnabled(controlLinkReady);
            relayTiles[i].setAlpha(controlLinkReady ? 1f : 0.55f);
            if (s.relays[i]) {
                activeRelays++;
                if (i < 4) lightsOn++;
                else if (i < 6) fansOn++;
            }
            if (s.relayVerificationAvailable && s.relayGpioVerified[i]) gpioVerified++;
        }
        txtRelaySummary.setText(activeRelays + " of 6 on"
                + (s.relayVerificationAvailable && gpioVerified < relayTiles.length ? "  ·  GPIO check failed" : ""));
        // ALL OFF glows while every relay is off; neither it nor AUTO/MANUAL glows while offline.
        boolean allRelaysOff = controllerOnline && activeRelays == 0;
        findViewById(R.id.btnAllOff).setSelected(allRelaysOff);
        findViewById(R.id.btnDashboardAllOff).setSelected(allRelaysOff);
        txtDashboardRelaySummary.setText("Lights " + lightsOn + "/4\nFans " + fansOn + "/2");
        if (txtManualHint != null) {
            txtManualHint.setText(manual ? "Switches unlocked" : "Switches follow occupancy");
        }

        boolean relayGpioFailure = s.relayVerificationAvailable && gpioVerified < relayTiles.length;
        if (!controllerOnline) {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_error);
            txtDashboardAlert.setText("Controller offline · check power or Wi-Fi");
        } else if (relayGpioFailure) {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_error);
            txtDashboardAlert.setText("Relay check failed · inspect relay wiring");
        } else if (s.noiseGuardActive) {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_error);
            txtDashboardAlert.setText("IR noise · counting paused");
        } else if (!s.occupancyValid) {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_warning);
            txtDashboardAlert.setText("Count needs correction · Settings");
        } else if (!s.schemaAtLeast(MIN_FIRMWARE_MAJOR, MIN_FIRMWARE_MINOR)) {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_warning);
            txtDashboardAlert.setText("Update firmware to " + MIN_FIRMWARE_MAJOR + "." + MIN_FIRMWARE_MINOR + "+");
        } else if (!energyFreshForUi) {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_warning);
            txtDashboardAlert.setText("No meter data");
        } else {
            txtDashboardAlert.setBackgroundResource(R.drawable.bg_notice_success);
            txtDashboardAlert.setText("All systems normal");
        }

        updateSessionUi();
    }

    private void setRelaySwitchesEnabled(boolean enabled) {
        for (View tile : relayTiles) {
            tile.setEnabled(enabled);
            tile.setAlpha(enabled ? 1f : 0.55f);
        }
    }

    /** Tap on a bulb/fan: request the opposite of the ESP32-confirmed state (MANUAL only). */
    private void onRelayTapped(int index) {
        boolean liveSelectedState = currentState != null && currentStateTransport == transport;
        boolean manualMode = liveSelectedState && "MANUAL".equalsIgnoreCase(currentState.mode);
        if (!liveSelectedState || !manualMode) {
            toast(!liveSelectedState ? "Wait for a live state from the selected connection"
                    : "CH1-CH6 are controlled by AUTO. Tap MANUAL first.");
            return;
        }
        // No optimistic change: the icon updates only when the ESP32 reports the new state.
        sendRelay(deviceKeys[index], !currentState.relays[index]);
    }

    /** Bulb: hollow when OFF, yellow with a pulsing glow when ON. Fan: blue always; still when OFF, spinning when ON. */
    private void showRelayState(int index, boolean on) {
        ImageView icon = relayIcons[index];
        String name = index < 4 ? "Light " + (index + 1) : "Fan " + (index - 3);
        relayTiles[index].setContentDescription(name + (on ? ", on" : ", off"));
        if (index < 4) {
            icon.setImageResource(on ? R.drawable.ic_bulb_on : R.drawable.ic_bulb_off);
            View glowView = bulbGlows[index];
            glowView.setActivated(on);
            ValueAnimator pulse = bulbPulses[index];
            if (on && !pulse.isStarted()) {
                pulse.start();
            } else if (!on && pulse.isStarted()) {
                pulse.cancel();
                glowView.setAlpha(1f);
                glowView.setScaleX(1f);
                glowView.setScaleY(1f);
            }
            return;
        }
        setFanSpinning(index - 4, on);
    }

    private void setFanSpinning(int fan, boolean on) {
        if (fanOn[fan] == on) return;
        fanOn[fan] = on;
        ImageView icon = relayIcons[4 + fan];
        if (fanSpins[fan] != null) fanSpins[fan].cancel();
        float from = icon.getRotation() % 360f;
        icon.setRotation(from);
        // Hardware layer: the vector is rasterized once and only the layer rotates each frame.
        icon.setLayerType(View.LAYER_TYPE_HARDWARE, null);
        if (on) {
            // Accelerate from rest to full speed (t^2 ends at 2x the average speed = steady speed).
            ObjectAnimator up = ObjectAnimator.ofFloat(icon, View.ROTATION, from, from + 180f);
            up.setDuration(FAN_TURN_MS);
            up.setInterpolator(new android.view.animation.AccelerateInterpolator());
            ObjectAnimator steady = ObjectAnimator.ofFloat(icon, View.ROTATION, from + 180f, from + 540f);
            steady.setDuration(FAN_TURN_MS);
            steady.setRepeatCount(ValueAnimator.INFINITE);
            steady.setInterpolator(new LinearInterpolator());
            // At full speed, swap to the motion-blurred blades.
            up.addListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(android.animation.Animator a) {
                    if (fanOn[fan]) icon.setImageResource(R.drawable.ic_fan_blur);
                }
            });
            android.animation.AnimatorSet set = new android.animation.AnimatorSet();
            set.playSequentially(up, steady);
            fanSpins[fan] = set;
            set.start();
        } else {
            // Coast to a stop from full speed instead of snapping back to 0.
            ObjectAnimator down = ObjectAnimator.ofFloat(icon, View.ROTATION, from, from + 240f);
            down.setDuration(FAN_TURN_MS * 4 / 3);
            down.setInterpolator(new android.view.animation.DecelerateInterpolator());
            // Sharp blades return once the fan has slowed to half speed (decelerate speed falls linearly).
            down.addUpdateListener(a -> {
                if (a.getAnimatedFraction() >= 0.5f && !fanOn[fan]) icon.setImageResource(R.drawable.ic_fan);
            });
            down.addListener(new android.animation.AnimatorListenerAdapter() {
                @Override public void onAnimationEnd(android.animation.Animator a) {
                    if (!fanOn[fan]) icon.setLayerType(View.LAYER_TYPE_NONE, null);
                }
            });
            fanSpins[fan] = down;
            down.start();
        }
    }

    private void updateTransportStatus() {
        if (transport == Transport.FIREBASE) {
            setTransportText(firebaseReady ? "Cloud · syncing…" : "Cloud · login required");
        } else {
            setTransportText(isBluetoothConnected() ? "Bluetooth · direct" : "Bluetooth · choose device");
        }
        updateSessionUi();
    }

    private void setTransportText(String text) {
        txtTransportStatus.setText(text);
        txtSettingsTransportStatus.setText(text);
    }

    private void updateSessionUi() {
        if (txtSystemFirebase == null || txtSystemBluetooth == null) return;
        txtSystemFirebase.setText("Cloud " + (firebaseReady ? "signed in" : "signed out"));
        txtSystemBluetooth.setText("Bluetooth " + (isBluetoothConnected() ? "connected" : "disconnected"));
    }

    private String buildDiagnosticsText() {
        String base = "ESAVE DIAGNOSTICS\nApp " + BuildConfig.VERSION_NAME + "\nRoom: " + BuildConfig.ROOM_ID + "\nSelected transport: " + transport.name();
        if (currentState == null) return base + "\nController state: not received";
        SmartClassroomState s = currentState;
        String stateSource = currentStateTransport == null ? "UNKNOWN" : currentStateTransport.name();
        int gpioVerified = 0;
        if (s.relayVerificationAvailable) for (int i = 0; i < relayTiles.length && i < s.relayGpioVerified.length; i++) if (s.relayGpioVerified[i]) gpioVerified++;
        return base + "\nState source: " + stateSource + "\nController IP: " + s.ip + "\nUptime: " + (s.uptimeMs / 1000L) + "s\nFree heap: " + s.freeHeap + " B\nSchema: " + safe(s.schemaVersion) +
                "\nRelay GPIO verification: " + (s.relayVerificationAvailable ? (gpioVerified + "/6") : "not reported") +
                "\nLast source: " + s.lastSource +
                "\nConnectivity: Wi-Fi " + yesNo(s.wifi) + ", Firebase " + yesNo(s.firebase) + ", Bluetooth " + yesNo(s.bluetooth) + ", RSSI " + s.rssi + " dBm" +
                "\nTelemetry #" + s.telemetrySequence + ", Firebase auth/http failures " + s.firebaseAuthFailures + "/" + s.firebaseHttpFailures +
                (s.lastHttpErrorCode < 0 ? ", HTTP " + s.lastHttpErrorCode + " " + safe(s.lastHttpErrorText) : "") +
                "\nOccupancy: " + s.people + (s.occupancyValid ? " (valid)" : " (needs correction)") +
                ", entrance +" + s.entranceEvents + ", exit -" + s.exitEvents + ", rejected noise " + s.rejectedPassages +
                ", underflow " + s.exitUnderflowEvents + ", last " + safe(s.lastOccupancyEvent) +
                "\nBeams: entrance " + (s.entranceBroken ? "BROKEN" : "CLEAR") + ", exit " + (s.exitBroken ? "BROKEN" : "CLEAR") +
                "\nDetection: " + safe(s.detectionMethod) + ", valid pulse " + s.passageMinMs + "-" + s.passageMaxMs + " ms" +
                (s.humanOnlyGuaranteed ? ", human-only confirmed" : ", IR passage filter only") +
                (s.instantLatchEnabled ? ", instant latch " + s.instantLatchTargetMs + " ms (clear gap " + s.minClearGapMs + " ms, latency " + s.lastPassageProcessLatencyMs + " ms)" : "") +
                "\nNoise guard: " + (s.noiseGuardActive ? "ACTIVE (entrance " + (s.entranceSensorNoisy ? "noisy" : "ok") + ", exit " + (s.exitSensorNoisy ? "noisy" : "ok") + ")" : "off") +
                "\nPZEM: " + (s.energyValid ? "valid" : "invalid") + ", last good age " + s.pzemLastGoodAgeSeconds + " s, sample #" + s.pzemSampleSequence + ", invalid samples " + s.pzemInvalidSamples;
    }

    private void copyText(String label, String text) {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) {
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
            toast("Copied to clipboard");
        }
    }

    private static boolean isSessionError(String message) {
        if (message == null) return false;
        String m = message.toLowerCase(Locale.US);
        return m.contains("session expired") || m.contains("login required") || m.contains("invalid_refresh_token") || m.contains("token_expired");
    }

    /** The PIN is only built into the app when rsu.bluetoothPin is set in local.properties. */
    private static String pairingPinHint() {
        return BuildConfig.BLUETOOTH_PAIRING_PIN.isEmpty()
                ? "PIN: ask your administrator"
                : "PIN " + BuildConfig.BLUETOOTH_PAIRING_PIN;
    }

    private static String yesNo(boolean v) { return v ? "OK" : "--"; }
    private static String formatUptime(long ms) {
        long m = ms / 60000L;
        return m < 60 ? m + "m" : (m / 60) + "h " + (m % 60) + "m";
    }
    private static String safe(String v) { return v == null || v.trim().isEmpty() ? "--" : v.trim(); }
    private static String safeMessage(Throwable t) {
        if (t == null) return "Unknown error";
        String m = t.getMessage();
        return m == null || m.trim().isEmpty() ? t.getClass().getSimpleName() : m.trim();
    }
    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }

    @Override public void onBluetoothStatus(String message, boolean connected) {
        ui.post(() -> {
            if (transport == Transport.BLUETOOTH) {
                setTransportText(message);
                txtMessage.setText(message);
                txtHeaderMode.setText(connected ? "BT LIVE" : "BT OFF");
                if (!connected) setRelaySwitchesEnabled(false);
            }
            updateSessionUi();
        });
    }

    @Override public void onState(SmartClassroomState state) {
        ui.post(() -> {
            lastBluetoothStateAt = System.currentTimeMillis();
            if (transport == Transport.BLUETOOTH && pollingActive && isBluetoothConnected()) {
                renderState(state, false);
                txtMessage.setText("Live state received from ESP32 Bluetooth.");
            }
        });
    }

    @Override public void onAck(String action, boolean ok, String error) {
        ui.post(() -> {
            if (transport == Transport.BLUETOOTH) {
                if (ok && "wifi_setup".equals(action)) {
                    txtMessage.setText("Wi-Fi setup open: join " + WIFI_SETUP_AP_SSID + ", then open http://192.168.4.1/");
                } else if (!ok && "wifi_setup".equals(action) && "UNKNOWN_COMMAND".equals(error)) {
                    txtMessage.setText("This controller firmware has no Wi-Fi setup. Flash firmware 25.18+.");
                } else {
                    txtMessage.setText(ok ? ("ESP32 ACK: " + safe(action) + " OK")
                            : ("ESP32 rejected " + safe(action) + ": " + safe(error)));
                }
            }
        });
    }

    @Override public void onControllerBye(String reason) {
        ui.post(() -> {
            if (transport != Transport.BLUETOOTH) return;
            if (firebaseReady) {
                radioFirebase.setChecked(true);
                txtMessage.setText("The controller's Bluetooth session ended. Switched to the cloud.");
            } else {
                txtMessage.setText("The controller is back on the cloud and turned Bluetooth off. Log in to the cloud to keep controlling it.");
            }
        });
    }

    @Override public void onBluetoothError(String message) {
        ui.post(() -> {
            if (transport == Transport.BLUETOOTH) {
                setTransportText(message);
                txtMessage.setText(message);
                txtHeaderMode.setText("BT ERROR");
                setRelaySwitchesEnabled(false);
            }
            updateSessionUi();
        });
    }

    @Override protected void onStart() {
        super.onStart();
        pollingActive = true;
        ui.removeCallbacks(pollTask);
        ui.post(pollTask);
        if (transport == Transport.BLUETOOTH && isBluetoothConnected()) {
            bluetooth.requestState();
        }
        if (transport == Transport.FIREBASE && firebaseReady) startFirebaseRealtimeStream();
    }

    @Override protected void onStop() {
        pollingActive = false;
        ui.removeCallbacks(pollTask);
        stopFirebaseRealtimeStream();
        super.onStop();
    }

    @Override protected void onSaveInstanceState(Bundle outState) {
        outState.putInt(STATE_SCREEN, selectedScreen);
        outState.putString(STATE_TRANSPORT, transport.name());
        super.onSaveInstanceState(outState);
    }

    @Override public void onBackPressed() {
        if (selectedScreen != SCREEN_DASHBOARD) {
            showScreen(SCREEN_DASHBOARD);
        } else {
            super.onBackPressed();
        }
    }

    @Override protected void onDestroy() {
        for (android.animation.Animator spin : fanSpins) if (spin != null) spin.cancel();
        for (ValueAnimator pulse : bulbPulses) if (pulse != null) pulse.cancel();
        pollingActive = false;
        ui.removeCallbacks(pollTask);
        stopFirebaseRealtimeStream();
        if (bluetoothBound) {
            if (bluetooth != null) bluetooth.clearListener(this);
            try { unbindService(bluetoothConnection); } catch (Exception ignoredUnbind) {}
            bluetoothBound = false;
            bluetooth = null;
        }
        network.shutdownNow();
        commands.shutdownNow();
        super.onDestroy();
    }
}
