// Security-rules tests for ../database.rules.json. Run with `npm install && npm test`, which
// starts the Realtime Database emulator. Payloads mirror exactly what firmware V25.17 writes
// (buildState, writeFirebaseAck, maybeWriteEnergyHistory) and what the Android app sends
// (FirebaseRestClient.command + per-command fields).
import { readFileSync } from 'node:fs';
import { initializeTestEnvironment, assertSucceeds, assertFails } from '@firebase/rules-unit-testing';
import { ref, set, get, serverTimestamp, query, orderByKey, startAt } from 'firebase/database';

// Default: ../database.rules.json next to this folder; a path argument overrides it.
const RULES = process.argv[2] ?? new URL('../database.rules.json', import.meta.url);
const ROOM = 'room206';
const testEnv = await initializeTestEnvironment({
  projectId: 'demo-rsu',
  database: { rules: readFileSync(RULES, 'utf8'), host: '127.0.0.1', port: 9000 },
});

await testEnv.withSecurityRulesDisabled(async (ctx) => {
  await set(ref(ctx.database(), 'access'), {
    controllers: { esp: ROOM, shared: ROOM, otherRoomEsp: 'room301' },
    operators: { op: { [ROOM]: true }, shared: { [ROOM]: true }, otherRoomOp: { room301: true } },
  });
});

const db = (uid) => (uid ? testEnv.authenticatedContext(uid) : testEnv.unauthenticatedContext()).database();
const esp = db('esp'), op = db('op'), stranger = db('stranger'), anon = db(null), shared = db('shared');
const otherEsp = db('otherRoomEsp'), otherOp = db('otherRoomOp');
const p = (s) => `rooms/${ROOM}/${s}`;

// Mirrors buildState(doc, true) in firmware V25.13 (PZEM-valid case).
const firmwareState = (overrides = {}) => ({
  schema_version: '25.17', device_id: 'room206-controller', room_id: ROOM, device_epoch: 1792000000,
  uptime_ms: 123456, mode: 'AUTO', last_control_source: 'auto_occupied', telemetry_sequence: 42,
  server_timestamp_ms: serverTimestamp(),
  connectivity: { wifi: true, firebase: true, bluetooth: false, wifi_rssi: -55, ip: '192.168.1.20' },
  occupancy: {
    count: 3, occupied: true, valid: true, entrance_broken: false, exit_broken: false, entrance_raw: 1, exit_raw: 1,
    entrance_events: 10, exit_events: 7, exit_underflow_events: 0, last_event: 'entrance_fast_validated',
    vacancy_delay_seconds: 30, detection_method: 'GOGOU_BREAK_BEAM_FAST_ISR_EDGE_FILTER', fast_pass_enabled: true,
    event_sequence: 17, last_passage_direction: 'entrance', last_passage_process_latency_ms: 1,
    person_classifier_present: false, human_only_guaranteed: false, passage_min_ms: 30, instant_latch_enabled: true,
    instant_latch_target_ms: 30, min_clear_gap_ms: 20, passage_max_ms: 5000, entrance_rejected_short: 0,
    entrance_rejected_long: 0, exit_rejected_short: 0, exit_rejected_long: 0, entrance_sensor_noisy: false,
    exit_sensor_noisy: false, entrance_noise_rejected: 0, exit_noise_rejected: 0, noise_guard_active: false,
    entrance_edges_window: 0, exit_edges_window: 0,
  },
  energy: { valid: true, voltage_v: 229.5, current_a: 1.234, power_w: 250.1, energy_kwh: 12.345, frequency_hz: 60,
            power_factor: 0.95, last_good_age_seconds: 0, sample_sequence: 99, invalid_samples: 0, source: 'PZEM-004T_V3_DIRECT' },
  relays: { light1: true, light2: true, light3: true, light4: true, fan1: true, fan2: true },
  relay_gpio_verified: { light1: true, light2: true, light3: true, light4: true, fan1: true, fan2: true },
  diagnostics: { free_heap: 120000, firebase_auth_failures: 0, firebase_http_failures: 0, last_http_error_code: 200,
                 last_http_error_text: 'HTTP 200', last_http_host: 'x', last_bt_command_age_seconds: -1, last_state_change: 'pzem_sample' },
  ...overrides,
});
// PZEM-invalid case: the firmware writes null electrical values (absent in RTDB).
const firmwareStateNoPzem = () => firmwareState({ energy: { valid: false, last_good_age_seconds: -1, sample_sequence: 0, invalid_samples: 5, source: 'PZEM-004T_V3_DIRECT' } });

