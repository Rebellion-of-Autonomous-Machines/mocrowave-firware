#include <WiFi.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Wire.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// --- I2C relays ---
#define I2C_SDA 4
#define I2C_SCL 15
#define PCF8574_RELAY 0x24
#define PCF8574_INPUT 0x22

// --- WiFi AP ---
const char* ap_ssid = "microwave";
const char* ap_password = "clickclick";

// --- Ethernet / Modbus TCP (W5500) ---
const uint8_t W5500_CS_PIN = 5;
const uint16_t MODBUS_TCP_PORT = 502;
const uint8_t MODBUS_UNIT_ID = 1;
byte ethernetMac[] = {0x02, 0xA0, 0xC9, 0x00, 0x00, 0x61};
IPAddress ethernetIp(192, 168, 1, 50);
IPAddress ethernetDns(192, 168, 1, 1);
IPAddress ethernetGateway(192, 168, 1, 1);
IPAddress ethernetSubnet(255, 255, 255, 0);
bool ethernetUseDhcp = false;

// relayStates index is 0-based, so relays 1..6 are indexes 0..5.
const uint8_t RELAY_PHASE = 0; // Physical relay 1
const uint8_t RELAY_NEUTRAL = 1; // Physical relay 2
const uint8_t RELAY_SHUNT = 2; // Physical relay 3

const uint32_t SHUNT_DELAY_MS = 200;
const uint32_t STARTUP_CLOSE_TIMEOUT_MS = 15000;
const uint32_t DOOR_OPEN_DURATION_MS = 6000;
const uint8_t RELAY_MOTOR_A = 4; // Physical relay 5
const uint8_t RELAY_MOTOR_B = 5; // Physical relay 6
const uint8_t DI5_BIT = 4; // P4 on input PCF8574 (IN_D5 on your schematic)
const bool DI5_ACTIVE_LOW = true;

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
// Arduino-ESP32 requires Server::begin(uint16_t), while Ethernet 2.0.2 only
// provides begin(). This adapter supplies the missing compatibility overload.
class Esp32EthernetServer : public EthernetServer {
 public:
  explicit Esp32EthernetServer(uint16_t port) : EthernetServer(port) {}

  void begin(uint16_t port = 0) override {
    (void)port;
    EthernetServer::begin();
  }
};

Esp32EthernetServer modbusServer(MODBUS_TCP_PORT);
EthernetClient modbusClient;
const uint16_t DNS_PORT = 53;
String networkSettingsMessage;
bool networkSettingsSaved = false;
bool startupDoorClosing = true;

bool powerOn = false;
uint32_t shuntStartMs = 0;
uint16_t remainingSeconds = 0;
uint32_t countdownTickMs = 0;
uint16_t cycleStartSeconds = 0;
uint32_t doorOpenStartedMs = 0;
bool di5Active = false;

enum ModbusReg : uint16_t {
  REG_COMMAND = 0,
  REG_SET_TIME_SECONDS = 1,
  REG_REMAINING_TIME_SECONDS = 2,
  REG_POWER_STATE = 3,
  REG_DOOR_STATE = 4,
  REG_LIMIT_SWITCH_STATE = 5,
  REG_MOTOR_DIRECTION = 6,
  REG_PERMISSION_STATE = 7,
  REG_COMMAND_RESULT = 8,
  REG_ERROR_CODE = 9,
  REG_OPEN_REMAINING_SECONDS = 10,
  REG_REG_COUNT = 32
};

enum ModbusCommand : uint16_t {
  CMD_NONE = 0,
  CMD_OPEN_DOOR = 1,
  CMD_CLOSE_DOOR = 2,
  CMD_STOP_DOOR = 3,
  CMD_POWER_ON = 4,
  CMD_POWER_OFF = 5,
  CMD_CLEAR_ERROR = 6
};

enum ModbusCommandResult : uint16_t {
  CMD_RESULT_OK = 0,
  CMD_RESULT_UNKNOWN_COMMAND = 1,
  CMD_RESULT_BUSY = 2,
  CMD_RESULT_NOT_ALLOWED = 3,
  CMD_RESULT_INVALID_VALUE = 4
};

