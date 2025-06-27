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

#define WIFI_CONNECTION_MAX_ATTEMPTS 100
#define MODE_BUTTON_PIN 14 // D5 on NodeMCU

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#include <FluxGarage_RoboEyes.h>
roboEyes roboEyes(&display);

WebSocketsClient webSocket;
AsyncWebServer server(80);

enum DisplayMode {
  MODE_ROBOT_EYES,
  MODE_MESSAGE,
  MODE_DEBUG
};

DisplayMode currentMode = MODE_ROBOT_EYES;
bool forceMessageMode = false;
bool forceDebugMode = false;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 500;
bool isInAPMode = false;

void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void connectWebSocket();
void startAPMode();
bool connectToWifi();
int pickBestFontSize(String text);
void processJson(String jsonStr, bool forceAndSave = true);
void displayMessageLines(const std::vector<String>& lines, int size = 1, int x = 0, int y = 0);
void loadSavedMessage();
void updateDisplay();

void setup() {
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(D2, D1);
  Serial.begin(115200);
  while (!Serial) delay(10);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 init failed"));
    while (true);
  }
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 60);  // 60 fps
  roboEyes.setAutoblinker(ON, 1, 0.5);               
  roboEyes.setIdleMode(ON, 1.5, 0.5);         
  roboEyes.anim_confused();
  roboEyes.open();


  if (!LittleFS.begin()) {
    Serial.println(F("Failed to mount LittleFS"));
    return;
  }

  if (!connectToWifi()) {
    currentMode = MODE_DEBUG;
    forceDebugMode = false;
    Serial.println("[SETUP] WiFi failed, entering DEBUG mode");
    updateDisplay();
    startAPMode();
  } else {
    connectWebSocket();
    delay(2000);
  }
}

void loop() {
  webSocket.loop();

  if (digitalRead(MODE_BUTTON_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastButtonPress > debounceDelay) {
      lastButtonPress = now;

      currentMode = static_cast<DisplayMode>((currentMode + 1) % 3);
      Serial.print("[BUTTON] Switched to mode: ");
      Serial.println(currentMode);
      updateDisplay();
    }
  }

  if (forceMessageMode && currentMode != MODE_MESSAGE) {
    Serial.println("[LOOP] Forcing MODE_MESSAGE");
    forceMessageMode = false;
    currentMode = MODE_MESSAGE;
    updateDisplay();
  } else if (forceDebugMode) {
      if (currentMode != MODE_DEBUG) {
        Serial.println("[LOOP] Forcing MODE_DEBUG");
        currentMode = MODE_DEBUG;
        updateDisplay();
      }
      // Reset the flag **after first execution**
      forceDebugMode = false;
}

  if (currentMode== MODE_ROBOT_EYES) {
    roboEyes.update();
  }
  
}

/*
  Callback function that handles WebSocket events as conenction,
  disconnectiona nd incoming messsage from server

  Incoming messages are expected to be JSON and are parsed by processJson()
*/
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
      processJson(String((char*)payload));
      break;
  }
}

/*
  Initializes WebSocket client connection to the Flask server
  This function connects to the WebSocket server at the specified host and port
  and sets up the event handler for incoming messages.
  It also sets a reconnection interval to attempt to reconnect if the connection is lost.
*/
void connectWebSocket() {
  const char* host = "192.168.198.155";
  const uint16_t port = 8765;

  webSocket.begin(host, port, "/");
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(5000);
}

