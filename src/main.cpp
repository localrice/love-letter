#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsClient.h>
#include <message_animaiton_frames.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define WIFI_CONNECTION_MAX_ATTEMPTS 150
#define MODE_BUTTON_PIN 14 // D5 on NodeMCU
#define TOUCH_PIN 12 // D6 on NodeMCU

// Display message feature animation settings
#define LOGO_WIDTH 128
#define LOGO_HEIGHT 64
#define MAX_FRAMES 50

const unsigned long frameDurationMs = 250;  // show each frame for 200ms
const unsigned char* animationFrames[MAX_FRAMES]; // pointers to bitmaps
uint8_t totalFrames = 0;  // how many actual frames are loaded
bool isMessageUnread = false;  // Tracks if new message bitmap should be shown


Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#include <FluxGarage_RoboEyes.h>
roboEyes roboEyes;

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

// Mood system
unsigned long lastMoodChange = 0;
unsigned long moodInterval = 15000; // 15 seconds
unsigned long happyUntil = 0;
int currentMood = DEFAULT;
bool isBeingPetted = false;

// forward declarations
void onWebSocketEvent(WStype_t type, uint8_t * payload, size_t length);
void connectWebSocket();
void startAPMode();
bool connectToWifi();
int pickBestFontSize(String text);
void processJson(String jsonStr, bool forceAndSave = true);
void displayMessageLines(const std::vector<String>& lines, int size = 1, int x = 0, int y = 0);
void loadSavedMessage();
void updateDisplay();
void changeMood(int mood);
void showNewMessageLogo();
void playFullAnimation();

void setup() {
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(TOUCH_PIN, INPUT);
  Wire.begin(D2, D1);
  Serial.begin(115200);
  while (!Serial) delay(10);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 init failed"));
    while (true);
  }
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 60);  // 60 fps
  roboEyes.setWidth(30, 30);
  roboEyes.setHeight(30, 30);
  roboEyes.setBorderradius(15, 15);
  roboEyes.setAutoblinker(ON, 1, 0.5);               
  roboEyes.setIdleMode(ON, 1.5, 0.5);   
  // roboEyes.setCuriosity(ON);      
  roboEyes.open();
  roboEyes.anim_confused();

  if (!LittleFS.begin()) {
    Serial.println(F("Failed to mount LittleFS"));
    return;
  }

  if (!connectToWifi()) {
    currentMode = MODE_DEBUG;
    forceDebugMode = false;
    Serial.println("[SETUP] WiFi failed, entering DEBUG mode");

    roboEyes.open();
    roboEyes.setMood(TIRED);
    roboEyes.setIdleMode(ON, 3, 2);

    updateDisplay();
    startAPMode();
  } else {
    Serial.println("[SETUP] WiFi connected, switching to happy face");

    roboEyes.open();
    roboEyes.setMood(HAPPY);
    roboEyes.anim_laugh();
    roboEyes.setIdleMode(ON, 5, 3);

    connectWebSocket();

  }
}

