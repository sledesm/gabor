/*
 * Muse gadget firmware for the Seeed XIAO nRF52840 Sense.
 *
 * The Muse Gadget SDK (github.com/facebookincubator/muse-gadget-sdk) connects
 * a gadget to Muse over Wi-Fi: the device holds a Noise session to Muse over
 * wss://. The nRF52840 has Bluetooth LE but no Wi-Fi, so this board cannot run
 * that firmware on its own. Instead it is a BLE peripheral, and a machine
 * running the SDK's Linux gadget (a Raspberry Pi, say) bridges it to Muse with
 * bridge/xiao_sense_bridge.py.
 *
 * What the board exposes over BLE (see README.md for the byte layouts):
 *   - State   (read, notify): battery, charging, IMU accel/gyro, temperature,
 *                             microphone level and peak, uptime.
 *   - LED     (write):         RGB colour and mode (solid, blink, breathe).
 *   - Event   (notify):        button press / long press, shake, free fall.
 *   - Standard Battery and Device Information services.
 *
 * Board: "Seeed XIAO nRF52840 Sense" from the Seeed nRF52 Boards core
 * (Seeeduino:nrf52:xiaonRF52840Sense). Library: Seeed Arduino LSM6DS3.
 */

#include <Arduino.h>
#include <bluefruit.h>
#include <PDM.h>
#include <LSM6DS3.h>
#include <Wire.h>

#define FW_VERSION "1.0.0"

// ---- Pins -------------------------------------------------------------------

// The RGB LED is common-anode: a pin driven LOW lights its colour.
static const int PIN_R = LED_RED;
static const int PIN_G = LED_GREEN;
static const int PIN_B = LED_BLUE;

// Optional push button between D1 and GND (the XIAO's own button is RESET).
static const int PIN_USER_BUTTON = D1;

// ~CHG from the BQ25101 charger, low while charging (P0.17).
static const int PIN_CHARGING = 23;

// ---- GATT UUIDs ---------------------------------------------------------------
// Base 9a3e0000-6b1f-4c3a-9e2d-4d7573655853, stored little-endian.

#define MUSE_XIAO_UUID(n)                                                     \
  {0x53, 0x58, 0x65, 0x73, 0x75, 0x4d, 0x2d, 0x9e, 0x3a, 0x4c, 0x1f, 0x6b,   \
   (uint8_t)((n) & 0xff), (uint8_t)((n) >> 8), 0x3e, 0x9a}

static const uint8_t UUID_SVC[16] = MUSE_XIAO_UUID(0x0000);
static const uint8_t UUID_STATE[16] = MUSE_XIAO_UUID(0x0001);
static const uint8_t UUID_LED[16] = MUSE_XIAO_UUID(0x0002);
static const uint8_t UUID_EVENT[16] = MUSE_XIAO_UUID(0x0003);

BLEService museSvc(UUID_SVC);
BLECharacteristic stateChr(UUID_STATE);
BLECharacteristic ledChr(UUID_LED);
BLECharacteristic eventChr(UUID_EVENT);
BLEDis bledis;
BLEBas blebas;

// ---- Wire formats (little-endian, packed) -----------------------------------

#define STATE_VERSION 1
#define FLAG_IMU_OK 0x01
#define FLAG_MIC_OK 0x02
#define FLAG_CHARGING 0x04
#define FLAG_BUTTON_DOWN 0x08

struct __attribute__((packed)) State {
  uint8_t version;      // STATE_VERSION
  uint8_t flags;        // FLAG_*
  uint16_t battery_mv;  // battery voltage, mV (0 if unreadable)
  int16_t temp_cc;      // IMU die temperature, 0.01 °C
  int16_t accel_mg[3];  // milli-g, x/y/z
  int16_t gyro_cdps[3]; // 0.1 degrees per second, x/y/z
  uint16_t mic_rms;     // RMS of the audio since the last update, 0..32767
  uint16_t mic_peak;    // peak absolute sample since the last update
  uint32_t uptime_s;
  uint16_t event_count; // events sent since boot
};
static_assert(sizeof(State) == 28, "State layout changed");

