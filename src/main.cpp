#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <DNSServer.h>

// --- I2C relays ---
#define I2C_SDA 4
#define I2C_SCL 15
#define PCF8574_RELAY 0x24
#define PCF8574_INPUT 0x22

// --- WiFi AP ---
const char* ap_ssid = "awh";
const char* ap_password = "clickclick";

// relayStates index is 0-based, so relays 1..6 are indexes 0..5.
const uint8_t RELAY_PHASE = 0; // Physical relay 1
const uint8_t RELAY_NEUTRAL = 1; // Physical relay 2
const uint8_t RELAY_SHUNT = 2; // Physical relay 3

const uint32_t SHUNT_DELAY_MS = 200;
const uint32_t STARTUP_CLOSE_TIMEOUT_MS = 15000;
const uint8_t RELAY_MOTOR_A = 4; // Physical relay 5
const uint8_t RELAY_MOTOR_B = 5; // Physical relay 6
const uint8_t DI5_BIT = 4; // P4 on input PCF8574 (IN_D5 on your schematic)
const bool DI5_ACTIVE_LOW = true;
const uint8_t DI6_BIT = 5; // P5 on input PCF8574 (IN_D6 on your schematic)
const bool DI6_ACTIVE_LOW = true;

// PCF8574 relay modules are usually active-low: true means relay coil ON (NO state).
bool relayStates[6] = {false, false, false, false, false, false};

// Delay to guarantee safe break-before-make switching.
const uint16_t RELAY_DEADTIME_MS = 120;
const bool STOP_USE_NO_STATE = false; // false = NC/NC stop, true = NO/NO stop

enum MotorDirection {
  MOTOR_STOP,
  MOTOR_OPEN,
  MOTOR_CLOSE
};

MotorDirection currentDirection = MOTOR_STOP;
int32_t logicalPosition = 0;
bool doorIsOpen = false;
bool doorIsClosed = false;

WebServer server(80);
DNSServer dnsServer;
const uint16_t DNS_PORT = 53;

bool powerOn = false;
uint32_t shuntStartMs = 0;
uint16_t remainingSeconds = 0;
uint32_t countdownTickMs = 0;
uint16_t cycleStartSeconds = 0;
bool di5Active = false;
bool di6Active = false;

void initInputExpander() {
  // For PCF8574, writing '1' makes a pin act as input (quasi-bidirectional).
  Wire.beginTransmission(PCF8574_INPUT);
  Wire.write(0xFF);
  Wire.endTransmission();
}

void writeRelays() {
  uint8_t value = 0xFF;
  for (uint8_t i = 0; i < 6; i++) {
    if (relayStates[i]) {
      bitClear(value, i);
    }
  }

  Wire.beginTransmission(PCF8574_RELAY);
  Wire.write(value);
  Wire.endTransmission();
}

void setMotorRelaysRaw(bool relayA_no, bool relayB_no) {
  relayStates[RELAY_MOTOR_A] = relayA_no;
  relayStates[RELAY_MOTOR_B] = relayB_no;
  writeRelays();
}

void stopMotor() {
  // For the described wiring, equal states on both relays stop the motor.
  setMotorRelaysRaw(STOP_USE_NO_STATE, STOP_USE_NO_STATE);
  currentDirection = MOTOR_STOP;
  logicalPosition = 0; // Reset software position marker on STOP.
}

void setPowerOff() {
  relayStates[RELAY_PHASE] = false;
  relayStates[RELAY_NEUTRAL] = false;
  relayStates[RELAY_SHUNT] = false;
  writeRelays();
  powerOn = false;
  shuntStartMs = 0;
  countdownTickMs = 0;
  if (cycleStartSeconds > 0) {
    remainingSeconds = cycleStartSeconds;
  }
}

void setPowerOn() {
  if (powerOn || remainingSeconds == 0 || !doorIsClosed) {
    return;
  }

  cycleStartSeconds = remainingSeconds;
  relayStates[RELAY_PHASE] = true;
  relayStates[RELAY_NEUTRAL] = true;
  relayStates[RELAY_SHUNT] = false; // start through resistor
  writeRelays();
  powerOn = true;
  shuntStartMs = millis();
  countdownTickMs = millis();
}

