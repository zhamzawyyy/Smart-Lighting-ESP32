#include <WiFi.h>
#include <WebServer.h>

// --- إعدادات الشبكة ---
const char* ssid = "YaraPC";      // اسم الشبكة
const char* password = "esp32iot"; // كلمة السر

// --- تعريف الأطراف (Pins) ---
const int ledPin = 23;    // طرف الليد
const int micPin = 34;    // طرف الميكروفون (Digital Output من حساس الصوت أو Analog)
// ملاحظة: لو الحساس له مخرج Digital (DO) وصله بـ Pin عادي. لو Analog (AO) وصله بـ 34.
// سنستخدم هنا الـ Analog لقراءة شدة الصوت.

// --- متغيرات النظام ---
int brightness = 0;       // مستوى الإضاءة الحالي
bool isLightOn = false;   // حالة النور

// --- إعدادات PWM ---
const int freq = 5000;
const int ledChannel = 0;
const int resolution = 8;

WebServer server(80);

// --- صفحة الويب المحسنة (UI Modern + Icons) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Smart Home Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.4.0/css/all.min.css">
  <link href="https://fonts.googleapis.com/css2?family=Cairo:wght@400;700&family=Poppins:wght@300;500;700&display=swap" rel="stylesheet">
  <style>
    body {
      font-family: 'Poppins', sans-serif;
      background-color: #121212;
      color: #e0e0e0;
      text-align: center;
      margin: 0;
      padding: 20px;
    }
    .card {
      max-width: 380px;
      margin: 20px auto;
      background: #1e1e2e;
      padding: 30px;
      border-radius: 25px;
      box-shadow: 0 0 20px rgba(0, 210, 255, 0.1);
      border: 1px solid #333;
    }
    h1 { color: #fff; margin-bottom: 5px; font-size: 24px; }
    h3 { color: #00d2ff; font-weight: 300; margin-top: 0; display: flex; align-items: center; justify-content: center; gap: 10px; }
    
    /* أيقونة الليد الكبيرة */
    .bulb-icon {
      font-size: 80px;
      color: #444;
      margin: 20px 0;
      transition: 0.3s ease;
    }
    .bulb-on { color: #ffeb3b; text-shadow: 0 0 30px #ffeb3b; }

    /* Slider Style */
    input[type=range] {
      -webkit-appearance: none;
      width: 100%;
      height: 10px;
      border-radius: 5px;
      background: #333;
      outline: none;
      margin: 20px 0;
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 22px;
      height: 22px;
      border-radius: 50%;
      background: #00d2ff;
      cursor: pointer;
      box-shadow: 0 0 10px #00d2ff;
    }

    /* Buttons */
    .btn-grid {
      display: grid;
      grid-template-columns: 1fr 1fr; 
      gap: 15px;
      margin-top: 20px;
    }
    .btn {
      padding: 15px;
      border: none;
      border-radius: 15px;
      font-size: 16px;
      font-weight: bold;
      cursor: pointer;
      color: white;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      transition: transform 0.2s;
    }
    .btn:active { transform: scale(0.95); }
    
    .btn-on { background: linear-gradient(135deg, #00b09b, #96c93d); box-shadow: 0 4px 15px rgba(0,176,155,0.4); grid-column: span 2; }
    .btn-high { background: #2b2b40; border: 1px solid #ff9800; color: #ff9800; }
    .btn-med { background: #2b2b40; border: 1px solid #00d2ff; color: #00d2ff; }
    .btn-off { background: linear-gradient(135deg, #ff416c, #ff4b2b); grid-column: span 2; box-shadow: 0 4px 15px rgba(255,75,43,0.4); }

    .status-text { font-size: 14px; color: #888; margin-top: 20px; }
    .fa-microphone { color: #ff4b2b; animation: pulse 2s infinite; }

    @keyframes pulse {
      0% { opacity: 1; }
      50% { opacity: 0.5; }
      100% { opacity: 1; }
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>Smart Control</h1>
    <h3><i class="fas fa-wifi"></i> Connected</h3>
    
    <i id="bulbIcon" class="fas fa-lightbulb bulb-icon"></i>
    
    <p>Brightness: <span id="statusValue">0</span>%</p>
    <input type="range" min="0" max="255" value="0" id="pwmSlider" oninput="updateSlider(this.value)">
    
    <div class="btn-grid">
      <button class="btn btn-on" onclick="setLight(255)"><i class="fas fa-power-off"></i> POWER ON</button>
      
      <button class="btn btn-high" onclick="setLight(255)">100%</button>
      <button class="btn btn-med" onclick="setLight(153)">60%</button>
      
      <button class="btn btn-off" onclick="setLight(0)"><i class="fas fa-stop"></i> SHUT DOWN</button>
    </div>

    <div class="status-text">
      <i class="fas fa-microphone"></i> Voice Sensor Active (Clap Control)
    </div>
  </div>

  <script>
    function updateSlider(val) {
      var percent = Math.round((val / 255) * 100);
      document.getElementById("statusValue").innerText = percent;
      
      // Update Bulb Glow
      var bulb = document.getElementById("bulbIcon");
      if(val > 0) {
        bulb.classList.add("bulb-on");
        bulb.style.opacity = (val/255) + 0.3; // Dynamic brightness effect
      } else {
        bulb.classList.remove("bulb-on");
        bulb.style.opacity = 1;
      }
      
      sendData(val);
    }

    function setLight(val) {
      document.getElementById("pwmSlider").value = val;
      updateSlider(val);
    }

    function sendData(val) {
      var xhr = new XMLHttpRequest();
      xhr.open("GET", "/set?value=" + val, true);
      xhr.send();
    }
  </script>
</body>
</html>
)rawliteral";

// --- دوال التحكم ---

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleSet() {
  if (server.hasArg("value")) {
    int val = server.arg("value").toInt();
    
    // تطبيق القيمة
    ledcWrite(ledChannel, val);
    brightness = val;
    isLightOn = (val > 0);
    
    server.send(200, "text/plain", "OK");
    Serial.print("Web Command: "); Serial.println(val);
  }
}

// دالة لمعالجة الميكروفون
void checkMicrophone() {
  // اقرأ قيمة حساس الصوت (Analog)
  // لو الحساس Digital استخدم digitalRead(micPin)
  int micVal = analogRead(micPin); 

  // حدد "عتبة" للصوت (Threshold) - غير الرقم ده حسب حساسية المايك بتاعك
  // مثلا لو القيمة زادت عن 2000 اعتبرها تصفيق
  if (micVal > 2000) { 
    Serial.println("Clap Detected!");
    
    // عكس الحالة (لو شغال اطفيه، لو مطفي شغله)
    if (isLightOn) {
      ledcWrite(ledChannel, 0);
      isLightOn = false;
      brightness = 0;
    } else {
      ledcWrite(ledChannel, 255);
      isLightOn = true;
      brightness = 255;
    }
    
    // تأخير بسيط عشان ميحصلش تكرار سريع (Debounce)
    delay(500); 
  }
}

void setup() {
  Serial.begin(115200);

  // إعداد الليد
  ledcSetup(ledChannel, freq, resolution);
  ledcAttachPin(ledPin, ledChannel);

  // إعداد الميكروفون
  pinMode(micPin, INPUT);

  // الاتصال بالواي فاي
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.begin();
}

void loop() {
  server.handleClient();
  
  // فحص الميكروفون بشكل مستمر
  checkMicrophone();
}