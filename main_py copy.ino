// ============================================
// ESP32 Smart Lighting System
// Motion + LDR + Auto-off Feature + Firebase + Voice Recognition
// ============================================

#define BLYNK_TEMPLATE_ID "TMPL23bmr2Y8E"
#define BLYNK_TEMPLATE_NAME "Smart Lighting ESP32"
#define BLYNK_AUTH_TOKEN "vSTfU3zIwpneYO4upFY5LXwyL3t0hk17"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp32.h>
#include <esp32-hal-ledc.h>  // for ledcAttach PWM control on ESP32
#include <WebServer.h>

// --------- WIFI CREDENTIALS ---------
#define WIFI_SSID "Market_EXT"  
#define WIFI_PASSWORD "134356**"

// --------- FIREBASE CREDENTIALS ---------
#define FIREBASE_HOST "ultrasonic-9a865-default-rtdb.firebaseio.com"
// Optional: If your database requires authentication, uncomment and add your database secret:
// #define FIREBASE_DB_SECRET "your_database_secret_here"
// To get database secret: Firebase Console → Project Settings → Service Accounts → Database Secrets

// --------- PIN DEFINITIONS ---------
#define TRIG_PIN 5
#define ECHO_PIN 18
#define LDR_PIN 36
#define LED_PIN 33
#define PWM_CHANNEL 0
#define PWM_FREQ    5000
#define PWM_RES     8   // 0–255
#define VOICE_DO_PIN 19  // HW-484 VO.2 DO (Digital Output) - detects voice activity
#define VOICE_AO_PIN 34  // HW-484 VO.2 AO (Analog Output) - voice signal level

// --------- CONFIGURATION ---------
#define LDR_THRESHOLD 2000    // Brightness threshold (0-4095)
#define MOTION_TIMEOUT 30000  // Auto-off delay: 30 seconds (ms)
#define ULTRASONIC_TIMEOUT 60000 // Max ultrasonic timeout (μs) - 60ms for better reliability
#define MOTION_ON_DISTANCE 25  // Distance to turn LED ON (cm)
#define MOTION_OFF_DISTANCE 40 // Distance to turn LED OFF (cm)

// --------- GLOBAL VARIABLES ---------
unsigned long lastMotionTime = 0;
bool motionDetected = false;
bool motionEverDetected = false; // Track if motion was ever detected
bool ledOn = false;
unsigned long lastFirebaseUpdate = 0;
const unsigned long FIREBASE_UPDATE_INTERVAL = 1000; // Update Firebase every 1 second
float lastValidDistance = -1; // Store last valid distance reading
int brightness = 0; // LDR brightness reading (global for debug output)

// Voice recognition variables
bool voiceOverride = false; // When true, voice commands override automatic control
bool voiceKeepOn = false; // When true, voice command keeps light on until explicitly turned off
unsigned long voiceOverrideTimeout = 0;
const unsigned long VOICE_OVERRIDE_DURATION = 5000; // Voice override lasts 5 seconds (reduced for faster response)
unsigned long lastFirebaseRead = 0;
const unsigned long FIREBASE_READ_INTERVAL = 150; // Read Firebase commands every 150ms (balanced for performance)

// Voice detection variables
bool lastVoiceState = false;
unsigned long voiceDetectedTime = 0;
const unsigned long VOICE_COMMAND_MIN_DURATION = 50; // Minimum voice duration to register (ms) - optimized for speed
const unsigned long VOICE_COMMAND_MAX_DURATION = 2000; // Maximum voice duration (ms)
int voiceCommandCount = 0; // Count voice activations to distinguish commands
unsigned long lastVoiceCommandTime = 0;
const unsigned long VOICE_COMMAND_TIMEOUT = 1000; // Time between voice activations (ms)
const int VOICE_SIGNAL_THRESHOLD = 2000; // Analog threshold to distinguish commands (0-4095)
const int VOICE_ACTIVITY_THRESHOLD = 500; // Lower threshold for fast activity detection (0-4095)
int maxVoiceSignal = 0; // Track maximum signal level during voice detection
bool voiceActivityDetected = false; // Fast activity detection from AO pin