enum ModbusErrorCode : uint16_t {
  ERROR_NONE = 0,
  ERROR_DOOR_STARTUP_CLOSE_TIMEOUT = 1,
  ERROR_POWER_ON_DOOR_NOT_CLOSED = 2,
  ERROR_POWER_ON_ZERO_TIME = 3,
  ERROR_DOOR_COMMAND_WHILE_POWER_ON = 4
};

uint16_t modbusRegs[REG_REG_COUNT] = {0};
uint16_t commandResult = CMD_RESULT_OK;
uint16_t errorCode = ERROR_NONE;

bool parseIPv4(const String& text, IPAddress& address) {
  String value = text;
  value.trim();
  return address.fromString(value);
}

void loadNetworkSettings() {
  Preferences preferences;
  if (!preferences.begin("ethernet", true)) {
    return;
  }

  IPAddress address;
  if (parseIPv4(preferences.getString("ip", ethernetIp.toString()), address)) {
    ethernetIp = address;
  }
  if (parseIPv4(preferences.getString("subnet", ethernetSubnet.toString()), address)) {
    ethernetSubnet = address;
  }
  if (parseIPv4(preferences.getString("gateway", ethernetGateway.toString()), address)) {
    ethernetGateway = address;
  }
  if (parseIPv4(preferences.getString("dns", ethernetDns.toString()), address)) {
    ethernetDns = address;
  }
  ethernetUseDhcp = preferences.getBool("dhcp", false);
  preferences.end();
}

bool saveNetworkSettings() {
  Preferences preferences;
  if (!preferences.begin("ethernet", false)) {
    return false;
  }

  bool saved = preferences.putString("ip", ethernetIp.toString()) > 0;
  saved &= preferences.putString("subnet", ethernetSubnet.toString()) > 0;
  saved &= preferences.putString("gateway", ethernetGateway.toString()) > 0;
  saved &= preferences.putString("dns", ethernetDns.toString()) > 0;
  saved &= preferences.putBool("dhcp", ethernetUseDhcp) > 0;
  preferences.end();
  return saved;
}

void applyEthernetSettings() {
  if (modbusClient) {
    modbusClient.stop();
  }
  if (ethernetUseDhcp) {
    Ethernet.begin(ethernetMac, 10000, 2000);
  } else {
    Ethernet.begin(ethernetMac, ethernetIp, ethernetDns, ethernetGateway, ethernetSubnet);
  }
  delay(200);
  modbusServer.begin();
}

const char* ethernetConnectionText() {
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    return "W5500 не обнаружен";
  }
  EthernetLinkStatus link = Ethernet.linkStatus();
  if (link == LinkOFF) {
    return "Сетевой кабель отключен";
  }
  if (link == Unknown) {
    return "Состояние линии неизвестно";
  }
  if (Ethernet.localIP() == IPAddress(0, 0, 0, 0)) {
    return ethernetUseDhcp ? "Кабель подключен, адрес DHCP не получен" : "Кабель подключен, IP не назначен";
  }
  return "Подключено";
}

String modbusClientText() {
  if (!modbusClient || !modbusClient.connected()) {
    return "Нет подключения";
  }
  return modbusClient.remoteIP().toString() + ":" + String(modbusClient.remotePort());
}

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
  doorOpenStartedMs = 0;
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
  return !startupDoorClosing && !powerOn;
}

bool canPowerOn() {
  return !startupDoorClosing && !powerOn && remainingSeconds > 0 && doorIsClosed;
}

bool canPowerOff() {
  return !startupDoorClosing && powerOn;
}

bool canOpenDoor() {
  return !startupDoorClosing && !powerOn && currentDirection != MOTOR_OPEN && !doorIsOpen;
}

bool canCloseDoor() {
  return !startupDoorClosing && !powerOn && currentDirection != MOTOR_CLOSE && !di5Active && !doorIsClosed;
}

bool canStopDoor() {
  return !startupDoorClosing && !powerOn && currentDirection != MOTOR_STOP && !di5Active && !doorIsOpen && !doorIsClosed;
}