String formatTimeMMSS() {
  char buff[6];
  uint8_t minutes = remainingSeconds / 60;
  uint8_t seconds = remainingSeconds % 60;
  snprintf(buff, sizeof(buff), "%02u:%02u", minutes, seconds);
  return String(buff);
}

void adjustTime(String unit, int delta) {
  int minutes = remainingSeconds / 60;
  int seconds = remainingSeconds % 60;

  if (unit == "min") {
    minutes += delta;
    if (minutes < 0) {
      minutes = 0;
    }
    if (minutes > 99) {
      minutes = 99;
    }
  } else if (unit == "sec") {
    seconds += delta;
    if (seconds < 0) {
      seconds = 0;
    }
    if (seconds > 59) {
      seconds = 59;
    }
  }

  remainingSeconds = (uint16_t)(minutes * 60 + seconds);
}

void setMotorDirectionSafe(MotorDirection target) {
  if (currentDirection == target) {
    return; // Ignore repeated command in same direction to avoid relay chatter.
  }
  // Safety step: drop both relays first, then wait for contacts to settle.
  stopMotor();
  delay(RELAY_DEADTIME_MS);

  // Required complementary states:
  // OPEN:  relay5 = NC, relay6 = NO
  // CLOSE: relay5 = NO, relay6 = NC
  if (target == MOTOR_OPEN) {
    setMotorRelaysRaw(false, true);
  } else {
    setMotorRelaysRaw(true, false);
  }

  currentDirection = target;
}

const char* directionText() {
  if (currentDirection == MOTOR_OPEN) {
    return "Открыть";
  }
  if (currentDirection == MOTOR_CLOSE) {
    return "Закрыть";
  }
  return "Стоп";
}

const char* doorText() {
  if (doorIsOpen) {
    return "Открыта";
  }
  if (doorIsClosed) {
    return "Закрыта";
  }
  if (currentDirection == MOTOR_OPEN || currentDirection == MOTOR_CLOSE) {
    return "В движении";
  }
  return "Между положениями";
}

const char* closedLimitText() {
  return di5Active ? "Замкнут" : "Разомкнут";
}

bool doorMoving() {
  return currentDirection == MOTOR_OPEN || currentDirection == MOTOR_CLOSE;
}

bool canAdjustTime() {
  return !powerOn;
}

bool canPowerOn() {
  return !powerOn && remainingSeconds > 0 && doorIsClosed;
}

bool canPowerOff() {
  return powerOn;
}

bool canOpenDoor() {
  return !powerOn && currentDirection != MOTOR_OPEN && !di6Active && !doorIsOpen;
}

bool canCloseDoor() {
  return !powerOn && currentDirection != MOTOR_CLOSE && !di5Active && !doorIsClosed;
}

bool canStopDoor() {
  return !powerOn && currentDirection != MOTOR_STOP && !di6Active && !di5Active && !doorIsOpen && !doorIsClosed;
}

void updateInputStates() {
  Wire.requestFrom((uint8_t)PCF8574_INPUT, (uint8_t)1);
  if (Wire.available() < 1) {
    return;
  }

  uint8_t inputByte = Wire.read();
  bool di5BitState = bitRead(inputByte, DI5_BIT);
  bool di6BitState = bitRead(inputByte, DI6_BIT);
  di5Active = DI5_ACTIVE_LOW ? !di5BitState : di5BitState;
  di6Active = DI6_ACTIVE_LOW ? !di6BitState : di6BitState;
}

void updateDoorStateFromLimitSwitches() {
  if (di5Active && !di6Active) {
    doorIsClosed = true;
    doorIsOpen = false;
    return;
  }

  if (di6Active && !di5Active) {
    doorIsOpen = true;
    doorIsClosed = false;
    return;
  }

  if (!di5Active && !di6Active) {
    doorIsOpen = false;
    doorIsClosed = false;
  }
}

