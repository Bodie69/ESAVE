#include "energy.h"

#include "state.h"

HardwareSerial pzemSerial(2);

static PZEM004Tv30 pzem(pzemSerial, PZEM_RX_PIN, PZEM_TX_PIN);

float voltage = NAN;
float currentA = NAN;
float powerW = NAN;
float energyKWh = NAN;
float frequencyHz = NAN;
float powerFactor = NAN;
bool pzemValid = false;
unsigned long lastPzemGoodAt = 0;
uint64_t pzemSampleSequence = 0;
unsigned long pzemInvalidSamples = 0;

void readPzem() {
  // Each value comes directly from the PZEM-004T v3 over UART.
  // Invalid/out-of-range samples are NEVER published as live electrical data.
  // Modbus I/O is slow, so it runs under pzemMutex only; stateMutex is taken afterwards.
  float v, a, w, kwh, hz, pf;
  {
    StateLock pzemLock(pzemMutex);
    v = pzem.voltage();
    a = pzem.current();
    w = pzem.power();
    kwh = pzem.energy();
    hz = pzem.frequency();
    pf = pzem.pf();
  }

  bool valid = isfinite(v) && isfinite(a) && isfinite(w) && isfinite(kwh) &&
               isfinite(hz) && isfinite(pf) &&
               v >= 50.0f && v <= 300.0f &&
               a >= 0.0f && a <= 120.0f &&
               w >= 0.0f && w <= 36000.0f &&
               kwh >= 0.0f && kwh <= 10000000.0f &&
               hz >= 40.0f && hz <= 70.0f &&
               pf >= 0.0f && pf <= 1.05f;

  StateLock lock;

  // Push immediately only for changes the app actually shows. Routine 1 s samples ride on
  // the regular heartbeat instead of keeping Firebase permanently dirty.
  bool significant = valid != pzemValid ||
                     (valid && (fabsf(w - powerW) >= 1.0f || fabsf(v - voltage) >= 1.0f ||
                                fabsf(kwh - energyKWh) >= 0.001f));

  if (valid) {
    voltage = v;
    currentA = a;
    powerW = w;
    energyKWh = kwh;
    frequencyHz = hz;
    powerFactor = pf;
    pzemValid = true;
    lastPzemGoodAt = millis();
    pzemSampleSequence++;
  } else {
    pzemValid = false;
    pzemInvalidSamples++;
    voltage = currentA = powerW = energyKWh = frequencyHz = powerFactor = NAN;
  }

  if (significant) markStateDirty(valid ? "pzem_sample" : "pzem_invalid");
}

bool resetPzemEnergy() {
  bool ok;
  {
    StateLock pzemLock(pzemMutex);
    ok = pzem.resetEnergy();
  }
  if (ok) readPzem();
  return ok;
}
