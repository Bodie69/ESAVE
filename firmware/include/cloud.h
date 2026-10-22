#pragma once
// Wi-Fi, network diagnostics, verified HTTPS and Firebase Auth / Realtime Database requests.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "config.h"

extern volatile bool firebaseAuthenticated;
extern unsigned long firebaseAuthFailures;
extern unsigned long firebaseHttpFailures;
extern unsigned long lastFirebaseOkAt;
extern unsigned long firebaseAuthRetryMs;
extern int lastHttpErrorCode;
extern String lastHttpErrorText;
extern String lastHttpHost;
extern uint32_t firebaseTransportFailuresConsecutive;
extern bool lastFirebaseDnsOk;
extern bool lastFirebaseTcp443Ok;
extern bool lastFirebaseTls443Ok;
extern String lastFirebaseResolvedIp;
extern String lastFirebaseTlsError;

void resetFirebaseConnection(bool dropTokens);
bool tlsHeapAvailable();
// Holds contiguous heap for the next TLS handshake while Wi-Fi is up and no TLS session is
// open; frees it while Wi-Fi is down. Call from firebaseNetworkTask on every pass.
void maintainTlsReserve(bool wifiConnected);
// Closes the RTDB session, frees the TLS reserve and signs out, so Bluetooth gets the heap.
// The next Firebase request after Bluetooth is off signs in again.
void stopFirebaseForBluetooth();

// httpsJsonRequest() result when the heap is too low to attempt TLS.
static const int HTTP_ERROR_LOW_MEMORY = -2000;
bool firebaseRequest(const String &method, const String &path, const String &body, String &response, bool retryAuth = true);