void ensureDoorClosedOnStartup() {
  updateInputStates();
  updateDoorStateFromLimitSwitches();

  if (doorIsClosed || doorIsOpen) {
    return;
  }

  setMotorDirectionSafe(MOTOR_CLOSE);
  uint32_t startedAt = millis();

  while ((millis() - startedAt) < STARTUP_CLOSE_TIMEOUT_MS) {
    delay(20);
    updateInputStates();
    if (di5Active) {
      break;
    }
  }

  stopMotor();
  updateInputStates();
  updateDoorStateFromLimitSwitches();
}

String generateHTML() {
  String html = R"rawliteral(
<!doctype html>
<html lang="ru">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Управление двигателем</title>
  <style>
    :root {
      --bg: #eef2ff;
      --card: #ffffff;
      --txt: #0f172a;
      --muted: #475569;
      --open: #2563eb;
      --close: #dc2626;
      --stop: #0f766e;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      display: grid;
      place-items: center;
      padding: 16px;
      font-family: "Segoe UI", Tahoma, sans-serif;
      color: var(--txt);
      background: radial-gradient(circle at top, #dbeafe, var(--bg) 52%);
    }
    .panel {
      width: min(460px, 100%);
      background: var(--card);
      border-radius: 16px;
      box-shadow: 0 10px 28px rgba(15, 23, 42, 0.12);
      padding: 22px;
    }
    h1 { margin: 0 0 8px; font-size: 24px; }
    p { margin: 0 0 14px; color: var(--muted); }
    .status {
      margin-bottom: 14px;
      padding: 10px 12px;
      border-radius: 10px;
      border: 1px solid #bfdbfe;
      background: #eff6ff;
      font-size: 14px;
    }
    .row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-bottom: 10px;
    }
    .time-title {
      margin: 0 0 8px;
      color: var(--muted);
      font-size: 14px;
      text-transform: uppercase;
      letter-spacing: 0.06em;
    }
    .time-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-bottom: 10px;
    }
    .time-btn {
      height: 42px;
      font-size: 24px;
      background: #0f172a;
      color: #fff;
    }
    .time-values {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
      margin-bottom: 8px;
      color: var(--muted);
      font-size: 13px;
      text-align: center;
      font-weight: 600;
    }
    .time-display {
      margin-bottom: 10px;
      border-radius: 12px;
      border: 1px solid #cbd5e1;
      background: #f8fafc;
      padding: 12px 10px;
      font-size: 32px;
      text-align: center;
      letter-spacing: 0.08em;
      font-weight: 700;
    }
    button {
      width: 100%;
      height: 56px;
      border: 0;
      border-radius: 12px;
      color: #fff;
      font-size: 20px;
      font-weight: 700;
      cursor: pointer;
    }
    button:disabled {
      opacity: 0.45;
      cursor: not-allowed;
      filter: grayscale(0.2);
      box-shadow: none;
    }
    .open { background: var(--open); }
    .close { background: var(--close); }
    .stop { background: var(--stop); }
  </style>