// Blynk / App control variables
bool blynkOverride = false;      // Manual override from Blynk button V0
bool blynkManualState = false;   // Desired LED state from Blynk
bool autoMode = true;            // Auto mode from Blynk V3 (default ON)
unsigned long lastMotionAlert = 0;
const unsigned long MOTION_ALERT_COOLDOWN = 30000; // 30s between alerts
bool updatingV0 = false;         // Prevent UI flicker/echo on V0 sync
bool updatingV4 = false;         // Prevent UI flicker/echo on V4 sync
int ledBrightnessTarget = 255;   // Target brightness 0-255 (from Blynk V4 control)

// Web server for SmartControl.html frontend
WebServer server(80);

// ============================================
// WIFI & FIREBASE FUNCTIONS
// ============================================

// Connect to WiFi
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi Connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("WiFi Connection Failed!");
  }
}

// Update Firebase database with sensor data using REST API - Optimized
void updateFirebase(float distance, int brightness, bool ledState) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  HTTPClient http;
  http.setTimeout(1000); // Reduce timeout for faster failure detection
  http.setReuse(true); // Reuse connection for better performance
  
  // Build URL efficiently
  String url = "https://" + String(FIREBASE_HOST) + "/sensors.json";
  
  #ifdef FIREBASE_DB_SECRET
    url += "?auth=" + String(FIREBASE_DB_SECRET);
  #endif
  
  // Create JSON payload (optimized size)
  StaticJsonDocument<150> doc; // Reduced from 200
  doc["LED"] = ledState ? "on" : "off";
  doc["brightness"] = brightness;
  
  // Send distance (displayDistance already handles last valid fallback)
  if (distance >= 0) {
    doc["distance"] = distance;
  } else {
    doc["distance"] = "N/A";
  }
  
  String jsonPayload;
  serializeJson(doc, jsonPayload);
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode = http.PUT(jsonPayload);
  
  // Only print errors, not success (reduces Serial overhead)
  if (httpResponseCode <= 0 || httpResponseCode >= 400) {
    Serial.printf("Firebase update failed: HTTP %d\n", httpResponseCode);
  }
  
  http.end();
}

// Push data to Blynk (brightness %, distance, LED state)
void updateBlynk(float distance, int brightnessRaw, bool ledState) {
  if (!Blynk.connected()) return;
  
  int brightnessPercent = map(brightnessRaw, 0, 4095, 0, 100);
  brightnessPercent = constrain(brightnessPercent, 0, 100);
  Blynk.virtualWrite(V1, brightnessPercent);
  if (distance >= 0) {
    Blynk.virtualWrite(V2, distance);
  } else {
    Blynk.virtualWrite(V2, 0);
  }
}

// Read voice command from Firebase (sent by Python script) - Optimized non-blocking
String readFirebaseVoiceCommand() {
  if (WiFi.status() != WL_CONNECTED) {
    return "";
  }
  
  HTTPClient http;
  http.setTimeout(300); // Reduced timeout for faster response
  http.setReuse(true); // Reuse connection
  
  String url = "https://" + String(FIREBASE_HOST) + "/voiceCommand.json";
  
  #ifdef FIREBASE_DB_SECRET
    url += "?auth=" + String(FIREBASE_DB_SECRET);
  #endif
  
  http.begin(url);
  int httpResponseCode = http.GET();
  
  if (httpResponseCode > 0 && httpResponseCode < 300) {
    String response = http.getString();
    http.end();
    
    // Fast check for non-empty response
    if (response.length() > 4 && response != "null") { // "null" = 4 chars
      StaticJsonDocument<80> doc; // Reduced size
      DeserializationError error = deserializeJson(doc, response);
      
      if (!error && doc.containsKey("command")) {
        String command = doc["command"].as<String>();
        
        if (command.length() > 0) {
          // Clear command (non-blocking, minimal timeout)
          HTTPClient httpClear;
          httpClear.setTimeout(200);
          httpClear.setReuse(true);
          httpClear.begin(url);
          httpClear.addHeader("Content-Type", "application/json");
          httpClear.PUT("\"\"");
          httpClear.end();
          
          return command;
        }
      }
    }
  } else {
    http.end();
  }
  
  return "";
}