enum EventType : uint8_t {
  EV_BUTTON = 1,
  EV_BUTTON_LONG = 2,
  EV_SHAKE = 3,
  EV_FREE_FALL = 4,
};

struct __attribute__((packed)) Event {
  uint8_t type;      // EventType
  uint8_t reserved;
  uint16_t seq;      // event_count after this event
  uint32_t uptime_ms;
};

enum LedMode : uint8_t { LED_SOLID = 0, LED_BLINK = 1, LED_BREATHE = 2 };

struct __attribute__((packed)) LedCmd {
  uint8_t r, g, b;
  uint8_t mode; // LedMode; optional, defaults to solid
};

// ---- State ------------------------------------------------------------------

LSM6DS3 imu(I2C_MODE, 0x6A);
static bool imuOk = false;
static bool micOk = false;

static State state;
static uint16_t eventCount = 0;

// Microphone: the PDM callback runs in interrupt context, so it only folds
// samples into accumulators that loop() reads and resets.
static int16_t pdmBuf[256];
static volatile uint64_t micSumSq = 0;
static volatile uint32_t micSamples = 0;
static volatile uint16_t micPeak = 0;

// LED: the colour Muse asked for, and the mode it plays in.
static LedCmd led = {0, 0, 0, LED_SOLID};
static bool ledOverride = false; // false: show the connection status instead

// ---- LED --------------------------------------------------------------------

static void writeRgb(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_R, 255 - r);
  analogWrite(PIN_G, 255 - g);
  analogWrite(PIN_B, 255 - b);
}

static void updateLed(uint32_t now) {
  if (!ledOverride) {
    // Status: a short blue blip every 2 s while advertising, a dim green one
    // while a bridge is connected.
    bool on = (now % 2000) < 60;
    if (Bluefruit.connected())
      writeRgb(0, on ? 40 : 0, 0);
    else
      writeRgb(0, 0, on ? 80 : 0);
    return;
  }
  uint16_t scale = 255;
  if (led.mode == LED_BLINK) {
    scale = (now % 1000) < 500 ? 255 : 0;
  } else if (led.mode == LED_BREATHE) {
    uint32_t t = now % 2000;
    uint32_t tri = t < 1000 ? t : 2000 - t; // 0..1000..0
    scale = (uint16_t)((tri * tri) * 255 / 1000000);
  }
  writeRgb(led.r * scale / 255, led.g * scale / 255, led.b * scale / 255);
}

static void onLedWrite(uint16_t, BLECharacteristic *, uint8_t *data, uint16_t len) {
  if (len < 3) return;
  led.r = data[0];
  led.g = data[1];
  led.b = data[2];
  led.mode = len >= 4 && data[3] <= LED_BREATHE ? data[3] : LED_SOLID;
  // All zero with solid hands the LED back to the status pattern.
  ledOverride = led.r || led.g || led.b;
}

// ---- Sensors ----------------------------------------------------------------

static void onPdmData() {
  int bytes = PDM.available();
  if (bytes > (int)sizeof(pdmBuf)) bytes = sizeof(pdmBuf);
  PDM.read(pdmBuf, bytes);
  int n = bytes / 2;
  uint64_t sum = 0;
  uint16_t peak = micPeak;
  for (int i = 0; i < n; i++) {
    int32_t s = pdmBuf[i];
    sum += (uint64_t)(s * s);
    uint16_t a = (uint16_t)(s < 0 ? -s : s);
    if (a > peak) peak = a;
  }
  micSumSq += sum;
  micSamples += n;
  micPeak = peak;
}

