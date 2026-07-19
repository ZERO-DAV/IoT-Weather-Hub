#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
/* ======================================================================
   عَيْن الأرصاد الذكية — IoT Weather Hub
   ملاحظة: هذا الكود مبني على ESP32 (مو Arduino عادي ولا ESP8266) لأنه
   هو اللي عنده الواي فاي والذاكرة الداخلية. إذا اللوحة غير كذا خبرني.
   ====================================================================== */
// ============================================================
//  إعدادات الشاشة
// ============================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// ============================================================
//  إعدادات الحساس
// ============================================================
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
const unsigned long READ_INTERVAL = 2000;      // قراءة الحساس كل ثانيتين
const unsigned long DISPLAY_INTERVAL = 250;    // تحديث الشاشة 4 مرات بالثانية
unsigned long lastReadTime = 0;
unsigned long lastDisplayTime = 0;
float lastTemp = NAN;
float lastHum  = NAN;
bool  hasValidReading = false;
byte  failCount = 0;
const byte MAX_FAILS = 5;
float minTemp = 1000, maxTemp = -1000;
float minHum  = 1000, maxHum  = -1000;
// ============================================================
//  إعدادات الزر
// ============================================================
#define BUTTON_PIN 27   // GPIO27: دبوس عام آمن تماماً، ما له علاقة بـ PSRAM ولا Strapping
bool buttonState = HIGH;     // الحالة "المؤكدة" بعد إزالة التذبذب
bool lastRawState = HIGH;    // آخر قراءة خام (تُستخدم فقط لاكتشاف التذبذب)
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;
unsigned long pressStartTime = 0;
bool isHolding = false;
bool resetCountdownActive = false;
const unsigned long HOLD_DISPLAY_THRESHOLD = 1500;  // بعد ثانية ونص إمساك يبدأ يطلع العد التنازلي
const unsigned long RESET_HOLD_TIME        = 15000; // إمساك متواصل 15 ثانية = فورمات المصنع
// ============================================================
//  شبكة الإعداد (Access Point) + السيرفر
// ============================================================
const char* AP_SSID_BASE = "DHT-Setup";
const char* AP_PASSWORD  = "12345678";   // باسورد بسيط وسهل الكتابة على أي جوال
String currentAPSSID;                    // يتحدد بشكل عشوائي كل مره تدخل وضع الإعداد
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);
Preferences prefs;
const unsigned long CONNECT_TIMEOUT      = 20000; // مهلة محاولة الاتصال بعد إدخال البيانات
const unsigned long BOOT_CONNECT_TIMEOUT = 10000;  // مهلة محاولة الاتصال التلقائي عند الإقلاع
String pendingSSID, pendingPassword;
bool credentialsSubmitted  = false;
bool connectingInProgress  = false;
unsigned long connectStartTime = 0;
bool freshConnection = false; // true لو توه اتصل، false لو بس ضغط الزر يعرض الآيبي
// ============================================================
//  التوقيت (NTP) - على توقيت البحرين GMT+3
// ============================================================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3 * 3600;
const int   daylightOffset_sec = 0;
// ============================================================
//  التسجيل طويل المدى (على فلاش الـ ESP32 عبر LittleFS)
// ============================================================
#define LOG_FILE "/log.csv"
const unsigned long LOG_INTERVAL = 10UL * 60UL * 1000UL; // تسجيل كل 10 دقائق
const int MAX_LOG_LINES = 20000; // تقريباً 4-5 شهور بمعدل تسجيل كل 10 دقائق قبل ما يبدأ يحذف الأقدم
unsigned long lastLogTime = 0;
// ============================================================
//  حالة الجهاز (State Machine)
// ============================================================
enum DeviceState { STATE_SENSOR, STATE_AP_CONFIG, STATE_CONNECTING, STATE_CONNECT_FAIL, STATE_SHOW_IP };
DeviceState state = STATE_SENSOR;
unsigned long stateEnteredTime = 0;
// ============================================================
//  توقيعات الدوال
// ============================================================
void tryAutoConnect();
void handleButton();
void onButtonPressed();
void startAPMode();
void stopAPMode();
void updateStateMachine();
void updateDisplay();
void showSensorScreen();
void showAPInfo();
void showConnecting();
void showConnectFail();
void showIPScreen();
void drawDegree(int x, int y);
void readSensor();
String getTimeString();
void logReading();
void trimLogIfNeeded();
void handleRoot();
void handleSave();
void handleApiCurrent();
void handleApiHistory();
void handleDownload();
String buildConfigPage();
void showResetCountdown();
void performFactoryReset();
// ============================================================
//  صفحات الويب (مخزّنة بالفلاش PROGMEM عشان توفر رام)
// ============================================================
// ملاحظة: صفحة الإعداد صارت تُبنى ديناميكياً بدالة buildConfigPage() تحت،
// عشان نقدر نعرض فيها اسم شبكة الـ AP العشوائي الحالي (currentAPSSID).
const char CONNECTING_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta http-equiv="refresh" content="4">
<style>body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
background:#0d0f13;color:#e8e8e8;font-family:-apple-system,Segoe UI,Arial,sans-serif;text-align:center}
</style></head><body>
<div><h2 style="color:#d4af37">Connecting...</h2>
<p style="color:#8a93a6">Check the device screen for status.</p></div>
</body></html>
)HTMLPAGE";
const char DASHBOARD_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>IoT Weather Hub</title>
<style>
:root{--bg:#0d0f13;--card:#171a21;--border:#262b35;--accent:#d4af37;--accent2:#4aa3ff;
--text:#e8e8e8;--muted:#8a93a6}
*{box-sizing:border-box}
body{margin:0;padding:24px;background:var(--bg);color:var(--text);
font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif}
h1{font-size:20px;letter-spacing:.5px;margin:0 0 20px;color:var(--accent);
text-transform:uppercase;font-weight:700}
.cards{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:20px}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;
padding:16px 22px;flex:1;min-width:140px}
.card .label{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:1px}
.card .value{font-family:ui-monospace,'SF Mono',Consolas,monospace;font-size:26px;
font-weight:600;color:var(--accent);margin-top:4px;font-variant-numeric:tabular-nums}
.card.hum .value{color:var(--accent2)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#3ddc84;
margin-right:6px;animation:pulse 2s infinite}
@keyframes pulse{
0%{box-shadow:0 0 0 0 rgba(61,220,132,.5)}
70%{box-shadow:0 0 0 8px rgba(61,220,132,0)}
100%{box-shadow:0 0 0 0 rgba(61,220,132,0)}}
canvas{background:var(--card);border:1px solid var(--border);border-radius:10px;
width:100%;max-width:720px;height:220px;display:block;margin-bottom:8px}
.legend{font-size:12px;color:var(--muted);margin-bottom:20px}
.legend span{margin-right:16px}
.sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px}
table{width:100%;max-width:720px;border-collapse:collapse;
font-family:ui-monospace,'SF Mono',Consolas,monospace;font-size:12px}
th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--border)}
th{color:var(--muted);font-weight:600;text-transform:uppercase;font-size:10px;letter-spacing:.5px}
a.dl{color:var(--accent2);font-size:12px;text-decoration:none}
a.dl:hover{text-decoration:underline}
</style></head><body>
<h1>IoT Weather Hub</h1>
<div class="cards">
<div class="card"><div class="label">Temperature</div><div class="value" id="t">--</div></div>
<div class="card hum"><div class="label">Humidity</div><div class="value" id="h">--</div></div>
<div class="card"><div class="label"><span class="dot"></span>Last Update</div>
<div class="value" id="u" style="font-size:14px">--</div></div>
</div>
<canvas id="chart"></canvas>
<div class="legend">
<span><i class="sw" style="background:#d4af37"></i>Temp</span>
<span><i class="sw" style="background:#4aa3ff"></i>Humidity</span>
</div>
<p><a class="dl" href="/download">Download full log (CSV)</a></p>
<table id="tbl"><thead><tr><th>Time</th><th>Temp C</th><th>Hum %</th></tr></thead>
<tbody></tbody></table>
<script>
async function loadCurrent(){
const r=await fetch('/api/current');const d=await r.json();
document.getElementById('t').textContent=d.temp.toFixed(1)+' C';
document.getElementById('h').textContent=d.hum.toFixed(1)+' %';
document.getElementById('u').textContent=d.time;
}
async function loadHistory(){
const r=await fetch('/api/history');const rows=await r.json();
const tbody=document.querySelector('#tbl tbody');tbody.innerHTML='';
const rev=rows.slice().reverse().slice(0,100);
for(const row of rev){
const tr=document.createElement('tr');
tr.innerHTML='<td>'+row.time+'</td><td>'+row.temp.toFixed(1)+'</td><td>'+row.hum.toFixed(1)+'</td>';
tbody.appendChild(tr);
}
drawChart(rows);
}
function drawChart(rows){
const c=document.getElementById('chart');const ctx=c.getContext('2d');
c.width=c.clientWidth;c.height=c.clientHeight;ctx.clearRect(0,0,c.width,c.height);
if(rows.length<2)return;
const pad=24;
function plot(vals,color){
const min=Math.min(...vals),max=Math.max(...vals);
ctx.beginPath();ctx.strokeStyle=color;ctx.lineWidth=2;
vals.forEach((v,i)=>{
const x=pad+(i/(vals.length-1))*(c.width-2*pad);
const y=c.height-pad-((v-min)/((max-min)||1))*(c.height-2*pad);
i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);
});
ctx.stroke();
}
plot(rows.map(r=>r.temp),'#d4af37');
plot(rows.map(r=>r.hum),'#4aa3ff');
}
loadCurrent();loadHistory();
setInterval(loadCurrent,5000);
setInterval(loadHistory,60000);
</script></body></html>
)HTMLPAGE";
// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(5000); // تأخير 5 ثواني حسب طلبك عشان تلحق تفتح الـ Serial Monitor وتشوف كل شيء
  Serial.println(F("\n\n=== Booting IoT Weather Hub ==="));
  dht.begin();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("فشل تشغيل الشاشة!"));
    for (;;);
  }
  Serial.println(F("OLED OK"));
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();
  LittleFS.begin(true); // true = فورمات تلقائي لو أول مرة
  prefs.begin("wifi", false);
  // شاشة انتظار بسيطة أثناء محاولة الاتصال التلقائي بالشبكة المحفوظة
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(5, 25);
  display.println(F("Connecting to WiFi..."));
  display.display();
  WiFi.persistent(false); // نتحكم بالحفظ بأنفسنا عبر Preferences
  tryAutoConnect();
  Serial.println(WiFi.status() == WL_CONNECTED
                  ? "WiFi: connected, IP=" + WiFi.localIP().toString()
                  : "WiFi: not connected (no saved network, or auto-connect failed)");
  Serial.println(F("Registering route: /"));
  server.on("/", handleRoot);
  Serial.println(F("Registering route: /save"));
  server.on("/save", HTTP_POST, handleSave);
  Serial.println(F("Registering route: /api/current"));
  server.on("/api/current", handleApiCurrent);
  Serial.println(F("Registering route: /api/history"));
  server.on("/api/history", handleApiHistory);
  Serial.println(F("Registering route: /download"));
  server.on("/download", handleDownload);
  Serial.println(F("Registering onNotFound"));
  server.onNotFound(handleRoot); // يخلي أي رابط ثاني يرجع لصفحة الإعداد (بوابة أسيرة)
  Serial.println(F("Routes done. Calling server.begin()..."));
  server.begin();
  Serial.println(F("server.begin() returned OK"));
  Serial.print(F("Button pin: "));
  Serial.println(BUTTON_PIN);
  Serial.println(F("=== Setup done, entering loop() ==="));
}
// ============================================================
//  LOOP
// ============================================================
void loop() {
  unsigned long now = millis();
  handleButton();
  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    readSensor();
  }
  if (state == STATE_AP_CONFIG || state == STATE_CONNECTING || state == STATE_CONNECT_FAIL) {
    dnsServer.processNextRequest();
  }
  server.handleClient();
  updateStateMachine();
  if (now - lastDisplayTime >= DISPLAY_INTERVAL) {
    lastDisplayTime = now;
    updateDisplay();
  }
  if (WiFi.status() == WL_CONNECTED && (now - lastLogTime >= LOG_INTERVAL)) {
    lastLogTime = now;
    logReading();
  }
}
// ============================================================
//  الاتصال التلقائي عند الإقلاع (لو فيه شبكة محفوظة)
// ============================================================
void tryAutoConnect() {
  WiFi.mode(WIFI_STA);
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  if (savedSSID.length() == 0) return;
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < BOOT_CONNECT_TIMEOUT) {
    delay(200); // انتظار لمرة وحدة بس أثناء الإقلاع، مو داخل loop()
  }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
}
// ============================================================
//  الزر
// ============================================================
void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastRawState) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        // بداية ضغطة جديدة
        pressStartTime = millis();
        isHolding = true;
        onButtonPressed(); // نفس سلوك الضغطة القصيرة يشتغل فوراً زي المعتاد
      } else {
        // تحرير الزر -> نلغي أي عد تنازلي شغال لو سحب قبل ما يخلص
        isHolding = false;
        resetCountdownActive = false;
      }
    }
  }
  if (isHolding) {
    unsigned long heldFor = millis() - pressStartTime;
    if (heldFor >= HOLD_DISPLAY_THRESHOLD) {
      resetCountdownActive = true;
      if (heldFor >= RESET_HOLD_TIME) {
        performFactoryReset();
      }
    }
  }
  lastRawState = reading;
}
void onButtonPressed() {
  Serial.println(F(">> Button press detected"));
  if (state == STATE_AP_CONFIG) {
    // إلغاء وضع الإعداد والرجوع لوضع القراءة العادي
    stopAPMode();
    state = STATE_SENSOR;
  }
  else if (WiFi.status() == WL_CONNECTED) {
    // متصل أصلاً -> بس اعرض الآيبي مؤقتاً
    freshConnection = false;
    state = STATE_SHOW_IP;
    stateEnteredTime = millis();
  }
  else if (state == STATE_SENSOR) {
    // مو متصل -> ابدأ وضع الإعداد (شبكة + بوابة أسيرة)
    startAPMode();
    state = STATE_AP_CONFIG;
    stateEnteredTime = millis();
  }
}
// ============================================================
//  وضع نقطة الوصول (AP) + البوابة الأسيرة
// ============================================================
void startAPMode() {
  randomSeed(micros()); // بذرة بسيطة عشان الاسم يتغير كل مره
  int suffix = random(1000, 9999);
  currentAPSSID = String(AP_SSID_BASE) + "-" + String(suffix);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(currentAPSSID.c_str(), AP_PASSWORD);
  dnsServer.start(53, "*", apIP);
}
void stopAPMode() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
  }
}
// ============================================================
//  ماكينة الحالة (State Machine)
// ============================================================
void updateStateMachine() {
  switch (state) {
    case STATE_CONNECTING:
      if (credentialsSubmitted && !connectingInProgress) {
        WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str());
        connectStartTime = millis();
        connectingInProgress = true;
      }
      if (connectingInProgress) {
        if (WiFi.status() == WL_CONNECTED) {
          prefs.putString("ssid", pendingSSID);
          prefs.putString("pass", pendingPassword);
          stopAPMode();
          configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
          connectingInProgress = false;
          credentialsSubmitted = false;
          freshConnection = true;
          state = STATE_SHOW_IP;
          stateEnteredTime = millis();
        } else if (millis() - connectStartTime > CONNECT_TIMEOUT) {
          connectingInProgress = false;
          credentialsSubmitted = false;
          state = STATE_CONNECT_FAIL;
          stateEnteredTime = millis();
        }
      }
      break;
    case STATE_CONNECT_FAIL:
      if (millis() - stateEnteredTime > 4000) {
        // نرجع لصفحة الإعداد (الشبكة لسه شغالة عشان يعيد المحاولة بسهولة)
        state = STATE_AP_CONFIG;
        stateEnteredTime = millis();
      }
      break;
    case STATE_SHOW_IP:
      if (millis() - stateEnteredTime > 6000) {
        state = STATE_SENSOR;
      }
      break;
    default:
      break;
  }
}
// ============================================================
//  الشاشة (OLED) - موزّع الحالات
// ============================================================
void updateDisplay() {
  if (resetCountdownActive) {
    showResetCountdown();
    return;
  }
  switch (state) {
    case STATE_AP_CONFIG:    showAPInfo();      break;
    case STATE_CONNECTING:   showConnecting();  break;
    case STATE_CONNECT_FAIL: showConnectFail(); break;
    case STATE_SHOW_IP:      showIPScreen();    break;
    case STATE_SENSOR:
    default:                 showSensorScreen(); break;
  }
}
void showSensorScreen() {
  display.clearDisplay();
  if (!hasValidReading || failCount >= MAX_FAILS) {
    display.setTextSize(1);
    display.setCursor(0, 25);
    display.println(F("Sensor Error!"));
    display.setCursor(0, 40);
    display.println(F("Check DHT22 wiring"));
    display.display();
    return;
  }
  display.setTextSize(1);
  display.setCursor(31, 2);
  display.print(F("WEATHER HUB"));
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 20);
  display.print(lastTemp, 1);
  int afterTemp = display.getCursorX();
  drawDegree(afterTemp, 20);
  display.setCursor(afterTemp + 8, 20);
  display.print("C");
  display.setCursor(0, 44);
  display.print(lastHum, 1);
  display.print(" %");
  // مؤشر صغير لحالة الواي فاي بأعلى يمين الشاشة
  display.setTextSize(1);
  display.setCursor(112, 2);
  display.print(WiFi.status() == WL_CONNECTED ? "W" : "-");
  if (failCount > 0) {
    display.setCursor(96, 44);
    display.print(F("old"));
  }
  display.display();
}
void showAPInfo() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(28, 0);
  display.print(F("SETUP MODE"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(0, 16);
  display.print(F("SSID: "));
  display.println(currentAPSSID);
  display.setCursor(0, 30);
  display.print(F("PASS: "));
  display.println(AP_PASSWORD);
  display.setCursor(0, 44);
  display.print(F("URL:  "));
  display.println(WiFi.softAPIP());
  display.setCursor(2, 55);
  display.print(F("Hold btn 15s = reset"));
  display.display();
}
void showConnecting() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(30, 0);
  display.print(F("CONNECTING"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(0, 26);
  display.println(F("Joining network:"));
  display.setCursor(0, 40);
  display.println(pendingSSID);
  display.display();
}
void showConnectFail() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(13, 0);
  display.print(F("CONNECTION FAILED"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(0, 26);
  display.println(F("Check the password"));
  display.setCursor(0, 40);
  display.println(F("Retrying setup..."));
  display.display();
}
void showIPScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(28, 0);
  display.print(freshConnection ? F("CONNECTED") : F("DEVICE IP"));
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  display.setCursor(20, 32);
  display.println(WiFi.localIP());
  display.display();
}
void showResetCountdown() {
  unsigned long heldFor = millis() - pressStartTime;
  long remainingMs = (long)RESET_HOLD_TIME - (long)heldFor;
  int remaining = remainingMs > 0 ? (remainingMs / 1000) + 1 : 0;
  String remainStr = String(remaining);
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(22, 2);
  display.print(F("HOLD FOR RESET"));
  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);
  display.setTextSize(3);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(remainStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 30);
  display.print(remainStr);
  display.display();
}
void performFactoryReset() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(14, 26);
  display.print(F("Factory Reset..."));
  display.display();
  prefs.remove("ssid");
  prefs.remove("pass");
  LittleFS.remove(LOG_FILE);
  delay(1200);
  ESP.restart();
}
void drawDegree(int x, int y) {
  display.drawCircle(x + 2, y + 2, 2, SSD1306_WHITE);
}
// ============================================================
//  قراءة الحساس
// ============================================================
void readSensor() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    failCount++;
    Serial.println(F("فشل قراءة الحساس - سيتم استخدام آخر قيمة صحيحة إن وجدت"));
    return;
  }
  failCount = 0;
  lastTemp = t;
  lastHum  = h;
  hasValidReading = true;
  if (t < minTemp) minTemp = t;
  if (t > maxTemp) maxTemp = t;
  if (h < minHum)  minHum  = h;
  if (h > maxHum)  maxHum  = h;
}
// ============================================================
//  الوقت والتسجيل طويل المدى
// ============================================================
String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return "unsynced-" + String(millis() / 1000) + "s";
  }
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}
void logReading() {
  if (!hasValidReading) return;
  time_t now = time(nullptr);
  if (now < 100000) return; // الوقت ما تزامن بعد مع NTP، تجاهل التسجيل حالياً
  String line = getTimeString() + "," + String(lastTemp, 1) + "," + String(lastHum, 1) + "\n";
  File f = LittleFS.open(LOG_FILE, "a");
  if (f) {
    f.print(line);
    f.close();
  }
  trimLogIfNeeded();
}
void trimLogIfNeeded() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) return;
  int lineCount = 0;
  while (f.available()) {
    f.readStringUntil('\n');
    lineCount++;
  }
  f.close();
  if (lineCount <= MAX_LOG_LINES) return;
  // نحتفظ بآخر نصف السجل بس (يقلل عدد مرات إعادة الكتابة على الفلاش)
  int keepFrom = lineCount - (MAX_LOG_LINES / 2);
  File src = LittleFS.open(LOG_FILE, "r");
  File tmp = LittleFS.open("/log_tmp.csv", "w");
  if (!src || !tmp) {
    if (src) src.close();
    if (tmp) tmp.close();
    return;
  }
  int idx = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (idx >= keepFrom && line.length() > 0) {
      tmp.println(line);
    }
    idx++;
  }
  src.close();
  tmp.close();
  LittleFS.remove(LOG_FILE);
  LittleFS.rename("/log_tmp.csv", LOG_FILE);
}
// ============================================================
//  السيرفر (صفحة الإعداد أو الداشبورد + API)
// ============================================================
String buildConfigPage() {
  String html = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Connect Device</title><style>
:root{--bg:#0b0d11;--panel:#14171d;--border:#232830;--gold:#d4af37;--text:#e8e8e8;--muted:#7a8494}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;
background:var(--bg);color:var(--text);font-family:-apple-system,Segoe UI,Arial,sans-serif;padding:20px}
.card{background:var(--panel);border:1px solid var(--border);border-radius:14px;
padding:32px 28px;width:100%;max-width:320px;text-align:center}
.ping-icon{margin-bottom:6px}
.ring{transform-origin:50px 50px;animation:ping 2.4s ease-out infinite;opacity:0}
.r2{animation-delay:.6s}
.r3{animation-delay:1.2s}
@keyframes ping{
0%{transform:scale(0.4);opacity:.9}
80%{opacity:0}
100%{transform:scale(1.8);opacity:0}
}
h1{font-size:18px;margin:6px 0 2px;letter-spacing:.3px;font-weight:700}
.ap-tag{font-family:ui-monospace,'SF Mono',Consolas,monospace;font-size:11px;
color:var(--muted);margin-bottom:22px;letter-spacing:.3px}
.ap-tag b{color:var(--gold);font-weight:600}
form{text-align:left}
label{display:block;font-family:ui-monospace,'SF Mono',Consolas,monospace;
font-size:10px;color:var(--muted);letter-spacing:1px;margin:14px 0 6px;text-transform:uppercase}
input{width:100%;padding:12px;border-radius:8px;border:1px solid var(--border);
background:#0b0d11;color:var(--text);font-size:15px;box-sizing:border-box}
input:focus{outline:none;border-color:var(--gold)}
button{width:100%;padding:13px;margin-top:22px;background:var(--gold);color:#111;
border:none;border-radius:8px;font-weight:700;font-size:14px;letter-spacing:.4px;
text-transform:uppercase;cursor:pointer}
button:active{opacity:.85}
.hint{margin-top:16px;font-size:11px;color:var(--muted);line-height:1.5}
</style></head><body>
<div class="card">
<svg class="ping-icon" viewBox="0 0 100 100" width="72" height="72">
<circle cx="50" cy="50" r="6" fill="var(--gold)"/>
<circle class="ring r1" cx="50" cy="50" r="16" fill="none" stroke="var(--gold)" stroke-width="2.5"/>
<circle class="ring r2" cx="50" cy="50" r="16" fill="none" stroke="var(--gold)" stroke-width="2.5"/>
<circle class="ring r3" cx="50" cy="50" r="16" fill="none" stroke="var(--gold)" stroke-width="2.5"/>
</svg>
<h1>Connect This Device</h1>
<div class="ap-tag">AP: <b>)HTML";
  html += currentAPSSID;
  html += R"HTML(</b></div>
<form action="/save" method="POST">
<label>Network Name</label>
<input name="ssid" placeholder="Your WiFi name" required autofocus>
<label>Password</label>
<input name="pass" type="password" placeholder="Your WiFi password">
<button type="submit">Connect</button>
</form>
<div class="hint">Your network details stay on this device.</div>
</div></body></html>
)HTML";
  return html;
}
void handleRoot() {
  if (state == STATE_AP_CONFIG || state == STATE_CONNECTING || state == STATE_CONNECT_FAIL) {
    server.send(200, "text/html", buildConfigPage());
  } else {
    server.send_P(200, "text/html", DASHBOARD_HTML);
  }
}
void handleSave() {
  if (server.hasArg("ssid") && server.arg("ssid").length() > 0) {
    pendingSSID = server.arg("ssid");
    pendingPassword = server.arg("pass");
    credentialsSubmitted = true;
    state = STATE_CONNECTING;
    stateEnteredTime = millis();
    server.send_P(200, "text/html", CONNECTING_HTML);
  } else {
    server.send(400, "text/plain", "Missing WiFi name");
  }
}
void handleApiCurrent() {
  String json = "{";
  if (hasValidReading) {
    json += "\"temp\":" + String(lastTemp, 1) + ",";
    json += "\"hum\":" + String(lastHum, 1) + ",";
  } else {
    json += "\"temp\":0,\"hum\":0,";
  }
  json += "\"time\":\"" + getTimeString() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}
void handleApiHistory() {
  const int MAXN = 200; // آخر 200 قراءة تُعرض بالداشبورد مباشرة (السجل الكامل عبر /download)
  static String buf[MAXN];
  int totalSeen = 0;
  File f = LittleFS.open(LOG_FILE, "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      buf[totalSeen % MAXN] = line;
      totalSeen++;
    }
    f.close();
  }
  int count = (totalSeen < MAXN) ? totalSeen : MAXN;
  int start = (totalSeen > MAXN) ? (totalSeen % MAXN) : 0;
  String json = "[";
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAXN;
    String line = buf[idx];
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) continue;
    String t = line.substring(0, c1);
    String temp = line.substring(c1 + 1, c2);
    String hum = line.substring(c2 + 1);
    if (i > 0) json += ",";
    json += "{\"time\":\"" + t + "\",\"temp\":" + temp + ",\"hum\":" + hum + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}
void handleDownload() {
  File f = LittleFS.open(LOG_FILE, "r");
  if (!f) {
    server.send(404, "text/plain", "No log file yet");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=sensor_log.csv");
  server.streamFile(f, "text/csv");
  f.close();
}