// Mirrors FirebaseRestClient.command("android_firebase").
let nextId = Date.now();
const cmd = (extra = {}) => ({ command_id: ++nextId, source: 'android_firebase', requested_at_ms: Date.now(), ...extra });
// Mirrors writeFirebaseAck().
const ack = (extra = {}) => ({ command_id: nextId, action: 'set_mode', ok: true, detail: 'AUTO', device_epoch: 1792000000, server_timestamp_ms: serverTimestamp(), ...extra });

const results = [];
async function check(name, expectOk, fn) {
  try {
    await (expectOk ? assertSucceeds(fn()) : assertFails(fn()));
    results.push(['PASS', name]);
  } catch (e) {
    results.push(['FAIL', `${name} -> ${e.message.split('\n')[0]}`]);
  }
}

// --- state ---
await check('ESP32 writes full firmware state', true, () => set(ref(esp, p('state')), firmwareState()));
await check('ESP32 writes state with PZEM invalid (null energy)', true, () => set(ref(esp, p('state')), firmwareStateNoPzem()));
await check('ESP32 MANUAL mode state', true, () => set(ref(esp, p('state')), firmwareState({ mode: 'MANUAL' })));
await check('ESP32 cannot fake server timestamp', false, () => set(ref(esp, p('state')), firmwareState({ server_timestamp_ms: Date.now() + 60000 })));
await check('ESP32 cannot write wrong room_id', false, () => set(ref(esp, p('state')), firmwareState({ room_id: 'room301' })));
await check('ESP32 cannot write bad mode', false, () => set(ref(esp, p('state')), firmwareState({ mode: 'PARTY' })));
await check('ESP32 cannot write unknown relay key', false, () => set(ref(esp, p('state')), firmwareState({ relays: { light9: true } })));
await check('ESP32 cannot write count > 999', false, () => set(ref(esp, p('state')), firmwareState({ occupancy: { count: 5000, valid: true } })));
await check('ESP32 for another room cannot write room206 state', false, () => set(ref(otherEsp, p('state')), firmwareState()));
await check('Operator reads state', true, () => get(ref(op, p('state'))));
await check('ESP32 reads state', true, () => get(ref(esp, p('state'))));
await check('Operator cannot write state', false, () => set(ref(op, p('state')), firmwareState()));
await check('Other-room operator cannot read room206 state', false, () => get(ref(otherOp, p('state'))));
await check('Signed-in stranger cannot read state', false, () => get(ref(stranger, p('state'))));
await check('Unauthenticated cannot read state', false, () => get(ref(anon, p('state'))));

