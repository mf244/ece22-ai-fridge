/*********************************************************************
 * ESP32 fridge controller – dual sensors, web buttons, cooling LED
 *********************************************************************/
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>

/* ---------- pins -------------------------------------------------- */
constexpr uint8_t BTN_UP   = 13;
constexpr uint8_t BTN_DN   = 27;
constexpr uint8_t BTN_MODE = 12;
constexpr uint8_t DHT1_PIN = 26;
constexpr uint8_t DHT2_PIN = 25;
constexpr uint8_t COOL_PIN = 18;

/* ---------- sensors ---------------------------------------------- */
#define DHTTYPE DHT11
DHT dht1(DHT1_PIN, DHTTYPE);
DHT dht2(DHT2_PIN, DHTTYPE);

/* ---------- state ------------------------------------------------- */
float curT1=NAN, curH1=NAN;
float curT2=NAN, curH2=NAN;
float setT = 38.0f;           // °F
int   mode = 1;               // 1‑Auto 2‑Cool 3‑Off
volatile bool lcdDirty = true;

/* ---------- LCD --------------------------------------------------- */
LiquidCrystal_I2C lcd(0x27,16,2);

/* ---------- web server ------------------------------------------- */
AsyncWebServer server(80);

const char INDEX_HTML[] PROGMEM = R"rawliteral(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="utf-8">
      <title>ESP32 Fridge</title>
      <style>
        body { font-family: sans-serif; margin: 2rem; }
        button { margin: 0.2rem 0.4rem; padding: 0.3rem 1rem; font-size: 1rem; }
        pre { font-size: 1.15rem; }
      </style>
    </head>
    <body>
      <h1>ESP32 Fridge</h1>
    
      <button onclick="btn('up')">▲ Up</button>
      <button onclick="btn('down')">▼ Down</button>
      <button onclick="btn('mode')">Mode</button>
    
      <pre id="data">Loading…</pre>
    
      <script>
        const el = document.getElementById("data");
        const modeNames = {
          1: "Auto",
          2: "Cool",
          3: "Off"
        };
    
        async function poll() {
          try {
            const res = await fetch("/status");
            const j   = await res.json();
            // wrap each line in backticks, then concat
            el.textContent =
              `T1  ${j.t1.toFixed(1)}°F   ${j.h1.toFixed(0)}% \n` +
              `T2  ${j.t2.toFixed(1)}°F   ${j.h2.toFixed(0)}% \n` +
              `Set ${j.set.toFixed(1)}°F   Mode ${modeNames[j.mode] || "?"} \n` +
              `Cooling ${j.cool ? "ON" : "OFF"}`;
          } catch (e) {
            console.error("poll error:", e);
            el.textContent = "Error fetching status";
          }
        }
    
        async function btn(cmd) {
          // URL must be a string, here using backticks again:
          await fetch(`/btn?cmd=${cmd}`);
          poll();
        }
    
        setInterval(poll, 1000);
        poll();
      </script>
    </body>
    </html>
    )rawliteral";
    


/********************************************************************/
void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP,   INPUT_PULLUP);
  pinMode(BTN_DN,   INPUT_PULLUP);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(COOL_PIN, OUTPUT);  digitalWrite(COOL_PIN, LOW);

  dht1.begin(); dht2.begin();

  Wire.begin(21,22);
  lcd.init(); lcd.backlight();
  lcd.print("ESP32 Fridge"); lcd.setCursor(0,1); lcd.print("Booting...");
  delay(1000); lcdDirty=true;

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-Fridge");
  MDNS.begin("fridge");

 /* ---------- Web routes -------------------------------------- */
 server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){
  r->send(200, "text/html", INDEX_HTML);
});

server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r){
  char buf[256];
  snprintf(buf, sizeof(buf),
    R"({"t1":%.1f,"h1":%.0f,"t2":%.1f,"h2":%.0f,"set":%.1f,"mode":%d,"cool":%d,"b1":%d,"b2":%d,"b3":%d})",
    curT1, curH1, curT2, curH2, setT, mode,
    digitalRead(COOL_PIN)==HIGH,
    digitalRead(BTN_UP)==LOW,
    digitalRead(BTN_DN)==LOW,
    digitalRead(BTN_MODE)==LOW
  );
  r->send(200, "application/json", buf);
});

server.on("/btn", HTTP_GET, [](AsyncWebServerRequest *r){
  if (r->hasParam("cmd")) {
    String c = r->getParam("cmd")->value();
    if (c == "up")   setT = min(setT + 1.0f, 120.0f);
    if (c == "down") setT = max(setT - 1.0f, -40.0f);
    if (c == "mode") mode = mode % 3 + 1;
    lcdDirty = true;
  }
  r->send(204);
});

server.begin();
}

/********************************************************************/
void loop() {

  /* ---------- physical buttons -------------------------------- */
  static bool lUp=HIGH,lDn=HIGH,lMd=HIGH;
  bool up=digitalRead(BTN_UP), dn=digitalRead(BTN_DN), md=digitalRead(BTN_MODE);

  if(lUp==HIGH && up==LOW){ setT=min(setT+1,120.f); lcdDirty=true; }
  if(lDn==HIGH && dn==LOW){ setT=max(setT-1,-40.f);lcdDirty=true; }
  if(lMd==HIGH && md==LOW){ mode=mode%3+1;          lcdDirty=true; }
  lUp=up; lDn=dn; lMd=md;

  /* ---------- read sensors every 2 s -------------------------- */
  static uint32_t last=0;
  if(millis()-last>2000){
      float c1=dht1.readTemperature();
      curT1=isnan(c1)?NAN:c1*9/5+32;
      curH1=dht1.readHumidity();
      float c2=dht2.readTemperature();
      curT2=isnan(c2)?NAN:c2*9/5+32;
      curH2=dht2.readHumidity();
      lcdDirty=true;
      last=millis();

      bool cooling = (digitalRead(COOL_PIN) == HIGH);
    Serial.printf(
      "T1: %.1f°F  H1: %.0f%%   T2: %.1f°F  H2: %.0f%%   "
      "Set: %.1f°F   Mode: %d   Cooling: %s\n",
      curT1, curH1, curT2, curH2,
      setT, mode,
      cooling ? "ON" : "OFF"
    );
  }

  

   /* ----- LCD refresh ------------------------------------------ */
   if(lcdDirty){
    lcd.clear();
    lcd.setCursor(0,0); lcd.printf("C:%4.1fF S:%4.1fF",curT1,setT);
    switch(mode) {
     case 1:
       lcd.setCursor(0,1); lcd.printf("Mode: Auto");
       break;
     case 2:
       lcd.setCursor(0,1); lcd.printf("Mode: Cool");
       break;
     case 3:
       lcd.setCursor(0,1); lcd.printf("Mode: Off");
       break;
     default:
       lcd.setCursor(0,1); lcd.printf("Mode: Error");
   }
    lcdDirty=false;
}

/*------------------ Operation Logic ------------------*/
bool coolOn = (curT1 > setT);          // true → need cooling

switch (mode) {
case 1:   // Auto
 digitalWrite(COOL_PIN, coolOn ? HIGH : LOW);
 break;

case 2:   // Force ON
 digitalWrite(COOL_PIN, HIGH);
 break;

case 3:   // Force OFF
 digitalWrite(COOL_PIN, LOW);
 break;

default:  // safety fallback
 digitalWrite(COOL_PIN, LOW);
 break;
}
}