// Set brightness and sync with Blynk
void setBrightness(int percent) {
  percent = constrain(percent, 0, 100);
  ledBrightnessTarget = map(percent, 0, 100, 0, 255);
  
  // Apply brightness immediately if LED is on
  if (ledOn) {
    applyLEDPWM(true);
  }
  
  // Sync Blynk V4 switch (0 = 50%, 1 = 100%)
  if (Blynk.connected() && !updatingV4) {
    updatingV4 = true;
    int v4Value = (percent <= 50) ? 0 : 1;
    Blynk.virtualWrite(V4, v4Value);
    updatingV4 = false;
  }
  
  Serial.printf("Brightness set to %d%%\n", percent);
}

// Process voice command - Optimized string matching
void processVoiceCommand(String command) {
  command.toLowerCase();
  command.trim();
  
  Serial.printf("Voice: %s\n", command.c_str());
  
  // Brightness control commands (check first before on/off to avoid conflicts)
  if (command.indexOf("brightness 30") >= 0 || command.indexOf("brightness thirty") >= 0 ||
      command.indexOf("low brightness") >= 0 || command.indexOf("30 percent") >= 0 ||
      command.indexOf("thirty percent") >= 0 || command.indexOf("set brightness 30") >= 0) {
    setBrightness(30);
    return;
  } else if (command == "3" || command.indexOf("brightness 50") >= 0 || command.indexOf("brightness fifty") >= 0 ||
             command.indexOf("half brightness") >= 0 || command.indexOf("50 percent") >= 0 ||
             command.indexOf("fifty percent") >= 0 || command.indexOf("set brightness 50") >= 0) {
    setBrightness(50);
    return;
  } else if (command == "4" || command.indexOf("brightness 100") >= 0 || command.indexOf("brightness hundred") >= 0 ||
             command.indexOf("full brightness") >= 0 || command.indexOf("100 percent") >= 0 ||
             command.indexOf("hundred percent") >= 0 || command.indexOf("set brightness 100") >= 0 ||
             command.indexOf("maximum brightness") >= 0 || command.indexOf("max brightness") >= 0) {
    setBrightness(100);
    return;
  }
  
  // Light on/off commands
  if (command == "1" || command.indexOf("lights on") >= 0 || command.indexOf("light on") >= 0) {
    setLED(true);
    voiceOverride = true;
    voiceKeepOn = true; // Keep light on until explicitly turned off
    voiceOverrideTimeout = millis();
    Serial.println("Voice: Lights ON (stays on)");
  } else if (command == "2" || command.indexOf("lights off") >= 0 || command.indexOf("light off") >= 0) {
    setLED(false);
    voiceOverride = false;
    voiceKeepOn = false; // Clear keep-on flag
    voiceOverrideTimeout = 0;
    // Voice has highest priority: clear Blynk manual override so OFF always works
    blynkOverride = false;
    blynkManualState = false;
    if (Blynk.connected() && !updatingV0) {
      updatingV0 = true;
      Blynk.virtualWrite(V0, 0);
      updatingV0 = false;
    }
    Serial.println("Voice: Lights OFF (auto mode)");
  }
}

// ======================================================
// Blynk Handlers
// ======================================================