void updateInputStates() {
  Wire.requestFrom((uint8_t)PCF8574_INPUT, (uint8_t)1);
  if (Wire.available() < 1) {
    return;
  }

  uint8_t inputByte = Wire.read();
  bool di5BitState = bitRead(inputByte, DI5_BIT);
  di5Active = DI5_ACTIVE_LOW ? !di5BitState : di5BitState;
}

void updateDoorStateFromLimitSwitches() {
  if (di5Active) {
    doorIsClosed = true;
    doorIsOpen = false;
    return;
  }

  if (currentDirection == MOTOR_OPEN || currentDirection == MOTOR_CLOSE) {
    doorIsOpen = false;
    doorIsClosed = false;
  }
}

void ensureDoorClosedOnStartup() {
  startupDoorClosing = true;
  updateInputStates();
  updateDoorStateFromLimitSwitches();

  if (doorIsClosed) {
    startupDoorClosing = false;
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
  if (!doorIsClosed) {
    errorCode = ERROR_DOOR_STARTUP_CLOSE_TIMEOUT;
  }
  startupDoorClosing = false;
}

uint16_t powerState() {
  if (!powerOn) {
    return 0;
  }
  return shuntStartMs == 0 ? 2 : 1;
}

uint16_t doorState() {
  if (currentDirection == MOTOR_OPEN) {
    return 3;
  }
  if (currentDirection == MOTOR_CLOSE) {
    return 4;
  }
  if (doorIsOpen) {
    return 1;
  }
  if (doorIsClosed) {
    return 2;
  }
  return 0;
}

uint16_t permissionState() {
  uint16_t bits = 0;
  if (canPowerOn()) {
    bitSet(bits, 0);
  }
  if (canOpenDoor()) {
    bitSet(bits, 1);
  }
  if (canCloseDoor()) {
    bitSet(bits, 2);
  }
  if (canStopDoor()) {
    bitSet(bits, 3);
  }
  if (canAdjustTime()) {
    bitSet(bits, 4);
  }
  if (canPowerOff()) {
    bitSet(bits, 5);
  }
  return bits;
}

uint16_t openRemainingSeconds() {
  if (currentDirection != MOTOR_OPEN || doorOpenStartedMs == 0) {
    return 0;
  }

  uint32_t elapsed = millis() - doorOpenStartedMs;
  if (elapsed >= DOOR_OPEN_DURATION_MS) {
    return 0;
  }
  return (uint16_t)((DOOR_OPEN_DURATION_MS - elapsed + 999) / 1000);
}

void syncModbusRegistersFromState() {
  modbusRegs[REG_COMMAND] = CMD_NONE;
  modbusRegs[REG_SET_TIME_SECONDS] = cycleStartSeconds > 0 ? cycleStartSeconds : remainingSeconds;
  modbusRegs[REG_REMAINING_TIME_SECONDS] = remainingSeconds;
  modbusRegs[REG_POWER_STATE] = powerState();
  modbusRegs[REG_DOOR_STATE] = doorState();
  modbusRegs[REG_LIMIT_SWITCH_STATE] = di5Active ? 1 : 0;
  modbusRegs[REG_MOTOR_DIRECTION] = (uint16_t)currentDirection;
  modbusRegs[REG_PERMISSION_STATE] = permissionState();
  modbusRegs[REG_COMMAND_RESULT] = commandResult;
  modbusRegs[REG_ERROR_CODE] = errorCode;
  modbusRegs[REG_OPEN_REMAINING_SECONDS] = openRemainingSeconds();
}

void executeModbusCommand(uint16_t cmd) {
  commandResult = CMD_RESULT_OK;

  if (startupDoorClosing) {
    commandResult = CMD_RESULT_BUSY;
    syncModbusRegistersFromState();
    return;
  }

  switch (cmd) {
    case CMD_NONE:
      break;
    case CMD_OPEN_DOOR:
      if (powerOn) {
        commandResult = CMD_RESULT_BUSY;
        errorCode = ERROR_DOOR_COMMAND_WHILE_POWER_ON;
      } else if (!canOpenDoor()) {
        commandResult = CMD_RESULT_NOT_ALLOWED;
      } else {
        setMotorDirectionSafe(MOTOR_OPEN);
        doorOpenStartedMs = millis();
        doorIsOpen = false;
        doorIsClosed = false;
      }
      break;
    case CMD_CLOSE_DOOR:
      if (powerOn) {
        commandResult = CMD_RESULT_BUSY;
        errorCode = ERROR_DOOR_COMMAND_WHILE_POWER_ON;
      } else if (!canCloseDoor()) {
        commandResult = CMD_RESULT_NOT_ALLOWED;
      } else {
        setMotorDirectionSafe(MOTOR_CLOSE);
        doorIsOpen = false;
        doorIsClosed = false;
      }
      break;
    case CMD_STOP_DOOR:
      if (powerOn) {
        commandResult = CMD_RESULT_BUSY;
        errorCode = ERROR_DOOR_COMMAND_WHILE_POWER_ON;
      } else if (!canStopDoor()) {
        commandResult = CMD_RESULT_NOT_ALLOWED;
      } else {
        stopMotor();
      }
      break;
    case CMD_POWER_ON:
      if (remainingSeconds == 0) {
        commandResult = CMD_RESULT_INVALID_VALUE;
        errorCode = ERROR_POWER_ON_ZERO_TIME;
      } else if (!doorIsClosed) {
        commandResult = CMD_RESULT_NOT_ALLOWED;
        errorCode = ERROR_POWER_ON_DOOR_NOT_CLOSED;
      } else if (powerOn) {
        commandResult = CMD_RESULT_BUSY;
      } else {
        setPowerOn();
      }
      break;
    case CMD_POWER_OFF:
      if (!canPowerOff()) {
        commandResult = CMD_RESULT_NOT_ALLOWED;
      } else {
        setPowerOff();
      }
      break;
    case CMD_CLEAR_ERROR:
      errorCode = ERROR_NONE;
      commandResult = CMD_RESULT_OK;
      break;
    default:
      commandResult = CMD_RESULT_UNKNOWN_COMMAND;
      break;
  }

  syncModbusRegistersFromState();
}

bool writeModbusRegister(uint16_t address, uint16_t value) {
  if (address >= REG_REG_COUNT) {
    return false;
  }

  if (startupDoorClosing) {
    commandResult = CMD_RESULT_BUSY;
    syncModbusRegistersFromState();
    return true;
  }

  if (address == REG_COMMAND) {
    executeModbusCommand(value);
    return true;
  }

  if (address == REG_SET_TIME_SECONDS) {
    if (powerOn) {
      commandResult = CMD_RESULT_BUSY;
      syncModbusRegistersFromState();
      return true;
    }
    if (value > 5999) {
      commandResult = CMD_RESULT_INVALID_VALUE;
      syncModbusRegistersFromState();
      return true;
    }
    remainingSeconds = value;
    cycleStartSeconds = value;
    commandResult = CMD_RESULT_OK;
    syncModbusRegistersFromState();
    return true;
  }

  return false;
}

void sendModbusTcpResponse(EthernetClient& client, const uint8_t* requestHeader, const uint8_t* pdu, uint16_t pduLen) {
  uint8_t header[7];
  header[0] = requestHeader[0];
  header[1] = requestHeader[1];
  header[2] = 0;
  header[3] = 0;
  uint16_t length = pduLen + 1;
  header[4] = highByte(length);
  header[5] = lowByte(length);
  header[6] = requestHeader[6];
  client.write(header, sizeof(header));
  client.write(pdu, pduLen);
}

void sendModbusException(EthernetClient& client, const uint8_t* requestHeader, uint8_t functionCode, uint8_t exceptionCode) {
  uint8_t pdu[2] = {(uint8_t)(functionCode | 0x80), exceptionCode};
  sendModbusTcpResponse(client, requestHeader, pdu, sizeof(pdu));
}

bool waitForClientBytes(EthernetClient& client, int count, uint16_t timeoutMs = 50) {
  uint32_t started = millis();
  while (client.connected() && client.available() < count && (millis() - started) < timeoutMs) {
    delay(1);
  }
  return client.available() >= count;
}

void processModbusTcpRequest(EthernetClient& client) {
  if (!waitForClientBytes(client, 7)) {
    return;
  }

  uint8_t header[7];
  client.read(header, sizeof(header));
  uint16_t protocolId = word(header[2], header[3]);
  uint16_t length = word(header[4], header[5]);
  if (protocolId != 0 || length < 2 || length > 253) {
    client.stop();
    return;
  }

  uint16_t pduLen = length - 1;
  if (!waitForClientBytes(client, pduLen)) {
    client.stop();
    return;
  }

  uint8_t pdu[253];
  client.read(pdu, pduLen);
  uint8_t functionCode = pdu[0];
  if (header[6] != MODBUS_UNIT_ID && header[6] != 0) {
    return;
  }

  if (startupDoorClosing && (functionCode == 0x06 || functionCode == 0x10)) {
    sendModbusException(client, header, functionCode, 0x06); // Server Device Busy
    return;
  }

  syncModbusRegistersFromState();

  if (functionCode == 0x03) {
    if (pduLen < 5) {
      sendModbusException(client, header, functionCode, 0x03);
      return;
    }
    uint16_t startAddress = word(pdu[1], pdu[2]);
    uint16_t quantity = word(pdu[3], pdu[4]);
    if (quantity == 0 || quantity > 60 || (startAddress + quantity) > REG_REG_COUNT) {
      sendModbusException(client, header, functionCode, 0x02);
      return;
    }

    uint8_t response[125];
    response[0] = functionCode;
    response[1] = quantity * 2;
    for (uint16_t i = 0; i < quantity; i++) {
      uint16_t value = modbusRegs[startAddress + i];
      response[2 + (i * 2)] = highByte(value);
      response[3 + (i * 2)] = lowByte(value);
    }
    sendModbusTcpResponse(client, header, response, 2 + (quantity * 2));
    return;
  }

  if (functionCode == 0x06) {
    if (pduLen < 5) {
      sendModbusException(client, header, functionCode, 0x03);
      return;
    }
    uint16_t address = word(pdu[1], pdu[2]);
    uint16_t value = word(pdu[3], pdu[4]);
    if (!writeModbusRegister(address, value)) {
      sendModbusException(client, header, functionCode, 0x02);
      return;
    }
    sendModbusTcpResponse(client, header, pdu, 5);
    return;
  }

  if (functionCode == 0x10) {
    if (pduLen < 6) {
      sendModbusException(client, header, functionCode, 0x03);
      return;
    }
    uint16_t startAddress = word(pdu[1], pdu[2]);
    uint16_t quantity = word(pdu[3], pdu[4]);
    uint8_t byteCount = pdu[5];
    if (quantity == 0 || byteCount != quantity * 2 || (startAddress + quantity) > REG_REG_COUNT || pduLen < (uint16_t)(6 + byteCount)) {
      sendModbusException(client, header, functionCode, 0x02);
      return;
    }
    for (uint16_t i = 0; i < quantity; i++) {
      uint16_t value = word(pdu[6 + (i * 2)], pdu[7 + (i * 2)]);
      if (!writeModbusRegister(startAddress + i, value)) {
        sendModbusException(client, header, functionCode, 0x02);
        return;
      }
    }
    uint8_t response[5] = {functionCode, pdu[1], pdu[2], pdu[3], pdu[4]};
    sendModbusTcpResponse(client, header, response, sizeof(response));
    return;
  }

  sendModbusException(client, header, functionCode, 0x01);
}

void handleModbusTcp() {
  if (!modbusClient || !modbusClient.connected()) {
    EthernetClient newClient = modbusServer.available();
    if (newClient) {
      modbusClient = newClient;
    }
  }

  if (modbusClient && modbusClient.connected() && modbusClient.available()) {
    processModbusTcpRequest(modbusClient);
  }
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
    .network {
      margin-top: 20px;
      padding-top: 18px;
      border-top: 1px solid #cbd5e1;
    }
    .network h2 { margin: 0 0 12px; font-size: 19px; }
    .network label {
      display: block;
      margin: 10px 0 5px;
      color: var(--muted);
      font-size: 13px;
      font-weight: 600;
    }
    .network input, .network select {
      width: 100%;
      height: 44px;
      padding: 0 12px;
      border: 1px solid #cbd5e1;
      border-radius: 10px;
      color: var(--txt);
      background: #f8fafc;
      font-size: 16px;
    }
    .network-state {
      margin-bottom: 8px;
      padding: 9px 11px;
      border-radius: 9px;
      background: #f1f5f9;
      color: var(--muted);
      font-size: 13px;
    }
    .network button {
      height: 48px;
      margin-top: 14px;
      background: #0f172a;
      font-size: 16px;
    }
    .notice {
      margin-bottom: 12px;
      padding: 10px 12px;
      border-radius: 10px;
      font-size: 14px;
      font-weight: 600;
    }
    .notice-ok { color: #166534; background: #dcfce7; }
    .notice-error { color: #991b1b; background: #fee2e2; }
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
        <input type="hidden" name="delta" value="10">
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
        <input type="hidden" name="delta" value="-10">
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
    <section class="network">
      <h2>Настройки Ethernet / Modbus TCP</h2>
)rawliteral";

  if (networkSettingsMessage.length() > 0) {
    html += "<div class=\"notice ";
    html += networkSettingsSaved ? "notice-ok" : "notice-error";
    html += "\">" + networkSettingsMessage + "</div>";
  }

  html += R"rawliteral(
      <div class="network-state">Состояние: <b id="ethernet-status">)rawliteral";
  html += ethernetConnectionText();
  html += R"rawliteral(</b></div>
      <div class="network-state">Текущий IP: <b id="ethernet-current-ip">)rawliteral";
  html += Ethernet.localIP().toString();
  html += R"rawliteral(</b></div>
      <div class="network-state">Маска: <b id="ethernet-current-subnet">)rawliteral";
  html += Ethernet.subnetMask().toString();
  html += R"rawliteral(</b></div>
      <div class="network-state">Шлюз: <b id="ethernet-current-gateway">)rawliteral";
  html += Ethernet.gatewayIP().toString();
  html += R"rawliteral(</b></div>
      <div class="network-state">DNS: <b id="ethernet-current-dns">)rawliteral";
  html += Ethernet.dnsServerIP().toString();
  html += R"rawliteral(</b></div>
      <div class="network-state">Клиент Modbus TCP: <b id="modbus-client">)rawliteral";
  html += modbusClientText();
  html += R"rawliteral(</b></div>
      <form action="/network" method="post">
        <label for="network-mode">Получение адреса</label>
        <select id="network-mode" name="mode" onchange="updateNetworkMode()">
          <option value="static")rawliteral";
  if (!ethernetUseDhcp) {
    html += " selected";
  }
  html += R"rawliteral(>Статический IP</option>
          <option value="dhcp")rawliteral";
  if (ethernetUseDhcp) {
    html += " selected";
  }
  html += R"rawliteral(>DHCP</option>
        </select>
        <label for="network-ip">IP-адрес W5500</label>
        <input class="static-network-field" id="network-ip" name="ip" type="text" inputmode="decimal" required value=")rawliteral";
  html += ethernetIp.toString();
  html += R"rawliteral(">
        <label for="network-subnet">Маска подсети</label>
        <input class="static-network-field" id="network-subnet" name="subnet" type="text" inputmode="decimal" required value=")rawliteral";
  html += ethernetSubnet.toString();
  html += R"rawliteral(">
        <label for="network-gateway">Шлюз</label>
        <input class="static-network-field" id="network-gateway" name="gateway" type="text" inputmode="decimal" required value=")rawliteral";
  html += ethernetGateway.toString();
  html += R"rawliteral(">
        <label for="network-dns">DNS-сервер</label>
        <input class="static-network-field" id="network-dns" name="dns" type="text" inputmode="decimal" required value=")rawliteral";
  html += ethernetDns.toString();
  html += R"rawliteral(">
        <button type="submit">Сохранить и применить</button>
      </form>
    </section>
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
        document.getElementById('ethernet-status').textContent = status.ethernetStatus;
        document.getElementById('ethernet-current-ip').textContent = status.ethernetIp;
        document.getElementById('ethernet-current-subnet').textContent = status.ethernetSubnet;
        document.getElementById('ethernet-current-gateway').textContent = status.ethernetGateway;
        document.getElementById('ethernet-current-dns').textContent = status.ethernetDns;
        document.getElementById('modbus-client').textContent = status.modbusClient;
      } catch (error) {
      }
    }

    function updateNetworkMode() {
      const useDhcp = document.getElementById('network-mode').value === 'dhcp';
      document.querySelectorAll('.static-network-field').forEach((field) => {
        field.readOnly = useDhcp;
        field.style.opacity = useDhcp ? '0.55' : '1';
      });
    }

    setInterval(refreshStatus, 300);
    updateNetworkMode();
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
  json += "\"canStopDoor\":" + String(canStopDoor() ? "true" : "false") + ",";
  json += "\"startupClosing\":" + String(startupDoorClosing ? "true" : "false") + ",";
  json += "\"ethernetStatus\":\"" + String(ethernetConnectionText()) + "\",";
  json += "\"ethernetIp\":\"" + Ethernet.localIP().toString() + "\",";
  json += "\"ethernetSubnet\":\"" + Ethernet.subnetMask().toString() + "\",";
  json += "\"ethernetGateway\":\"" + Ethernet.gatewayIP().toString() + "\",";
  json += "\"ethernetDns\":\"" + Ethernet.dnsServerIP().toString() + "\",";
  json += "\"modbusClient\":\"" + modbusClientText() + "\"";
  json += "}";
  server.send(200, "application/json; charset=UTF-8", json);
}