</head>
<body>
  <div class="panel">
    <h1>Микроволновая печь</h1>
    <div class="time-title">Время (минуты / секунды)</div>
    <div class="time-grid">
      <form action="/time_adjust" method="post">
        <input type="hidden" name="unit" value="min">
        <input type="hidden" name="delta" value="1">
        <button id="min-plus" class="time-btn" type="submit" )rawliteral";
  if (!canAdjustTime()) {
    html += "disabled";
  }
  html += R"rawliteral(>+</button>
      </form>
      <form action="/time_adjust" method="post">
        <input type="hidden" name="unit" value="sec">
        <input type="hidden" name="delta" value="1">
        <button id="sec-plus" class="time-btn" type="submit" )rawliteral";
  if (!canAdjustTime()) {
    html += "disabled";
  }
  html += R"rawliteral(>+</button>
      </form>
    </div>
    <div class="time-values">
      <div>Минуты</div>
      <div>Секунды</div>
    </div>
    <div class="time-display">)rawliteral";

  html += formatTimeMMSS();

  html += R"rawliteral(</div>
    <div class="time-grid">
      <form action="/time_adjust" method="post">
        <input type="hidden" name="unit" value="min">
        <input type="hidden" name="delta" value="-1">
        <button id="min-minus" class="time-btn" type="submit" )rawliteral";
  if (!canAdjustTime()) {
    html += "disabled";
  }
  html += R"rawliteral(>-</button>
      </form>
      <form action="/time_adjust" method="post">
        <input type="hidden" name="unit" value="sec">
        <input type="hidden" name="delta" value="-1">
        <button id="sec-minus" class="time-btn" type="submit" )rawliteral";
  if (!canAdjustTime()) {
    html += "disabled";
  }
  html += R"rawliteral(>-</button>
      </form>
    </div>
    <div class="row">
      <form action="/power_on" method="post">
        <button id="power-on" class="open" type="submit" )rawliteral";
  if (!canPowerOn()) {
    html += "disabled";
  }
  html += R"rawliteral(>Включить</button>
      </form>
      <form action="/power_off" method="post">
        <button id="power-off" class="close" type="submit" )rawliteral";
  if (!canPowerOff()) {
    html += "disabled";
  }
  html += R"rawliteral(>Выключить</button>
      </form>
    </div>
    <div class="status">Текущее направление: <b id="direction-text">)rawliteral";

  html += directionText();

  html += R"rawliteral(</b></div>
    <div class="status">Состояние двери: <b id="door-text">)rawliteral";

  html += doorText();

  html += R"rawliteral(</b></div>
    <div class="status">Концевик закрытия DI5: <b id="closed-limit-text">)rawliteral";

  html += closedLimitText();

  html += R"rawliteral(</b></div>

    <div class="row">
      <form action="/close" method="post">
        <button id="door-close" class="close" type="submit" )rawliteral";
  if (!canCloseDoor()) {
    html += "disabled";
  }
  html += R"rawliteral(>Закрыть</button>
      </form>
      <form action="/open" method="post">
        <button id="door-open" class="open" type="submit" )rawliteral";
  if (!canOpenDoor()) {
    html += "disabled";
  }
  html += R"rawliteral(>Открыть</button>
      </form>
    </div>
    <form action="/stop" method="post">
      <button id="door-stop" class="stop" type="submit" )rawliteral";
  if (!canStopDoor()) {
    html += "disabled";
  }
  html += R"rawliteral(>Стоп</button>
    </form>
  </div>
  <script>
    async function refreshStatus() {
      try {
        const response = await fetch('/status', { cache: 'no-store' });
        if (!response.ok) return;
        const status = await response.json();
        document.getElementById('direction-text').textContent = status.direction;
        document.getElementById('door-text').textContent = status.door;
        document.getElementById('closed-limit-text').textContent = status.closedLimit;
        document.querySelector('.time-display').textContent = status.time;
        document.getElementById('min-plus').disabled = !status.canAdjustTime;
        document.getElementById('sec-plus').disabled = !status.canAdjustTime;
        document.getElementById('min-minus').disabled = !status.canAdjustTime;
        document.getElementById('sec-minus').disabled = !status.canAdjustTime;
        document.getElementById('power-on').disabled = !status.canPowerOn;
        document.getElementById('power-off').disabled = !status.canPowerOff;
        document.getElementById('door-open').disabled = !status.canOpenDoor;
        document.getElementById('door-close').disabled = !status.canCloseDoor;
        document.getElementById('door-stop').disabled = !status.canStopDoor;
      } catch (error) {
      }
    }

    setInterval(refreshStatus, 300);
    refreshStatus();
  </script>
)rawliteral";

  html += R"rawliteral(
  <script>
    if (window.history.replaceState) {
      window.history.replaceState(null, '', '/');
    }
  </script>
</body>
</html>
)rawliteral";

  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", generateHTML());
}