BLYNK_WRITE(V0) { // Manual LED on/off
  if (updatingV0) return; // Ignore echoes from device sync
  int v = param.asInt();
  blynkManualState = (v == 1);
  blynkOverride = true; // Engage manual override
  if (v == 0) {
    // Manual OFF should clear voice holds so OFF always works
    voiceKeepOn = false;
    voiceOverride = false;
  }
  // Manual command should act immediately (unless voiceKeepOn is active)
  if (!voiceKeepOn) {
    setLED(blynkManualState);
  }
}

BLYNK_WRITE(V4) { // Brightness preset switch: 0 -> 50%, 1 -> 100%
  if (updatingV4) return; // ignore echoes from device sync
  int val = param.asInt();
  int pct = (val == 0) ? 50 : 100;
  ledBrightnessTarget = map(pct, 0, 100, 0, 255);
  // If LED is on, apply new brightness immediately
  if (ledOn) {
    applyLEDPWM(true);
  }
}

BLYNK_WRITE(V3) { // Auto mode switch
  int v = param.asInt();
  autoMode = (v == 1);
  if (autoMode) {
    // Returning to auto -> clear manual override so motion/LDR can work
    blynkOverride = false;
  }
  // Optional: when auto is enabled, keep manual override but auto will run when override is not active
}

BLYNK_CONNECTED() {
  // Sync button and auto switch to current state
  updatingV0 = true;
  Blynk.virtualWrite(V0, ledOn ? 1 : 0);
  updatingV0 = false;
  Blynk.virtualWrite(V3, autoMode ? 1 : 0);
  // Sync brightness switch to nearest preset (0->50%, 1->100%)
  int pct = map(ledBrightnessTarget, 0, 255, 0, 100);
  int preset = (pct <= 75) ? 0 : 1; // 0 for 50%, 1 for 100%
  updatingV4 = true;
  Blynk.virtualWrite(V4, preset);
  updatingV4 = false;
}

