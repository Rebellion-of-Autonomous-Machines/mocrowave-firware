#include <WiFi.h>
#include <Wire.h>
#include <WebServer.h>

// Название сети и пароль для точки доступа
const char* ap_ssid = "awn";
const char* ap_password = "clickclick";

// Пины I2C и адрес PCF8574
#define I2C_SDA 4
#define I2C_SCL 15
#define PCF8574_ADDR 0x24

WebServer server(80);
bool relayStates[6] = {false, false, false, false, false, false};

// Отправка состояния реле на PCF8574
void writeRelays() {
  uint8_t value = 0xFF;  // все пины HIGH (выключено)
  for (int i = 0; i < 6; i++) {
    if (relayStates[i]) {
      bitClear(value, i);  // активный LOW (включено)
    }
  }
  Wire.beginTransmission(PCF8574_ADDR);
  Wire.write(value);
  Wire.endTransmission();
}

// Генерация HTML страницы
String generateHTML() {
  String html = R"rawliteral(
    <html><head><title>Relay Control</title>
    <style>
      body { font-family: sans-serif; padding: 20px; }
      button {
        width: 80px; height: 40px; margin: 6px;
        font-size: 16px; color: white; border: none;
        cursor: pointer;
      }
    </style></head><body>
    <h2>Relay Control Panel</h2>
  )rawliteral";

  for (int i = 0; i < 6; i++) {
    html += "Relay " + String(i + 1) + ": ";
    html += "<a href=\"/toggle?r=" + String(i) + "\">";
    html += "<button style='background-color:" + String(relayStates[i] ? "#4CAF50" : "#f44336") + "'>";
    html += relayStates[i] ? "ON" : "OFF";
    html += "</button></a><br>";
  }

  html += "</body></html>";
  return html;
}

// Обработка главной страницы
void handleRoot() {
  server.send(200, "text/html", generateHTML());
}

// Обработка переключения реле
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

void setup() {
  Serial.begin(115200);

  // Настройка точки доступа
  WiFi.softAP(ap_ssid, ap_password);
  delay(100);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("Access Point IP: ");
  Serial.println(IP);

  // Настройка I2C и начального состояния реле
  Wire.begin(I2C_SDA, I2C_SCL);
  writeRelays();

  // Настройка маршрутов
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  server.handleClient();
}