void handleStatus() {
  String json = "{";
  json += "\"time\":\"" + formatTimeMMSS() + "\",";
  json += "\"direction\":\"" + String(directionText()) + "\",";
  json += "\"door\":\"" + String(doorText()) + "\",";
  json += "\"closedLimit\":\"" + String(closedLimitText()) + "\",";
  json += "\"canAdjustTime\":" + String(canAdjustTime() ? "true" : "false") + ",";
  json += "\"canPowerOn\":" + String(canPowerOn() ? "true" : "false") + ",";
  json += "\"canPowerOff\":" + String(canPowerOff() ? "true" : "false") + ",";
  json += "\"canOpenDoor\":" + String(canOpenDoor() ? "true" : "false") + ",";
  json += "\"canCloseDoor\":" + String(canCloseDoor() ? "true" : "false") + ",";
  json += "\"canStopDoor\":" + String(canStopDoor() ? "true" : "false");
  json += "}";
  server.send(200, "application/json; charset=UTF-8", json);
}

void handleOpen() {
  if (powerOn) {
    handleRoot();
    return;
  }
  if (di6Active || doorIsOpen) {
    stopMotor();
    doorIsOpen = true;
    doorIsClosed = false;
    handleRoot();
    return;
  }
  setMotorDirectionSafe(MOTOR_OPEN);
  doorIsOpen = false;
  doorIsClosed = false;
  handleRoot();
}

void handleClose() {
  if (powerOn) {
    handleRoot();
    return;
  }
  if (di5Active || doorIsClosed) {
    stopMotor();
    doorIsOpen = false;
    doorIsClosed = true;
    handleRoot();
    return;
  }
  setMotorDirectionSafe(MOTOR_CLOSE);
  doorIsOpen = false;
  doorIsClosed = false;
  handleRoot();
}

void handleStop() {
  if (powerOn) {
    handleRoot();
    return;
  }
  stopMotor();
  handleRoot();
}

void handlePowerOn() {
  setPowerOn();
  handleRoot();
}

void handlePowerOff() {
  setPowerOff();
  handleRoot();
}

void handleTimeAdjust() {
  if (!powerOn && server.hasArg("unit") && server.hasArg("delta")) {
    adjustTime(server.arg("unit"), server.arg("delta").toInt());
  }
  handleRoot();
}

void handleRedirectToRoot() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);

  WiFi.softAP(ap_ssid, ap_password);
  delay(200);

  Wire.begin(I2C_SDA, I2C_SCL);
  initInputExpander();
  stopMotor();
  setPowerOff();
  ensureDoorClosedOnStartup();

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/close", HTTP_POST, handleClose);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/power_on", HTTP_POST, handlePowerOn);
  server.on("/power_off", HTTP_POST, handlePowerOff);
  server.on("/time_adjust", HTTP_POST, handleTimeAdjust);
  server.on("/status", HTTP_GET, handleStatus);

  // Captive portal compatibility endpoints.
  server.on("/generate_204", HTTP_GET, handleRedirectToRoot);      // Android
  server.on("/hotspot-detect.html", HTTP_GET, handleRedirectToRoot); // Apple
  server.on("/fwlink", HTTP_GET, handleRedirectToRoot);             // Microsoft
  server.onNotFound(handleRedirectToRoot);

  server.begin();

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  dnsServer.processNextRequest();
  updateInputStates();
  updateDoorStateFromLimitSwitches();

  if (di6Active && currentDirection == MOTOR_OPEN) {
    stopMotor();
    updateDoorStateFromLimitSwitches();
  }

  if (di5Active && currentDirection == MOTOR_CLOSE) {
    stopMotor();
    updateDoorStateFromLimitSwitches();
  }

  if (powerOn && shuntStartMs != 0 && (millis() - shuntStartMs) >= SHUNT_DELAY_MS) {
    relayStates[RELAY_SHUNT] = true;
    writeRelays();
    shuntStartMs = 0;
  }

  if (powerOn && countdownTickMs != 0) {
    uint32_t now = millis();
    if ((now - countdownTickMs) >= 1000) {
      countdownTickMs += 1000;
      if (remainingSeconds > 0) {
        remainingSeconds--;
      }
      if (remainingSeconds == 0) {
        setPowerOff();
      }
    }
  }

  server.handleClient();
}