// --- commands (every app command shape) ---
await check('Operator: set_mode auto', true, () => set(ref(op, p('commands/mode')), cmd({ mode: 'auto' })));
await check('Operator: set_mode manual', true, () => set(ref(op, p('commands/mode')), cmd({ mode: 'manual' })));
for (const d of ['light1', 'light2', 'light3', 'light4', 'fan1', 'fan2']) {
  await check(`Operator: relay ${d}`, true, () => set(ref(op, p(`commands/devices/${d}`)), cmd({ state: true })));
}
await check('Operator: all_off', true, () => set(ref(op, p('commands/all_off')), cmd()));
await check('Operator: set_count 0', true, () => set(ref(op, p('commands/set_count')), cmd({ count: 0 })));
await check('Operator: set_count 999', true, () => set(ref(op, p('commands/set_count')), cmd({ count: 999 })));
await check('Operator: calibrate_ir', true, () => set(ref(op, p('commands/calibrate_ir')), cmd()));
await check('Operator: reset_energy', true, () => set(ref(op, p('commands/reset_energy')), cmd()));
await check('Operator: bluetooth_session', true, () => set(ref(op, p('commands/bluetooth_session')), cmd()));
await check('Reject: bluetooth_session extra field', false, () => set(ref(op, p('commands/bluetooth_session')), cmd({ minutes: 60 })));
await check('Reject: bad mode value', false, () => set(ref(op, p('commands/mode')), cmd({ mode: 'party' })));
await check('Reject: unknown relay light9', false, () => set(ref(op, p('commands/devices/light9')), cmd({ state: true })));
await check('Reject: relay state not boolean', false, () => set(ref(op, p('commands/devices/light1')), cmd({ state: 'on' })));
await check('Reject: set_count 1000', false, () => set(ref(op, p('commands/set_count')), cmd({ count: 1000 })));
await check('Reject: set_count -1', false, () => set(ref(op, p('commands/set_count')), cmd({ count: -1 })));
await check('Reject: set_count 2.5', false, () => set(ref(op, p('commands/set_count')), cmd({ count: 2.5 })));
await check('Reject: extra field on command', false, () => set(ref(op, p('commands/all_off')), cmd({ evil: 1 })));
await check('Reject: missing requested_at_ms', false, () => set(ref(op, p('commands/all_off')), { command_id: 5, source: 'x' }));
await check('Reject: unknown command node', false, () => set(ref(op, p('commands/reboot')), cmd()));
await check('Reject: vacancy_delay command (setting removed in V25.17)', false, () => set(ref(op, p('commands/vacancy_delay')), cmd({ seconds: 30 })));
await check('Reject: old settings/vacancy_delay_seconds path', false, () => set(ref(op, p('commands/settings/vacancy_delay_seconds')), 30));
await check('ESP32 (not operator) cannot send commands', false, () => set(ref(esp, p('commands/all_off')), cmd()));
await check('Stranger cannot send commands', false, () => set(ref(stranger, p('commands/all_off')), cmd()));
await check('Unauthenticated cannot send commands', false, () => set(ref(anon, p('commands/all_off')), cmd()));
await check('Other-room operator cannot command room206', false, () => set(ref(otherOp, p('commands/all_off')), cmd()));
await check('ESP32 reads whole commands node', true, () => get(ref(esp, p('commands'))));
await check('Operator cannot read commands', false, () => get(ref(op, p('commands'))));

// --- acks ---
await check('ESP32 writes ACK', true, () => set(ref(esp, p('acks/last')), ack()));
await check('ESP32 writes COMMAND_EXPIRED ACK', true, () => set(ref(esp, p('acks/last')), ack({ ok: false, detail: 'COMMAND_EXPIRED' })));
await check('ESP32 cannot fake ACK timestamp', false, () => set(ref(esp, p('acks/last')), ack({ server_timestamp_ms: 1 })));
await check('Operator reads acks/last', true, () => get(ref(op, p('acks/last'))));
await check('Operator cannot forge ACK', false, () => set(ref(op, p('acks/last')), ack()));
await check('Stranger cannot read ACK', false, () => get(ref(stranger, p('acks/last'))));