static uint16_t readBatteryMv() {
  // VBAT reaches P0.31 through a 1M / 510k divider, enabled by VBAT_ENABLE
  // held LOW. It stays LOW: driving it HIGH would put the full battery voltage
  // on P0.31.
  analogReference(AR_INTERNAL_2_4);
  analogReadResolution(12);
  uint32_t raw = 0;
  for (int i = 0; i < 4; i++) raw += analogRead(PIN_VBAT);
  raw /= 4;
  analogReference(AR_DEFAULT);
  uint32_t mv = raw * 2400UL / 4096UL * 1510UL / 510UL;
  // With no battery the charger output floats; report 0 rather than noise.
  return mv < 2500 ? 0 : (uint16_t)mv;
}

static uint8_t batteryPercent(uint16_t mv) {
  if (mv == 0) return 0;
  if (mv >= 4150) return 100;
  if (mv <= 3300) return 0;
  return (uint8_t)((mv - 3300) * 100UL / (4150 - 3300));
}

// ---- Events -----------------------------------------------------------------

static void sendEvent(EventType type) {
  eventCount++;
  Event ev = {type, 0, eventCount, millis()};
  eventChr.notify(&ev, sizeof(ev));
  Serial.printf("event %u seq %u\n", type, eventCount);
}

// Button on D1: press shorter than 1 s is a click, holding 1 s is a long press.
static void pollButton(uint32_t now) {
  static bool down = false;
  static bool longSent = false;
  static uint32_t since = 0;
  static uint32_t lastChange = 0;
  bool pressed = digitalRead(PIN_USER_BUTTON) == LOW;
  if (pressed != down && now - lastChange > 30) { // 30 ms debounce
    lastChange = now;
    down = pressed;
    if (down) {
      since = now;
      longSent = false;
    } else if (!longSent) {
      sendEvent(EV_BUTTON);
    }
  }
  if (down && !longSent && now - since >= 1000) {
    longSent = true;
    sendEvent(EV_BUTTON_LONG);
  }
}

// Motion events from the accelerometer magnitude (in g):
//   free fall: below 0.35 g for 80 ms; shake: 4 swings past 2.2 g within 1 s.
static void detectMotion(float ax, float ay, float az, uint32_t now) {
  static uint32_t fallSince = 0;
  static uint32_t lastFall = 0;
  static uint8_t swings = 0;
  static uint32_t swingWindow = 0;
  static uint32_t lastShake = 0;
  static bool high = false;

  float mag = sqrtf(ax * ax + ay * ay + az * az);

  if (mag < 0.35f) {
    if (!fallSince) fallSince = now;
    if (now - fallSince >= 80 && now - lastFall > 2000) {
      lastFall = now;
      sendEvent(EV_FREE_FALL);
    }
  } else {
    fallSince = 0;
  }

  if (now - swingWindow > 1000) {
    swingWindow = now;
    swings = 0;
  }
  if (!high && mag > 2.2f) {
    high = true;
    if (++swings >= 4 && now - lastShake > 2000) {
      lastShake = now;
      swings = 0;
      sendEvent(EV_SHAKE);
    }
  } else if (high && mag < 1.6f) {
    high = false;
  }
}

static void sampleImu(uint32_t now) {
  if (!imuOk) return;
  float ax = imu.readFloatAccelX(), ay = imu.readFloatAccelY(), az = imu.readFloatAccelZ();
  state.accel_mg[0] = (int16_t)(ax * 1000);
  state.accel_mg[1] = (int16_t)(ay * 1000);
  state.accel_mg[2] = (int16_t)(az * 1000);
  state.gyro_cdps[0] = (int16_t)constrain(imu.readFloatGyroX() * 10, -32767, 32767);
  state.gyro_cdps[1] = (int16_t)constrain(imu.readFloatGyroY() * 10, -32767, 32767);
  state.gyro_cdps[2] = (int16_t)constrain(imu.readFloatGyroZ() * 10, -32767, 32767);
  detectMotion(ax, ay, az, now);
}

// ---- BLE --------------------------------------------------------------------