// Check for voice commands from HW-484 module using DO and AO pins - Optimized
// Also uses AO pin for fast activity/presence detection
void checkVoiceModule() {
  bool currentVoiceState = digitalRead(VOICE_DO_PIN); // Read digital output
  int voiceSignalLevel = analogRead(VOICE_AO_PIN); // Read analog output (0-4095)
  
  // Fast activity detection: Use AO pin to detect any sound/activity (faster than waiting for full command)
  // This can help speed up presence detection
  voiceActivityDetected = (voiceSignalLevel > VOICE_ACTIVITY_THRESHOLD);
  
  // Detect rising edge (voice starts) - optimized
  if (currentVoiceState && !lastVoiceState) {
    voiceDetectedTime = millis();
    maxVoiceSignal = 0; // Reset max signal level
  }
  
  // Track maximum signal level while voice is active (optimized comparison)
  if (currentVoiceState && voiceDetectedTime > 0 && voiceSignalLevel > maxVoiceSignal) {
    maxVoiceSignal = voiceSignalLevel;
  }
  
  // Detect falling edge (voice ends) - optimized logic
  if (!currentVoiceState && lastVoiceState && voiceDetectedTime > 0) {
    unsigned long voiceDuration = millis() - voiceDetectedTime;
    
    // Fast range check
    if (voiceDuration >= VOICE_COMMAND_MIN_DURATION && voiceDuration <= VOICE_COMMAND_MAX_DURATION) {
      unsigned long timeSinceLastCommand = millis() - lastVoiceCommandTime;
      String command = "";
      
      // Optimized command detection (check signal level first, then count)
      // Note: For brightness commands, train HW-484 with "3" for 50% and "4" for 100%
      // Or use Python script with natural phrases like "brightness 50" or "brightness 100"
      if (maxVoiceSignal > VOICE_SIGNAL_THRESHOLD) {
        command = "lights on";
      } else {
        if (timeSinceLastCommand > VOICE_COMMAND_TIMEOUT) {
          voiceCommandCount = 1;
          command = "lights on";
        } else {
          voiceCommandCount++;
          if (voiceCommandCount == 2) {
            voiceCommandCount = 0;
            command = "lights off";
          }
        }
      }
      
      if (command.length() > 0) {
        processVoiceCommand(command);
        lastVoiceCommandTime = millis();
      }
    }
    
    voiceDetectedTime = 0;
    maxVoiceSignal = 0;
  }
  
  // Reset counter if too much time has passed (optimized check)
  static unsigned long lastCounterReset = 0;
  if (millis() - lastCounterReset > VOICE_COMMAND_TIMEOUT * 2) {
    if (voiceCommandCount > 0) {
      voiceCommandCount = 0;
    }
    lastCounterReset = millis();
  }
  
  lastVoiceState = currentVoiceState;
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  // PWM setup for LED brightness control
  ledcAttach(LED_PIN, PWM_FREQ, PWM_RES);
  
  // Initialize voice recognition module (HW-484 VO.2)
  pinMode(VOICE_DO_PIN, INPUT);  // DO pin as input
  // VOICE_AO_PIN (34) is analog input, no pinMode needed
  delay(100);
  Serial.println("Voice recognition module initialized (DO=19, AO=34)");
  
  digitalWrite(LED_PIN, LOW);
  lastMotionTime = millis(); // Initialize to prevent immediate timeout
  
  // Connect to WiFi
  connectToWiFi();

  // Start HTTP server for web controls (SmartControl.html)
  server.on("/set", []() {
    if (server.hasArg("value")) {
      int val = server.arg("value").toInt();
      val = constrain(val, 0, 255);
      int percent = map(val, 0, 255, 0, 100);
      Serial.printf("HTTP /set value=%d (%d%%)\n", val, percent);

      if (percent <= 0) {
        // Treat as manual OFF
        voiceKeepOn = false;
        voiceOverride = false;
        blynkManualState = false;
        blynkOverride = true;
        setLED(false);
      } else {
        // Manual brightness + ON
        setBrightness(percent);
        blynkOverride = true;
        blynkManualState = true;
        setLED(true);
      }
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing value");
    }
  });

  // Web auto mode control: /auto?value=0|1
  server.on("/auto", []() {
    if (server.hasArg("value")) {
      int v = server.arg("value").toInt();
      bool enable = (v != 0);
      autoMode = enable;
      Serial.printf("HTTP /auto value=%d (autoMode=%s)\n", v, enable ? "ON" : "OFF");

      if (enable) {
        // When returning to auto, clear manual override so motion/LDR can work
        blynkOverride = false;
      }

      // Sync Blynk V3 switch if connected
      if (Blynk.connected()) {
        Blynk.virtualWrite(V3, enable ? 1 : 0);
      }

      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Missing value");
    }
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found");
  });

  server.begin();
  Serial.println("HTTP server started on port 80");

  // Configure Blynk
  Blynk.config(BLYNK_AUTH_TOKEN);
  if (!Blynk.connect(2000)) {
    Serial.println("Blynk connection failed, will retry in loop");
  }
  
  Serial.println("Smart Lighting System Initialized!");
  Serial.println("Voice commands: 'lights on' or 'lights off'");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Ready to send data to Firebase!");
  }
}