// --- per-command ACKs (firmware V25.16 acks/by_id) ---
const ackId = 1792130000123;
await check('ESP32 writes ACK by command id', true, () => set(ref(esp, p(`acks/by_id/${ackId}`)), ack({ command_id: ackId })));
await check('Operator reads own ACK by id', true, () => get(ref(op, p(`acks/by_id/${ackId}`))));
await check('Operator cannot forge ACK by id', false, () => set(ref(op, p(`acks/by_id/${ackId + 1}`)), ack({ command_id: ackId + 1 })));
await check('Operator cannot edit an existing ACK', false, () => set(ref(op, p(`acks/by_id/${ackId}/ok`)), false));
await check('Operator deletes ACK after reading', true, () => set(ref(op, p(`acks/by_id/${ackId}`)), null));
await check('Reject: non-numeric ACK id', false, () => set(ref(esp, p('acks/by_id/abc')), ack()));
await check('Reject: ACK by id with faked timestamp', false, () => set(ref(esp, p(`acks/by_id/${ackId}`)), ack({ server_timestamp_ms: 5 })));
await check('Stranger cannot delete ACKs', false, () => set(ref(stranger, p(`acks/by_id/${ackId}`)), null));
await check('Other-room operator cannot delete room206 ACK', false, () => set(ref(otherOp, p(`acks/by_id/${ackId}`)), null));

// --- energy history (firmware V25.15 maybeWriteEnergyHistory) ---
const hist = (extra = {}) => ({ kwh: 12.345, epoch: 1792130000, ts: serverTimestamp(), ...extra });
await check('ESP32 writes hourly kWh reading', true, () => set(ref(esp, p('energy_history/hourly/2026101614')), hist()));
await check('ESP32 writes reading after meter reset (0 kWh)', true, () => set(ref(esp, p('energy_history/hourly/2026101615')), hist({ kwh: 0 })));
await check('ESP32 overwrites same hour after retry', true, () => set(ref(esp, p('energy_history/hourly/2026101614')), hist({ kwh: 12.4 })));
await check('Reject: bad hour key (hour 24)', false, () => set(ref(esp, p('energy_history/hourly/2026101624')), hist()));
await check('Reject: bad hour key (month 13)', false, () => set(ref(esp, p('energy_history/hourly/2026131614')), hist()));
await check('Reject: non-numeric key', false, () => set(ref(esp, p('energy_history/hourly/latest')), hist()));
await check('Reject: negative kWh', false, () => set(ref(esp, p('energy_history/hourly/2026101616')), hist({ kwh: -1 })));
await check('Reject: faked ts', false, () => set(ref(esp, p('energy_history/hourly/2026101616')), hist({ ts: 1 })));
await check('Reject: missing epoch', false, () => set(ref(esp, p('energy_history/hourly/2026101616')), { kwh: 1, ts: serverTimestamp() }));
await check('Reject: extra field', false, () => set(ref(esp, p('energy_history/hourly/2026101616')), hist({ note: 'x' })));
await check('ESP32 cannot wipe all history at once', false, () => set(ref(esp, p('energy_history/hourly')), null));
await check('Operator cannot write history', false, () => set(ref(op, p('energy_history/hourly/2026101617')), hist()));
await check('Operator reads history (app query)', true, () => get(query(ref(op, p('energy_history/hourly')), orderByKey(), startAt('2026091600'))));
await check('Stranger cannot read history', false, () => get(ref(stranger, p('energy_history/hourly'))));
await check('Other-room operator cannot read history', false, () => get(ref(otherOp, p('energy_history'))));

// --- access list and other paths ---
await check('Operator cannot read /access', false, () => get(ref(op, 'access')));
await check('Operator cannot grant themselves access', false, () => set(ref(op, 'access/operators/stranger/room206'), true));
await check('ESP32 cannot change /access', false, () => set(ref(esp, 'access/controllers/stranger'), ROOM));
await check('Operator cannot write unknown room child', false, () => set(ref(op, p('junk')), 1));
await check('Nobody can read the root', false, () => get(ref(op, '/')));

// --- one shared login for app + ESP32 ---
await check('Shared account: writes state', true, () => set(ref(shared, p('state')), firmwareState()));
await check('Shared account: sends command', true, () => set(ref(shared, p('commands/mode')), cmd({ mode: 'auto' })));
await check('Shared account: reads commands', true, () => get(ref(shared, p('commands'))));

await testEnv.cleanup();
for (const [status, name] of results) console.log(`${status}  ${name}`);
const failed = results.filter(([s]) => s === 'FAIL').length;
console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