void loop() {
  webSocket.loop();
  if (currentMode == MODE_MESSAGE && digitalRead(TOUCH_PIN) == HIGH) {
    if (isMessageUnread) {
      Serial.println("[TOUCH] Acknowledged. Playing animation before message.");

      isMessageUnread = false;

      // Play animation before showing message
      playFullAnimation();
    
      delay(500);  // brief pause before message appears
      updateDisplay();  // will now load saved message
      delay(500);  // debounce
    }
  }

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

  if (forceMessageMode) {
    forceMessageMode = false;

    if (currentMode != MODE_MESSAGE) {
      Serial.println("[LOOP] Forcing MODE_MESSAGE");
      currentMode = MODE_MESSAGE;
    }

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

  if (currentMode == MODE_ROBOT_EYES) {
    unsigned long now = millis();
    // Head pat sensor triggers happy mood
    if (digitalRead(TOUCH_PIN) == HIGH) {
      if (!isBeingPetted) {
        Serial.println("[TOUCH] Head pat detected!");
        isBeingPetted = true;
        changeMood(HAPPY);
        happyUntil = now + 5000; // Stay happy for 5s after last touch
      }
    } else {
        isBeingPetted = false;
    }
    // If not being petted and happy timeout expired, change mood randomly
    if (now > happyUntil && !isBeingPetted) {
      if (now - lastMoodChange > moodInterval) {
        lastMoodChange = now;
        int moods[] = { DEFAULT, TIRED, ANGRY };
        int nextMood;
        do {
          nextMood = moods[random(0, 3)];
        } while (nextMood == currentMood); {
          changeMood(nextMood);
        }
      }
    }

    // Update eyes
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

  String visitLine = "http://" + IP.toString() + "/";
  displayMessageLines({
      "WiFi Setup Mode",
      "Turn on your phone's WiFi and connect to",
      "ESP-Setup",
      "",
      "Visit:",
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
    delay(40); // this changes the frame rate of the eyes animation during boot
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

  if (saveAndForce) {
    isMessageUnread = true;  // Mark new message as unread
    forceMessageMode = true; // Force message mode display
    Serial.println("[JSON] New message received. Forcing MODE_MESSAGE");
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

/*
  Function to update the OLED display based on the current mode
  This function clears the display and updates it according to the current mode.
  It handles different modes like MODE_ROBOT_EYES, MODE_MESSAGE, and MODE_DEBUG.
*/
void updateDisplay() {
  Serial.print("[DISPLAY] Updating mode: ");
  Serial.println(currentMode);

  if (currentMode != MODE_ROBOT_EYES) {
    display.clearDisplay(); // Only clear when not in robot mode
  }

  switch (currentMode) {
    case MODE_ROBOT_EYES:
      break;

    case MODE_MESSAGE:
      if (isMessageUnread) {
        showNewMessageLogo();
      } else {
        loadSavedMessage();
      }
      break;

    case MODE_DEBUG:
      if (isInAPMode) {
        String visitLine = "http://" + WiFi.softAPIP().toString() + "/";
        displayMessageLines({
          "WiFi Setup Mode",
          "Turn on your phone's WiFi and connect to",
          "ESP-Setup",
          "",
          "Visit:",
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

/*
  Function to change the mood of the robot eyes
  This function changes the mood of the robot eyes and updates the display accordingly.
  It also prints the new mood to the Serial monitor for debugging purposes.
*/
void changeMood(int mood) {
  if (currentMood != mood) {
    currentMood = mood;
    roboEyes.setMood(mood);
    Serial.print("[MOOD] Changed to: ");
    switch (mood) {
      case DEFAULT: Serial.println("DEFAULT"); break;
      case HAPPY: Serial.println("HAPPY"); break;
      case TIRED: Serial.println("TIRED"); break;
      case ANGRY: Serial.println("ANGRY"); break;
    }
  }
}

/*
  This function shows a logo on the screen to indicate a new message
  It clears the display and draws a bitmap image of the logo in the center.
*/
void showNewMessageLogo() {
  display.clearDisplay();
  display.drawBitmap((SCREEN_WIDTH - LOGO_WIDTH) / 2, (SCREEN_HEIGHT - LOGO_HEIGHT) / 2,
                    messageAnimation[0], LOGO_WIDTH, LOGO_HEIGHT, SSD1306_WHITE);
  display.display();
}

/*
  Function to play the full message animation
  This function iterates through all frames of the message animation and displays them one by one.
  It clears the display before each frame and waits for a specified duration between frames.
*/
void playFullAnimation() {
  Serial.println("[ANIMATION] Playing message animation");

  for (uint8_t i = 0; i < messageAnimationFrameCount; i++) {
    display.clearDisplay();
    display.drawBitmap(0, 0, messageAnimation[i], LOGO_WIDTH, LOGO_HEIGHT, SSD1306_WHITE);
    display.display();
    delay(frameDurationMs);
  }
}