bool rejectWebCommandDuringStartup() {
  if (!startupDoorClosing) {
    return false;
  }
  server.send(423, "text/plain; charset=UTF-8", "Команды заблокированы: выполняется стартовое закрытие двери.");
  return true;
}

void handleOpen() {
  if (rejectWebCommandDuringStartup()) {
    return;
  }
  if (powerOn) {
    handleRoot();
    return;
  }
  if (doorIsOpen) {
    stopMotor();
    doorIsOpen = true;
    doorIsClosed = false;
    handleRoot();
    return;
  }
  setMotorDirectionSafe(MOTOR_OPEN);
  doorOpenStartedMs = millis();
  doorIsOpen = false;
  doorIsClosed = false;
  handleRoot();
}

void handleClose() {
  if (rejectWebCommandDuringStartup()) {
    return;
  }
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
  if (rejectWebCommandDuringStartup()) {
    return;
  }
  if (powerOn) {
    handleRoot();
    return;
  }
  stopMotor();
  handleRoot();
}

void handlePowerOn() {
  if (rejectWebCommandDuringStartup()) {
    return;
  }
  setPowerOn();
  handleRoot();
}

void handlePowerOff() {
  if (rejectWebCommandDuringStartup()) {
    return;
  }
  setPowerOff();
  handleRoot();
}