/*
  Create Access Point (AP) mode for WiFi setup
  This function sets up an access point with the SSID "ESP-Setup"
  and serves a simple HTML form to input WiFi credentials.
  When the form is submitted, it saves the credentials to LittleFS in a JSON file

  Also displays debug info on the oled screen
*/
void startAPMode() {
  WiFi.softAP("ESP-Setup");
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  isInAPMode = true;

  String visitLine = "Visit: http://" + IP.toString() + "/";
  displayMessageLines({
    "WiFi Setup Mode",
    "SSID: ESP-Setup",
    visitLine,
    "to configure WiFi"
  }, 1);

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

  Function to connect to WiFi using credentials stored in LittleFS
  The credentials are stored in a JSON file named "wifi.json" in the LittleFS filesystem
  
  requires the file to have the following structure:
    { "ssid": "your_ssid", 
      "password": "your_password" 
    }
  returns true if connected successfully, false otherwise
*/
bool connectToWifi() {
  File file = LittleFS.open("/wifi.json", "r");
  if (!file) {
    Serial.println(F("Failed to open wifi.json"));
    return false;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print(F("JSON Parse Error: "));
    Serial.println(error.c_str());
    return false;
  }

  const char* ssid = doc["ssid"];
  const char* password = doc["password"];

  Serial.printf("Connecting to %s...", ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECTION_MAX_ATTEMPTS) {
    delay(33);             // ~60 FPS for OLED
    roboEyes.update();
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\n[WiFi] Failed to connect");
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
    int charW = 6 * size;
    int charH = 8 * size;
    int charsPerLine = screenW / charW;
    int linesPerScreen = screenH / charH;
    int maxChars = charsPerLine * linesPerScreen;

    if (text.length() <= maxChars) {
      return size;
    }
  }
  return 1;
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
void processJson(String jsonStr, bool saveAndForce) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error) {
    Serial.print("[JSON] Parse Error: ");
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

  File file = LittleFS.open("/message.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("[JSON] Saved to /message.json");
  } else {
    Serial.println("[JSON] Failed to save message");
  }

  if (saveAndForce && currentMode != MODE_MESSAGE) {
    forceMessageMode = true;
    Serial.println("[JSON] Forcing MODE_MESSAGE");
  }
}

/*
  Displays multiple lines of text on the OLED display
  This function takes a vector of strings and displays them on the screen.

  Parameters:
  - lines: A vector of strings to display.
  - size: The text size multiplier (default is 1).
  - x: The x-coordinate for the text cursor (default is 0).
  - y: The y-coordinate for the text cursor (default is 0).

  Example usage:
    displayMessageLines({ "Hello", "World" }, 2);
*/
void displayMessageLines(const std::vector<String>& lines, int size, int x, int y) {
  display.clearDisplay();
  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(x, y);
  for (const auto& line : lines) {
    display.println(line);
  }
  display.display();
}

/*
  Function to load a saved message from LittleFS
  This function reads a JSON file named "message.json" from the LittleFS filesystem
  and displays the message on the OLED screen using the same logic as processJson().
*/
void loadSavedMessage() {
  File file = LittleFS.open("/message.json", "r");
  if (!file) {
    Serial.println("[LOAD] No saved message found.");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("[LOAD] Failed to parse saved message.");
    return;
  }

  String jsonStr;
  serializeJson(doc, jsonStr);
  processJson(jsonStr, false);
}

void updateDisplay() {
  Serial.print("[DISPLAY] Updating mode: ");
  Serial.println(currentMode);

  if (currentMode != MODE_ROBOT_EYES) {
    display.clearDisplay(); // Only clear when not in robot mode
  }

  switch (currentMode) {
    case MODE_ROBOT_EYES:
      // displayMessageLines({ "Robot Eyes :)", "Face Mode" }, 2);
      break;

    case MODE_MESSAGE:
      loadSavedMessage();
      break;

    case MODE_DEBUG:
      if (isInAPMode) {
        String visitLine = "Visit: http://" + WiFi.softAPIP().toString() + "/";
        displayMessageLines({
          "WiFi Setup Mode",
          "SSID: ESP-Setup",
          visitLine,
          "to configure WiFi"
        }, 1);
      } else {
        displayMessageLines({
          "WiFi Status: " + String(WiFi.status()),
          "IP: " + WiFi.localIP().toString(),
          "Mode: DEBUG"
        }, 1);
      }
      break;
  }

  
}

