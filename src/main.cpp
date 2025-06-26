#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsClient.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

WebSocketsClient webSocket;
AsyncWebServer server(80);

// forward declaration
void processJson(String jsonStr);

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] Connected");
      webSocket.sendTXT("ESP8266 connected");
      break;
    case WStype_TEXT:
      Serial.printf("[WS] Received: %s\n", payload);
      processJson(String((char*)payload));  // show on screen
      break;
  }
}

void connectWebSocket() {
  // Replace with your server IP/domain and port
  const char* host = "192.168.198.155";
  const uint16_t port = 8765;

  webSocket.begin(host, port, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000); // Try to reconnect every 5s
}


/*
  Create Access Point (AP) mode for WiFi setup
  This function sets up an access point with the SSID "ESP-Setup"
*/
void startAPMode() {
  WiFi.softAP("ESP-Setup");

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", R"rawliteral(
      <h2>WiFi Setup</h2>
      <form action="/save" method="POST">
        SSID:<br><input type="text" name="ssid"><br>
        Password:<br><input type="password" name="password"><br><br>
        <input type="submit" value="Save">
      </form>
    )rawliteral");
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
      request->send(400, "text/plain", "Missing parameters");
      return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String password = request->getParam("password", true)->value();

    StaticJsonDocument<256> doc;
    doc["ssid"] = ssid;
    doc["password"] = password;

    File file = LittleFS.open("/wifi.json", "w");
    if (!file) {
      request->send(500, "text/plain", "Failed to save WiFi credentials.");
      return;
    }

    serializeJson(doc, file);
    file.close();

    request->send(200, "text/plain", "WiFi credentials saved. Rebooting...");
    delay(3000);
    ESP.restart();
  });

  server.begin();
}

/*
 *Function to connect to WiFi using credentials stored in LittleFS
 *The credentials are stored in a JSON file named "wifi.json" in the LittleFS filesystem
 *
 *requires the file to have the following structure:
 *  { "ssid": "your_ssid", 
 *    "password": "your_password" 
 *  }
 *returns true if connected successfully, false otherwise
*/
bool connectToWifi() {
  if (!LittleFS.begin()) {
    Serial.println(F("Failed to mount LittleFS"));
    return false;
  }

  File file = LittleFS.open("/wifi.json", "r");
  if (!file) {
    Serial.println(F("Failed to open wifi.json"));
    return false;
  }
  StaticJsonDocument<256> doc;  
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.print(F("JSON Parse Error: "));
    file.close();
    return false;
  }

  const char* ssid = doc["ssid"];
  const char* password = doc["password"];

  Serial.printf("Connecting to %s...",ssid);
  WiFi.begin(ssid, password);

  int attemps = 0;

  while (WiFi.status() != WL_CONNECTED && attemps < 20) {
    delay(500);
    Serial.print(".");
    attemps++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("Connected!"));
    Serial.print(F("IP Address: "));
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println(F("Failed to connect to WiFi"));
    return false;
  }
}

/* 
    Automatic font size according to the number of characters in the message
    21 characters per line and 8 lines total at size 1
*/
int pickBestFontSize(String text) {
  const int screenW = 128;
  const int screenH = 64;

  for (int size = 4; size >= 1; size--) {
    // each character is 6 pixels wide and 8 pixels tall at size 1
    int charW = 6 * size;
    int charH = 8 * size;

    int charsPerLine = screenW / charW;
    int linesPerScreen = screenH / charH;
    int maxChars = charsPerLine * linesPerScreen;

    if (text.length() <= maxChars) {
      return size;
    }
  }

  return 1; // default to smallest if nothing fits
}



/*
  Function to process incoming JSON messages from the WebSocket
  It expects a JSON object with the following structure:
  {
    "size": <int>, // text size (1-4)
    "pos": [<x>, <y>], // cursor position
    "text": "<string>" // text to display
  }
*/
void processJson(String jsonStr) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error) {
    Serial.print(F("JSON Parse Error: "));
    Serial.println(error.c_str());
    return;
  }

  if (doc.containsKey("size")) {
    display.setTextSize(doc["size"]);
  }

  if (doc.containsKey("pos") && doc["pos"].is<JsonArray>()) {
    int x = doc["pos"][0];
    int y = doc["pos"][1];
    display.setCursor(x, y);
  }

  if (doc.containsKey("text")) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.println(doc["text"].as<String>());
    display.display();
  }
}

void setup() {
  Wire.begin(D2, D1);
  Serial.begin(115200);
  while (!Serial) delay(10);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 init failed"));
    while (true);
  }

  if (!LittleFS.begin()) {
    Serial.println(F("Failed to mount LittleFS"));
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Connecting to WiFi...");
  display.display();

  if (!connectToWifi()) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Starting AP Mode");
    display.display();
    startAPMode();
  } else {
    connectWebSocket();
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi Connected!");
    display.display();
  }
}

void loop() {
  webSocket.loop();
}