static void onConnect(uint16_t handle) {
  BLEConnection *conn = Bluefruit.Connection(handle);
  char name[32] = {0};
  conn->getPeerName(name, sizeof(name));
  Serial.printf("connected: %s\n", name);
  conn->requestMtuExchange(247);
}

static void onDisconnect(uint16_t, uint8_t reason) {
  Serial.printf("disconnected, reason 0x%02x\n", reason);
  ledOverride = false;
}

static void setupBle() {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setTxPower(4);

  // Name: MuseXiao-XXXX, from the last bytes of the device address.
  uint8_t mac[6];
  Bluefruit.getAddr(mac);
  char name[20];
  snprintf(name, sizeof(name), "MuseXiao-%02X%02X", mac[1], mac[0]);
  Bluefruit.setName(name);

  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  bledis.setManufacturer("Seeed Studio");
  bledis.setModel("XIAO nRF52840 Sense");
  bledis.setFirmwareRev(FW_VERSION);
  bledis.begin();

  blebas.begin();
  blebas.write(0);

  museSvc.begin();

  stateChr.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  stateChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  stateChr.setFixedLen(sizeof(State));
  stateChr.begin();

  ledChr.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  ledChr.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  ledChr.setMaxLen(sizeof(LedCmd));
  ledChr.setWriteCallback(onLedWrite);
  ledChr.begin();

  eventChr.setProperties(CHR_PROPS_NOTIFY);
  eventChr.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  eventChr.setFixedLen(sizeof(Event));
  eventChr.begin();

  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(museSvc);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 800); // 100 ms fast, then 500 ms
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);

  Serial.printf("advertising as %s\n", name);
}

// ---- Arduino ----------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  writeRgb(0, 0, 0);

  pinMode(PIN_USER_BUTTON, INPUT_PULLUP);
  pinMode(PIN_CHARGING, INPUT);
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW);

  imuOk = imu.begin() == 0;

  PDM.onReceive(onPdmData);
  PDM.setBufferSize(sizeof(pdmBuf));
  micOk = PDM.begin(1, 16000) == 1;
  if (micOk) PDM.setGain(30);

  setupBle();

  state.version = STATE_VERSION;
  Serial.printf("muse-xiao-sense %s imu=%d mic=%d\n", FW_VERSION, imuOk, micOk);
}

void loop() {
  static uint32_t lastImu = 0, lastState = 0, lastBattery = 0;
  uint32_t now = millis();

  pollButton(now);
  updateLed(now);

  if (now - lastImu >= 20) { // 50 Hz, for the motion events
    lastImu = now;
    sampleImu(now);
  }

  if (now - lastBattery >= 10000 || lastBattery == 0) {
    lastBattery = now;
    state.battery_mv = readBatteryMv();
    blebas.write(batteryPercent(state.battery_mv));
  }

  if (now - lastState >= 500) { // 2 Hz state notifications
    lastState = now;

    noInterrupts();
    uint64_t sumSq = micSumSq;
    uint32_t samples = micSamples;
    uint16_t peak = micPeak;
    micSumSq = 0;
    micSamples = 0;
    micPeak = 0;
    interrupts();

    state.flags = (imuOk ? FLAG_IMU_OK : 0) | (micOk ? FLAG_MIC_OK : 0) |
                  (digitalRead(PIN_CHARGING) == LOW ? FLAG_CHARGING : 0) |
                  (digitalRead(PIN_USER_BUTTON) == LOW ? FLAG_BUTTON_DOWN : 0);
    state.mic_rms = samples ? (uint16_t)sqrtf((float)sumSq / samples) : 0;
    state.mic_peak = peak;
    state.temp_cc = imuOk ? (int16_t)(imu.readTempC() * 100) : 0;
    state.uptime_s = now / 1000;
    state.event_count = eventCount;

    stateChr.write(&state, sizeof(state));
    if (Bluefruit.connected()) stateChr.notify(&state, sizeof(state));
  }

  delay(5);
}
