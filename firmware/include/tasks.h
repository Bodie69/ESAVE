#pragma once
// FreeRTOS tasks (realtime sensor, Firebase network) and the task watchdog.

#include <Arduino.h>

#include "config.h"

extern TaskHandle_t sensorRealtimeTaskHandle;
extern TaskHandle_t firebaseNetworkTaskHandle;
extern volatile unsigned long networkTaskHeartbeatAt;

void setupTaskWatchdog();
void sensorRealtimeTask(void *parameter);
void firebaseNetworkTask(void *parameter);