void handleTimeAdjust() {
  if (rejectWebCommandDuringStartup()) {
    return;
  }
  if (!powerOn && server.hasArg("unit") && server.hasArg("delta")) {
    adjustTime(server.arg("unit"), server.arg("delta").toInt());
  }
  handleRoot();
}

void handleNetworkSettings() {
  if (rejectWebCommandDuringStartup()) {
    return;
  }
  if (powerOn || doorMoving()) {
    networkSettingsSaved = false;
    networkSettingsMessage = "Настройки сети можно менять только при выключенном нагреве и остановленной двери.";
    server.send(409, "text/html; charset=UTF-8", generateHTML());
    return;
  }
  if (!server.hasArg("mode") || (server.arg("mode") != "static" && server.arg("mode") != "dhcp")) {
    networkSettingsSaved = false;
    networkSettingsMessage = "Выберите режим получения IP-адреса.";
    server.send(400, "text/html; charset=UTF-8", generateHTML());
    return;
  }

  bool newUseDhcp = server.arg("mode") == "dhcp";
  IPAddress newIp;
  IPAddress newSubnet;
  IPAddress newGateway;
  IPAddress newDns;
  if (!newUseDhcp) {
    const char* requiredArgs[] = {"ip", "subnet", "gateway", "dns"};
    for (const char* arg : requiredArgs) {
      if (!server.hasArg(arg)) {
        networkSettingsSaved = false;
        networkSettingsMessage = "Заполните все параметры статической сети.";
        server.send(400, "text/html; charset=UTF-8", generateHTML());
        return;
      }
    }
    if (!parseIPv4(server.arg("ip"), newIp) ||
        !parseIPv4(server.arg("subnet"), newSubnet) ||
        !parseIPv4(server.arg("gateway"), newGateway) ||
        !parseIPv4(server.arg("dns"), newDns)) {
      networkSettingsSaved = false;
      networkSettingsMessage = "Ошибка: проверьте формат IPv4-адресов.";
      server.send(400, "text/html; charset=UTF-8", generateHTML());
      return;
    }
  }

  bool previousUseDhcp = ethernetUseDhcp;
  IPAddress previousIp = ethernetIp;
  IPAddress previousSubnet = ethernetSubnet;
  IPAddress previousGateway = ethernetGateway;
  IPAddress previousDns = ethernetDns;
  ethernetUseDhcp = newUseDhcp;
  if (!newUseDhcp) {
    ethernetIp = newIp;
    ethernetSubnet = newSubnet;
    ethernetGateway = newGateway;
    ethernetDns = newDns;
  }

  if (!saveNetworkSettings()) {
    ethernetUseDhcp = previousUseDhcp;
    ethernetIp = previousIp;
    ethernetSubnet = previousSubnet;
    ethernetGateway = previousGateway;
    ethernetDns = previousDns;
    networkSettingsSaved = false;
    networkSettingsMessage = "Не удалось сохранить настройки во flash-памяти.";
    server.send(500, "text/html; charset=UTF-8", generateHTML());
    return;
  }

  applyEthernetSettings();
  networkSettingsSaved = true;
  networkSettingsMessage = "Режим ";
  networkSettingsMessage += ethernetUseDhcp ? "DHCP" : "статического IP";
  networkSettingsMessage += " сохранён. Текущий IP W5500: " + Ethernet.localIP().toString();
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

  SPI.begin();
  Ethernet.init(W5500_CS_PIN);
  loadNetworkSettings();
  applyEthernetSettings();
  syncModbusRegistersFromState();

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/open", HTTP_POST, handleOpen);
  server.on("/close", HTTP_POST, handleClose);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/power_on", HTTP_POST, handlePowerOn);
  server.on("/power_off", HTTP_POST, handlePowerOff);
  server.on("/time_adjust", HTTP_POST, handleTimeAdjust);
  server.on("/network", HTTP_POST, handleNetworkSettings);
  server.on("/status", HTTP_GET, handleStatus);

  // Captive portal compatibility endpoints.
  server.on("/generate_204", HTTP_GET, handleRedirectToRoot);      // Android
  server.on("/hotspot-detect.html", HTTP_GET, handleRedirectToRoot); // Apple
  server.on("/fwlink", HTTP_GET, handleRedirectToRoot);             // Microsoft
  server.onNotFound(handleRedirectToRoot);

  server.begin();

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Ethernet IP: ");
  Serial.println(Ethernet.localIP());
}

void loop() {
  dnsServer.processNextRequest();
  if (ethernetUseDhcp) {
    Ethernet.maintain();
  }
  updateInputStates();
  updateDoorStateFromLimitSwitches();

  if (currentDirection == MOTOR_OPEN && doorOpenStartedMs != 0 && (millis() - doorOpenStartedMs) >= DOOR_OPEN_DURATION_MS) {
    stopMotor();
    doorIsOpen = true;
    doorIsClosed = false;
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

  syncModbusRegistersFromState();
  handleModbusTcp();
  server.handleClient();
}
