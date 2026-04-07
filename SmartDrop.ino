/*
 * ====================================================================
 * PROJECT: SmartDrop+ (Système d'Irrigation Intelligent & Prédictif)
 * VERSION: 10.0 (Arabic Dashboard + Corrected Urgency Logic + Export Data Btn)
 * MATÉRIEL: ESP32-S3, SHT31, RTC DS3231, Capteurs Sol analogiques
 * ====================================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_SHT31.h>
#include "RTClib.h"

// --- CONFIGURATION WIFI ---
const char* ssid = "VOTRE_NOM_WIFI";
const char* password = "VOTRE_MOT_DE_PASSE";
WebServer server(80);

// --- CONFIGURATION LOGIQUE DES RELAIS ---
const int RELAY_ON = HIGH;  
const int RELAY_OFF = LOW; 

// --- PINS DES CAPTEURS & LEDS ---
const int RAIN_DO_PIN = 10;   
const int RAIN_AO_PIN = 7;    
const int SURVIVAL_LED = 18;  
const int URGENT_LED = 20;    

// --- STRUCTURE DES ZONES ---
struct Zone {
  String name;
  int analogPin;   
  int relayPin;    
  int ledPin;      
  float Kc, Zr, p;         
  float TAW, RAW, Dr;
  bool isIrrigating;
  bool sensorError;
  float currentMoisture; 
};

Zone zones[3] = {
  {"Maticha", 1, 2, 15, 1.15, 0.6, 0.40, 0, 0, 0, false, false, 0},
  {"Na3na3",  3, 4, 16, 1.00, 0.4, 0.30, 0, 0, 0, false, false, 0},
  {"Orange",  5, 6, 17, 0.70, 1.0, 0.50, 0, 0, 0, false, false, 0}
};

// --- VARIABLES GLOBALES ---
bool globalError = false;
bool isRainingGlobal = false;
float currentTemp = 25.0;
float currentRainMm = 0.0;

// --- VARIABLES DU MODE URGENCE ---
bool isUrgentMode = false;
String urgentReason = "طبيعي";

float lastTemp = -999.0;
unsigned long lastTempChangeTime = 0;

float lastMoisture[3] = {-1.0, -1.0, -1.0};
unsigned long lastMoistureChangeTime[3] = {0, 0, 0};
unsigned long illogicalRainTime[3] = {0, 0, 0};

Adafruit_SHT31 sht31 = Adafruit_SHT31();
RTC_DS3231 rtc;

// --- TIMING ---
unsigned long previousMainMillis = 0;
const unsigned long mainInterval = 4000; 
unsigned long previousLedMillis = 0;
const unsigned long ledInterval = 300;   
bool ledState = false;

// ====================================================================
// PAGE HTML & CSS (ARABIC - RTL)
// ====================================================================
const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SmartDrop+ Dashboard</title>
  <style>
    body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; margin: 0; padding: 20px; }
    h1 { text-align: center; color: #00d2ff; }
    .header-card { background: #1e1e1e; border-radius: 10px; padding: 15px; margin-bottom: 20px; text-align: center; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }
    .header-card span { font-size: 1.2em; margin: 0 10px; font-weight: bold; }
    .urgent-banner { background: #ff3333; color: white; padding: 10px; border-radius: 5px; text-align: center; font-weight: bold; font-size: 1.2em; margin-bottom: 20px; display: none; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; }
    .card { background: #1e1e1e; border-radius: 10px; padding: 20px; box-shadow: 0 4px 8px rgba(0,0,0,0.3); }
    .card h2 { margin-top: 0; color: #00d2ff; border-bottom: 1px solid #333; padding-bottom: 10px; }
    .stat { margin: 10px 0; font-size: 1.1em; }
    .on { color: #00ff00; font-weight: bold; }
    .off { color: #aaaaaa; }
    .err { color: #ff3333; font-weight: bold; }
    .btn-extract { background-color: #00d2ff; color: #121212; border: none; padding: 10px; width: 100%; border-radius: 5px; font-size: 1em; font-weight: bold; cursor: pointer; margin-top: 15px; transition: background 0.3s; }
    .btn-extract:hover { background-color: #0099cc; }
  </style>
</head>
<body>
  <h1>💧 SmartDrop+</h1>
  
  <div id="urgent-box" class="urgent-banner">
    ⚠️ وضع الطوارئ نشط: <span id="urgent-reason"></span>
  </div>

  <div class="header-card">
    <span id="temp">الحرارة: -- °C</span> | 
    <span id="rain">المطر: -- mm</span> 
    <span id="status"></span>
  </div>
  
  <div class="grid" id="zones">
    </div>

  <script>
    function extractData(zoneName) {
      alert("يتم الآن تجهيز استخراج البيانات لمنطقة: " + zoneName + "\n(ملاحظة: تحتاج إلى ذاكرة SD لحفظ السجل اليومي بالكامل)");
    }

    function updateDashboard() {
      fetch('/data')
        .then(response => response.json())
        .then(data => {
          document.getElementById('temp').innerText = "الحرارة: " + data.temp + " °C";
          document.getElementById('rain').innerText = "المطر: " + data.rain + " mm";
          document.getElementById('status').innerHTML = data.isRaining ? "<span class='err'>(توقف بسبب المطر)</span>" : "<span class='on'>(الطقس جيد)</span>";
          
          let uBox = document.getElementById('urgent-box');
          if(data.isUrgent) {
             uBox.style.display = "block";
             document.getElementById('urgent-reason').innerText = data.urgentReason;
          } else {
             uBox.style.display = "none";
          }

          let zonesHtml = '';
          data.zones.forEach(z => {
            let valveHtml = z.valve ? "<span class='on'>[يعمل] جاري الري</span>" : "<span class='off'>[متوقف] وضع الاستعداد</span>";
            let healthHtml = z.error ? "<span class='err'>غير متصل</span>" : "<span class='on'>سليم</span>";
            
            zonesHtml += `
              <div class="card">
                <h2>🌱 ${z.name}</h2>
                <div class="stat">الرطوبة: <b>${z.moisture}%</b></div>
                <div class="stat">النقص (Dr): <b>${z.dr}</b></div>
                <div class="stat">الحالة: ${healthHtml}</div>
                <div class="stat">الصمام: ${valveHtml}</div>
                <button class="btn-extract" onclick="extractData('${z.name}')">📥 استخراج بيانات اليوم</button>
              </div>
            `;
          });
          document.getElementById('zones').innerHTML = zonesHtml;
        })
        .catch(err => console.log("Fetching data...", err));
    }
    setInterval(updateDashboard, 2000);
    updateDashboard(); 
  </script>
</body>
</html>
)rawliteral";

// ====================================================================
// FONCTIONS DU SERVEUR WEB
// ====================================================================
void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

void handleData() {
  // Buffer augmenté pour supporter les caractères Arabes (UTF-8)
  char jsonBuffer[1536]; 
  snprintf(jsonBuffer, sizeof(jsonBuffer), 
    "{\"temp\":%.1f,\"rain\":%.1f,\"isRaining\":%s,\"isUrgent\":%s,\"urgentReason\":\"%s\",\"zones\":["
    "{\"name\":\"%s\",\"moisture\":%.0f,\"dr\":%.2f,\"error\":%s,\"valve\":%s},"
    "{\"name\":\"%s\",\"moisture\":%.0f,\"dr\":%.2f,\"error\":%s,\"valve\":%s},"
    "{\"name\":\"%s\",\"moisture\":%.0f,\"dr\":%.2f,\"error\":%s,\"valve\":%s}"
    "]}",
    currentTemp, currentRainMm, isRainingGlobal ? "true" : "false", 
    isUrgentMode ? "true" : "false", urgentReason.c_str(),
    zones[0].name.c_str(), zones[0].currentMoisture, zones[0].Dr, zones[0].sensorError ? "true" : "false", zones[0].isIrrigating ? "true" : "false",
    zones[1].name.c_str(), zones[1].currentMoisture, zones[1].Dr, zones[1].sensorError ? "true" : "false", zones[1].isIrrigating ? "true" : "false",
    zones[2].name.c_str(), zones[2].currentMoisture, zones[2].Dr, zones[2].sensorError ? "true" : "false", zones[2].isIrrigating ? "true" : "false"
  );
  server.send(200, "application/json", jsonBuffer);
}

// ====================================================================
// INITIALISATION
// ====================================================================
void setup() {
  Serial.begin(115200);
  unsigned long t = millis();
  while (!Serial && millis() - t < 3000); 

  Wire.begin(8, 9); 
  if (!sht31.begin(0x44)) { globalError = true; }
  if (!rtc.begin())       { globalError = true; }
  
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  pinMode(RAIN_DO_PIN, INPUT_PULLUP);
  pinMode(SURVIVAL_LED, OUTPUT); digitalWrite(SURVIVAL_LED, LOW);
  pinMode(URGENT_LED, OUTPUT);   digitalWrite(URGENT_LED, LOW);

  for(int i=0; i<3; i++) {
    pinMode(zones[i].relayPin, OUTPUT); digitalWrite(zones[i].relayPin, RELAY_OFF); 
    pinMode(zones[i].ledPin, OUTPUT);   digitalWrite(zones[i].ledPin, LOW);         
    zones[i].TAW = 110.0 * zones[i].Zr; 
    zones[i].RAW = zones[i].p * zones[i].TAW;
    zones[i].Dr = 0;
  }

  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) { delay(500); }
  
  if(WiFi.status() == WL_CONNECTED) {
    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.begin();
  }
  
  unsigned long startMillis = millis();
  lastTempChangeTime = startMillis;
  for(int i=0; i<3; i++) lastMoistureChangeTime[i] = startMillis;
}

// ====================================================================
// LOOP PRINCIPAL
// ====================================================================
void loop() {
  unsigned long currentMillis = millis();
  server.handleClient();

  if (globalError || isUrgentMode) {
    if (currentMillis - previousLedMillis >= ledInterval) {
      previousLedMillis = currentMillis;
      ledState = !ledState;
      if (globalError) digitalWrite(SURVIVAL_LED, ledState ? HIGH : LOW);
      if (isUrgentMode) digitalWrite(URGENT_LED, ledState ? HIGH : LOW);
    }
  } else {
    digitalWrite(SURVIVAL_LED, LOW);
    digitalWrite(URGENT_LED, LOW);
    ledState = false;
  }

  if (currentMillis - previousMainMillis >= mainInterval) {
    previousMainMillis = currentMillis;
    
    isUrgentMode = false;
    urgentReason = "";
    globalError = false; 

    // --- 1. SHT31 ---
    currentTemp = sht31.readTemperature();
    if (isnan(currentTemp)) { currentTemp = 25.0; globalError = true; }

    if (currentTemp != lastTemp) {
      lastTempChangeTime = currentMillis;
      lastTemp = currentTemp;
    }
    if (currentMillis - lastTempChangeTime > 30000) {
      isUrgentMode = true;
      urgentReason = "مستشعر الحرارة معلق ولا يتغير";
    }

    // --- 2. PLUIE ---
    int rainRaw = analogRead(RAIN_AO_PIN);
    currentRainMm = constrain(map(rainRaw, 4095, 0, 0, 40) / 10.0, 0.0, 4.0);
    isRainingGlobal = ((digitalRead(RAIN_DO_PIN) == LOW) || currentRainMm > 1.0);

    float ET0 = ((0.0023 * 31.0 * (currentTemp + 17.8) * 1.5) / 86400.0) * 5.0;

    for(int i = 0; i < 3; i++) {
      int soilRaw = analogRead(zones[i].analogPin);
      
      if (soilRaw < 100 || soilRaw > 4050) { 
        zones[i].sensorError = true; globalError = true; 
      } else { 
        zones[i].sensorError = false; 
      }

      zones[i].currentMoisture = constrain(map(soilRaw, 4095, 1500, 0, 100), 0, 100);

      // --- LOGIQUE D'IRRIGATION ---
      DateTime now = rtc.now();
      zones[i].Dr += (ET0 * zones[i].Kc);
      bool isNight = (now.hour() >= 18 || now.hour() <= 8);

      if (globalError || zones[i].sensorError || isRainingGlobal) {
        zones[i].isIrrigating = false; 
      } else {
        if (zones[i].currentMoisture >= 75.0) { 
          zones[i].isIrrigating = false; 
          zones[i].Dr = 0; 
        }
        else if (zones[i].currentMoisture <= 30.0) {
          zones[i].isIrrigating = true; 
        }
        else if (zones[i].Dr >= zones[i].RAW && isNight) {
          zones[i].isIrrigating = true; 
        }
      }

      // --- VERIFICATION URGENCE: HUMIDITÉ BLOQUÉE (PENDANT L'IRRIGATION SEULEMENT) ---
      if (zones[i].currentMoisture != lastMoisture[i]) {
        lastMoistureChangeTime[i] = currentMillis;
        lastMoisture[i] = zones[i].currentMoisture;
      }
      
      // RESET LE CHRONO SI LA VANNE EST FERMÉE
      if (!zones[i].isIrrigating) {
        lastMoistureChangeTime[i] = currentMillis;
      }

      // DÉCLENCHE L'ALARME SEULEMENT SI LA VANNE EST OUVERTE DEPUIS > 30s SANS CHANGEMENT D'HUMIDITÉ
      if (zones[i].isIrrigating && (currentMillis - lastMoistureChangeTime[i] > 30000)) {
        isUrgentMode = true;
        urgentReason = zones[i].name + " - مستشعر الرطوبة معلق أثناء الري";
      }

      // --- VERIFICATION URGENCE: ILLOGIQUE (Pluie + Sol Sec) ---
      if (isRainingGlobal && zones[i].currentMoisture <= 30.0) {
        if (illogicalRainTime[i] == 0) { illogicalRainTime[i] = currentMillis; }
        else if (currentMillis - illogicalRainTime[i] > 30000) {
          isUrgentMode = true;
          urgentReason = zones[i].name + " - تمطر ولكن التربة جافة";
        }
      } else {
        illogicalRainTime[i] = 0; 
      }

      // --- ACTIVATION ---
      digitalWrite(zones[i].relayPin, zones[i].isIrrigating ? RELAY_ON : RELAY_OFF);
      digitalWrite(zones[i].ledPin, zones[i].isIrrigating ? HIGH : LOW);
    }
  }
}
