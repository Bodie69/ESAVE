#pragma once
// Copy this file to secrets.h and fill in real values. secrets.h is git-ignored: never commit it.
// The firmware refuses to sign in to Firebase while any YOUR_... placeholder is left.

// Wi-Fi is NOT set here: join the controller's setup access point (ESAVE-Room206-Setup)
// and choose the network at http://192.168.4.1/ (see README.md). This is that AP's WPA2
// password, 8-63 characters, unique to this classroom. Give it only to staff.
#define SETUP_AP_PASSWORD "YOUR_SETUP_AP_PASSWORD"

// Firebase project settings (Project settings > General > Web API key; Realtime Database URL).
#define API_KEY         "YOUR_FIREBASE_WEB_API_KEY"
#define DATABASE_URL    "https://YOUR_PROJECT-default-rtdb.asia-southeast1.firebasedatabase.app"

// Dedicated Email/Password account for this controller. Put its UID under
// /access/controllers/<uid> = "room206" in the database (see README.md).
#define USER_EMAIL      "YOUR_CONTROLLER_ACCOUNT@example.com"
#define USER_PASSWORD   "YOUR_CONTROLLER_PASSWORD"

// Bluetooth pairing PIN, 6-16 digits, unique to this classroom. Give it only to staff.
#define BLUETOOTH_PIN   "000000"

// Diagnostics only - leave commented out in normal use.
// #define FIREBASE_TLS_INSECURE 1           // disables certificate checks (unsafe)
// #define BLUETOOTH_LEGACY_TEXT_COMMANDS 1  // accept "ALL OFF"-style serial-terminal commands
