#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>
#include <TMC2209.h>

#define RX_PIN 13
#define TX_PIN 12

// --- I2C Relays --- //
#define I2C_SDA 4
#define I2C_SCL 15
#define PCF8574_ADDR 0x24

// --- WiFi & Web --- //
const char* ap_ssid = "awh";
const char* ap_password = "clickclick";


bool relayStates[6] = {false, false, false, false, false, false};

// --- TMC2209 Stepper --- //
HardwareSerial& serial_stream = Serial1;
const long SERIAL_BAUD_RATE = 115200;

const int32_t RUN_VELOCITY = 20000;
const int32_t STOP_VELOCITY = 0;
const int RUN_DURATION = 1000; // ms
const uint8_t RUN_CURRENT_PERCENT = 100;

TMC2209 stepper_driver;

WebServer server(80);

// --- Relays --- //
void writeRelays() {
  uint8_t value = 0xFF;
  for (int i = 0; i < 6; i++) {
    if (relayStates[i]) bitClear(value, i);
  }
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(value);
  Wire.endTransmission();
}

// --- Web UI --- //
String generateHTML() {
  String html = R"rawliteral(
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <title>Управление реле и шаговым двигателем</title>
      <style>
        body { font-family: sans-serif; padding: 20px; background: #f8f8f8; }
        h2 { margin-top: 20px; }
        button {
          width: 120px; height: 60px; margin: 10px 8px 16px 0;
          font-size: 22px; color: #fff; border: none; cursor: pointer;
          border-radius: 10px; box-shadow: 0 2px 8px #aaa2;
          transition: background 0.2s;
        }
        .relay { background-color: #4CAF50;}
        .off   { background-color: #f44336;}
        .dir   { background-color: #2196F3;}
        .panel { max-width: 480px; margin: 0 auto; background: #fff; padding: 24px 18px 32px 18px; border-radius: 16px; box-shadow: 0 2px 12px #aaa3;}
        @media (max-width: 600px) {
          button { width: 50%; min-width: 120px; height: 40px; font-size: 20px; margin: 4px 0;}
          .panel { padding: 10px; }
        }
      </style>
    </head>
    <body>
    <div class="panel">
      <h2>Панель управления реле</h2>
  )rawliteral";

  for (int i = 0; i < 6; i++) {
    html += "Реле " + String(i + 1) + ": ";
    html += "<a href=\"/toggle?r=" + String(i) + "\">";
    html += "<button class='" + String(relayStates[i] ? "relay" : "off") + "'>";
    html += relayStates[i] ? "ВКЛ" : "ВЫКЛ";
    html += "</button></a><br>";
  }

  html += R"rawliteral(
      <hr>
      <h2>Управление шаговым двигателем</h2>
      <div style="display:flex;gap:20px;justify-content:center;flex-wrap:wrap;">
        <form action="/step" method="get">
          <button class="dir" name="dir" value="left">&#8592; Влево</button>
        </form>
        <form action="/step" method="get">
          <button class="dir" name="dir" value="right">Вправо &#8594;</button>
        </form>
      </div>
      <br>
      <a href="/">Обновить</a>
    </div>
    </body>
    </html>
  )rawliteral";
  return html;
}

// --- Web Handlers --- //
void handleRoot() {
  server.send(200, "text/html; charset=UTF-8", generateHTML());
}

void handleToggle() {
  if (server.hasArg("r")) {
    int r = server.arg("r").toInt();
    if (r >= 0 && r < 6) {
      relayStates[r] = !relayStates[r];
      writeRelays();
    }
  }
  handleRoot();
}

void handleStep() {
  String dir = server.arg("dir");
  if (dir == "left") {
    stepper_driver.disableInverseMotorDirection(); // стандартное направление
    stepper_driver.moveAtVelocity(RUN_VELOCITY);
    delay(RUN_DURATION);
    stepper_driver.moveAtVelocity(STOP_VELOCITY);
  } else if (dir == "right") {
    stepper_driver.enableInverseMotorDirection();
    stepper_driver.moveAtVelocity(RUN_VELOCITY);
    delay(RUN_DURATION);
    stepper_driver.moveAtVelocity(STOP_VELOCITY);
  }
  handleRoot();
}

void setup() {
  Serial.begin(115200);

  // --- WiFi ---
  WiFi.softAP(ap_ssid, ap_password);
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Access Point IP: ");
  Serial.println(IP);

  // --- I2C Relays ---
  Wire.begin(I2C_SDA, I2C_SCL);
  writeRelays();

  stepper_driver.setup(
    serial_stream,
    SERIAL_BAUD_RATE,
    TMC2209::SERIAL_ADDRESS_0,
    RX_PIN,
    TX_PIN
  );
  stepper_driver.setRunCurrent(RUN_CURRENT_PERCENT);
  stepper_driver.enableCoolStep();
  stepper_driver.enable();

  // --- Web Handlers ---
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/step", handleStep);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  server.handleClient();
}