// ============================================
// MAIN LOOP
// ============================================
void loop() {
  // Run Blynk
  Blynk.run();
  // Reconnect Blynk if needed (non-blocking)
  static unsigned long lastBlynkReconnect = 0;
  if (!Blynk.connected() && millis() - lastBlynkReconnect > 5000) {
    Blynk.connect(1000);
    lastBlynkReconnect = millis();
  }

  // Handle HTTP clients for web UI
  server.handleClient();
  
  // Check for voice commands from HW-484 module
  checkVoiceModule();
  
  // Check for voice commands from Firebase (sent by Python script) - Non-blocking
  // Only check if we have time (don't block motion sensor)
  static bool wifiStatusCached = false;
  static unsigned long lastWifiCheck = 0;
  
  if (millis() - lastFirebaseRead > FIREBASE_READ_INTERVAL) {
    // Cache WiFi status check (check every 2 seconds to avoid overhead)
    if (millis() - lastWifiCheck > 2000) {
      wifiStatusCached = (WiFi.status() == WL_CONNECTED);
      lastWifiCheck = millis();
    }
    
    if (wifiStatusCached) {
      String voiceCmd = readFirebaseVoiceCommand();
      if (voiceCmd.length() > 0) {
        processVoiceCommand(voiceCmd);
      }
    }
    lastFirebaseRead = millis();
  }
  
  // Check if voice override has expired (only if not keeping light on)
  if (voiceOverride && !voiceKeepOn && voiceOverrideTimeout > 0 && 
      (millis() - voiceOverrideTimeout > VOICE_OVERRIDE_DURATION)) {
    voiceOverride = false;
    voiceOverrideTimeout = 0; // Reset to prevent calculation errors
    Serial.println("Voice override expired, returning to automatic mode");
  }
  
  // Read sensors - optimize by reading distance once
  float currentDistance = getDistance();
  motionDetected = detectMotionFromDistance(currentDistance); // Use already-read distance
  brightness = readLDR(); // Update global brightness variable
  
  // Get distance for display/Firebase (uses last valid if current fails)
  float displayDistance = (currentDistance >= 0) ? currentDistance : 
                          ((lastValidDistance >= 0) ? lastValidDistance : -1);
  
  // Update motion timer - reset on motion OR voice activity for faster response
  if (motionDetected) {
    lastMotionTime = millis();
    if (motionDetected) {
    motionEverDetected = true; // Mark that motion was detected
    }
  }
  
  // Check if motion timeout exceeded (only if motion was previously detected)
  bool motionTimeout = motionEverDetected && ((millis() - lastMotionTime) > MOTION_TIMEOUT);
  
  // LED Logic priority: 1) Voice command, 2) Blynk manual override, 3) Auto logic
  if (voiceKeepOn) {
    setLED(true);
  } else if (blynkOverride) {
    setLED(blynkManualState);
  } else if (autoMode) {
    // Auto mode with motion + brightness (voice activity no longer holds LED on)
    bool activityDetected = motionDetected;
    if (brightness >= LDR_THRESHOLD || !activityDetected || motionTimeout) {
      setLED(false);
    } else {
      bool prevLed = ledOn;
      setLED(true);
      // Send motion alert event (once per cooldown) when auto turned LED on in dark due to motion
      if (!prevLed && ledOn && motionDetected && brightness < LDR_THRESHOLD) {
        if (millis() - lastMotionAlert > MOTION_ALERT_COOLDOWN) {
          if (Blynk.connected()) {
            Blynk.logEvent("motion_alert", "Motion detected – Light ON");
          }
          lastMotionAlert = millis();
        }
      }
    }
  } else {
    // Auto mode disabled and no overrides -> keep LED off
    setLED(false);
  }
  
  // Update Firebase every second (non-blocking, don't delay motion sensor)
  static bool wifiConnected = false;
  static unsigned long lastReconnectAttempt = 0;
  
  if (millis() - lastFirebaseUpdate > FIREBASE_UPDATE_INTERVAL) {
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      // Update Firebase but don't block - this runs in background
      updateFirebase(displayDistance, brightness, ledOn);
      updateBlynk(displayDistance, brightness, ledOn);
    } else {
      // Only reconnect if not already connected (non-blocking check)
      if (millis() - lastReconnectAttempt > 5000) { // Try reconnecting every 5 seconds
      Serial.println("WiFi disconnected, reconnecting...");
      connectToWiFi();
        lastReconnectAttempt = millis();
      }
    }
    lastFirebaseUpdate = millis();
  }
  
  // Debug output every 2 seconds (optimized, reduced Serial overhead)
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 2000) {
    // Optimized debug output (single printf call)
    if (displayDistance < 0) {
      Serial.printf("TO | M:%d | B:%d | LED:%s", 
                    motionDetected, brightness, ledOn ? "ON" : "OFF");
      if (lastValidDistance >= 0) {
        Serial.printf(" | L:%.1f", lastValidDistance);
      }
    } else {
      Serial.printf("D:%.1f | M:%d | B:%d | LED:%s",
                    displayDistance, motionDetected, brightness, ledOn ? "ON" : "OFF");
    }
    if (voiceOverride || voiceKeepOn) {
      Serial.print(" | VO");
    }
    Serial.println();
    lastDebug = millis();
  }
  
  // No delay - use non-blocking timing for maximum responsiveness
  // Yield to other tasks if needed
  yield();
}

// ============================================
// SENSOR FUNCTIONS
// ============================================

// Get distance from ultrasonic sensor (returns -1 on failure) - Optimized
float getDistance() {
  // Optimized trigger sequence (minimal delays)
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  // Wait for echo (with timeout) - pulseIn handles the delay
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT);
  
  // Fast check for timeout
  if (duration == 0 || duration > 23529) { // > 400cm in microseconds
    return -1; // Sensor timeout or out of range
  }
  
  // Calculate distance in cm (optimized: 0.034/2 = 0.017)
  float distance = duration * 0.017f;
  
  // Fast range validation (HC-SR04 typically 2cm to 400cm)
  if (distance < 2.0f || distance > 400.0f) {
    return -1; // Invalid reading
  }
  
  // Store valid distance
  lastValidDistance = distance;
  return distance;
}

// Get distance for display/Firebase (returns last valid if current fails)
float getDistanceForDisplay() {
  float dist = getDistance();
  // If current reading failed but we have a last valid, use it
  if (dist < 0 && lastValidDistance >= 0) {
    return lastValidDistance;
  }
  return dist;
}

// Ultrasonic motion detection with hysteresis (reads distance)
bool detectMotion() {
  float distance = getDistance();
  return detectMotionFromDistance(distance);
}
  
// Ultrasonic motion detection with hysteresis (uses provided distance)
// Optimized for immediate response when motion stops
bool detectMotionFromDistance(float distance) {
  // Invalid reading = no motion
  if (distance < 0) {
    return false;
  }
  
  // Hysteresis: turns on at 25cm, off at 40cm
  // For immediate OFF response, use tighter hysteresis when turning off
  if (distance < MOTION_ON_DISTANCE) {
    return true;  // Motion detected - immediate ON
  } else if (distance > MOTION_OFF_DISTANCE) {
    return false; // No motion - immediate OFF (no delay)
  }
  
  // If between thresholds (25-40cm), use previous state for hysteresis
  // But if currently ON and distance is increasing, turn off faster
  if (motionDetected && distance > (MOTION_ON_DISTANCE + MOTION_OFF_DISTANCE) / 2) {
    // Distance is in upper half of hysteresis zone and was ON - turn off for faster response
    return false;
  }
  
  return motionDetected;
}

// Read LDR brightness level
int readLDR() {
  return analogRead(LDR_PIN);
}

// Apply PWM brightness to LED (pin-based API)
void applyLEDPWM(bool state) {
  if (state) {
    ledcWrite(LED_PIN, ledBrightnessTarget);
  } else {
    ledcWrite(LED_PIN, 0);
  }
}

// Control LED - with PWM brightness, and sync Blynk button
void setLED(bool state) {
  if (state != ledOn) {
    ledOn = state;
    applyLEDPWM(state);
    // Sync Blynk button if connected (non-blocking)
    if (Blynk.connected() && !updatingV0) {
      updatingV0 = true;
      Blynk.virtualWrite(V0, ledOn ? 1 : 0);
      updatingV0 = false;
    }
  }
}