#include "portal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "alerts.h"
#include "areas_data.h"
#include "led.h"
#include "settings.h"
#include "version.h"

namespace {

DNSServer dns;
WebServer web(80);
bool running = false;

const char* AP_SSID = "Homefront-Alert-Lamp";
IPAddress apIP(192, 168, 4, 1);

String areasJson;  // built once in begin(): {"d":[districts],"a":[[d,"label"],…]}

String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { out += ' '; }
    else out += c;
  }
  return out;
}

void buildAreasJson() {
  areasJson = "{\"d\":[";
  for (int i = 0; i < AREAS_DISTRICT_COUNT; i++) {
    if (i) areasJson += ',';
    areasJson += '"';
    areasJson += jsonEscape(AREAS_DISTRICTS[i]);
    areasJson += '"';
  }
  areasJson += "],\"a\":[";
  for (int i = 0; i < AREAS_COUNT; i++) {
    if (i) areasJson += ',';
    areasJson += '[';
    areasJson += String(AREAS[i].district);
    areasJson += ",\"";
    areasJson += jsonEscape(AREAS[i].label);
    areasJson += "\"]";
  }
  areasJson += "]}";
}

// ──────────────────────────────────────────────
// Routes
// ──────────────────────────────────────────────

void handleAreas() {
  if (!areasJson.length()) buildAreasJson();
  web.send(200, "application/json; charset=utf-8", areasJson);
}

void handleScan() {
  // Channel hopping during the scan can briefly disturb the AP client;
  // the UI tells the user to re-tap the button if the reply is lost.
  int n = WiFi.scanNetworks(false, true);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 40; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["sec"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json; charset=utf-8", out);
}

void handleStatus() {
  JsonDocument doc;
  doc["version"] = FW_VERSION;
  doc["ssid"] = g_settings.ssid;
  doc["hasPass"] = g_settings.pass.length() > 0;
  doc["led_pin"] = g_settings.ledPin;
  doc["led_count"] = g_settings.ledCount;
  doc["led_bri"] = g_settings.ledBri;

  JsonArray p = doc["primary"].to<JsonArray>();
  for (const String& a : g_settings.primary) p.add(a);
  JsonArray s = doc["secondary"].to<JsonArray>();
  for (const String& a : g_settings.secondary) s.add(a);

  doc["wifi_status"] = (int)WiFi.status();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();

  String a = alerts::status();
  JsonDocument ad;
  if (!deserializeJson(ad, a)) doc["alerts"] = ad.as<JsonVariant>();

  String out;
  serializeJson(doc, out);
  web.send(200, "application/json; charset=utf-8", out);
}

void handleSave() {
  String ssid = web.arg("ssid");
  ssid.trim();
  String pass = web.arg("pass");

  if (ssid.length()) g_settings.ssid = ssid;   // empty field → keep existing
  if (pass.length()) g_settings.pass = pass;

  if (web.hasArg("areas")) {
    JsonDocument doc;
    if (!deserializeJson(doc, web.arg("areas"))) {
      std::vector<String> p, s;
      for (JsonVariant v : doc["p"].as<JsonArray>()) p.push_back(v.as<String>());
      for (JsonVariant v : doc["s"].as<JsonArray>()) s.push_back(v.as<String>());
      g_settings.primary = p;
      g_settings.secondary = s;
    }
  }

  if (web.hasArg("led_pin")) g_settings.ledPin = constrain(web.arg("led_pin").toInt(), 0, 48);
  if (web.hasArg("led_count")) g_settings.ledCount = constrain(web.arg("led_count").toInt(), 1, 1024);
  if (web.hasArg("led_bri")) g_settings.ledBri = constrain(web.arg("led_bri").toInt(), 1, 100);

  if (!g_settings.hasWifi()) {
    web.send(400, "text/plain; charset=utf-8", "חסר שם רשת WiFi");
    return;
  }

  cfg::save();
  Serial.println("[portal] saved: " + cfg::dump());

  web.send(200, "text/html; charset=utf-8",
           "<!DOCTYPE html><html lang='he'><head><meta charset='utf-8'></head>"
           "<body dir='rtl' style='font-family:sans-serif;padding:30px'>"
           "<h3>ההגדרות נשמרו</h3><p>המכשיר מפעיל מחדש ומתחבר לרשת…</p></body></html>");
  delay(700);
  ESP.restart();
}

void handleTest() {
  // Apply whatever is currently typed in the LED fields, so wiring can be
  // tested before (or without) saving and rebooting.
  if (web.hasArg("pin") && web.hasArg("count") && web.hasArg("bri")) {
    int pin = constrain(web.arg("pin").toInt(), 0, 48);
    int count = constrain(web.arg("count").toInt(), 1, 1024);
    int bri = constrain(web.arg("bri").toInt(), 1, 100);
    if (pin != g_settings.ledPin || count != g_settings.ledCount || bri != g_settings.ledBri) {
      led::begin(pin, count, bri);  // re-init the driver with the form's values
    }
  }

  String m = web.arg("m");
  if (m == "primary") led::overrideFor(LedMode::ALERT_PRIMARY, 6000);
  else if (m == "secondary") led::overrideFor(LedMode::ALERT_SECONDARY, 6000);
  else if (m == "pixels") led::overrideFor(LedMode::TEST, 8000);
  else if (m == "off") led::overrideFor(LedMode::OFF, 3000);
  else led::overrideFor(LedMode::PORTAL, 3000);
  web.send(200, "application/json", "{\"ok\":true}");
}

void handleReset() {
  cfg::factoryReset();
  web.send(200, "text/html; charset=utf-8",
           "<!DOCTYPE html><html lang='he'><head><meta charset='utf-8'></head>"
           "<body dir='rtl' style='font-family:sans-serif;padding:30px'>"
           "<h3>ההגדרות נמחקו</h3><p>מפעיל מחדש…</p></body></html>");
  delay(700);
  ESP.restart();
}

void handleNotFound() {
  web.sendHeader("Location", String("http://") + apIP.toString() + "/", true);
  web.send(302, "text/plain", "");
}

const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="he"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>הגדרת מנורת התרעה</title>
<style>
*{box-sizing:border-box}
body{font-family:system-ui,"Segoe UI",Arial,sans-serif;margin:0;background:#f5f6f8;color:#1b1f24;direction:rtl}
header{background:#171a1f;color:#fff;padding:13px 16px;display:flex;justify-content:space-between;align-items:center}
main{max-width:760px;margin:0 auto;padding:12px}
section{background:#fff;border:1px solid #e3e6ea;border-radius:10px;padding:14px;margin-bottom:12px}
h2{margin:0 0 8px;font-size:16px}
label{display:block;margin:9px 0 3px;font-weight:600;font-size:14px}
input,select{width:100%;padding:9px;border:1px solid #ccd2d9;border-radius:6px;font-size:15px}
button{padding:10px 14px;border:0;border-radius:6px;background:#2f6fed;color:#fff;font-size:15px;cursor:pointer}
button:disabled{opacity:.5}
.ghost{background:#e9edf2;color:#222}
.danger{background:#e63946}
.grid{display:flex;gap:8px}.grid>div{flex:1}
#list{max-height:52vh;overflow:auto;border:1px solid #e6e9ee;border-radius:8px;padding:6px;background:#fbfcfd}
details{border-bottom:1px solid #eceff3;padding:4px 2px}
summary{cursor:pointer;font-weight:700;padding:5px 2px}
.row{display:flex;gap:8px;align-items:center;padding:5px 4px}
.row input[type=checkbox]{width:18px;height:18px;flex:0 0 auto}
.row span{flex:1;font-size:15px}
.row select{width:104px;flex:0 0 auto}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.msg{min-height:20px;margin-top:10px;font-size:14px;color:#0a7a32}
.err{color:#c1121f}
#status{font-size:13px;white-space:pre-wrap;background:#f7f8fa;border:1px solid #e6e9ee;border-radius:8px;padding:8px;max-height:180px;overflow:auto}
.hint{font-size:13px;color:#5b6470}
</style></head><body>
<header><b>מנורת התרעה – הגדרה</b><span id="ver"></span></header>
<main>

<section>
  <h2>1. חיבור לרשת WiFi</h2>
  <button class="ghost" id="scanBtn" onclick="scan()">סרוק רשתות</button>
  <label>רשתות שנמצאו</label>
  <select id="nets" onchange="pickNet()"><option value="">—</option></select>
  <label>שם רשת (SSID)</label>
  <input type="text" id="ssid" autocomplete="off" spellcheck="false">
  <label>סיסמה</label>
  <input type="password" id="pass" autocomplete="off" placeholder="השאר ריק כדי להשאיר את הסיסמה השמורה">
  <div class="hint">הפירוק והחיבור מחדש של הרשת עלולים להפיל לרגע את הדפדפן – אפשר ללחוץ שוב על "סרוק".</div>
</section>

<section>
  <h2>2. אזורי התרעה</h2>
  <input type="text" id="q" placeholder="חיפוש אזור…" oninput="renderAreas()">
  <div class="hint">סמן אזורים והערם כל אחד כ<b>ראשי</b> (דפוס חירום) או <b>משני</b> (דפוס רגוע).</div>
  <div id="list">טוען…</div>
  <div class="actions">
    <button class="ghost" onclick="selectAllVisible(true)">סמן את כל מה שמוצג</button>
    <button class="ghost" onclick="selectAllVisible(false)">נקה סימון</button>
  </div>
</section>

<section>
  <h2>3. נורית / פס LED</h2>
  <div class="grid">
    <div><label>פין נתונים</label><input type="number" id="led_pin" min="0" max="48"></div>
    <div><label>מספר פיקסלים</label><input type="number" id="led_count" min="1" max="1024"></div>
    <div><label>בהירות %</label><input type="number" id="led_bri" min="1" max="100"></div>
  </div>
  <div class="hint">ברירת מחדל: GPIO8 = הנורית הצבעונית שעל לוח ה־C6. לפס חיצוני שנה לפין שחוברת אליו.</div>
  <div class="actions">
    <button class="danger" onclick="test('primary')">בדיקת דפוס ראשי</button>
    <button class="ghost" onclick="test('secondary')">דפוס משני</button>
    <button class="ghost" onclick="test('pixels')">בדיקת פיקסלים</button>
  </div>
</section>

<section>
  <h2>4. שמירה</h2>
  <div class="actions">
    <button onclick="save()">שמור והפעל מחדש</button>
    <button class="danger" onclick="resetCfg()">איפוס הגדרות</button>
    <button class="ghost" onclick="showStatus()">מצב נוכחי</button>
  </div>
  <div class="msg" id="msg"></div>
  <div id="status" style="display:none"></div>
</section>

</main>
<script>
const $=s=>document.querySelector(s);
let AREAS=null;
const jget=async u=>(await fetch(u,{cache:'no-store'})).json();
const msg=(t,cls='')=>{const e=$('#msg');e.textContent=t;e.className='msg '+cls;};

async function init(){
  try{
    const st=await jget('/status');
    $('#ver').textContent='v'+(st.version||'?');
    $('#ssid').value=st.ssid||'';
    $('#led_pin').value=st.led_pin; $('#led_count').value=st.led_count; $('#led_bri').value=st.led_bri;
    AREAS={p:new Set(st.primary||[]),s:new Set(st.secondary||[])};
  }catch(e){msg('טעינת סטטוס נכשלה','err');}
  await loadAreas();
}
async function loadAreas(){
  try{
    const d=await jget('/areas.json');
    AREAS=AREAS||{p:new Set(),s:new Set()};
    AREAS.list=d; renderAreas(); markSelection();
  }catch(e){$('#list').textContent='טעינת האזורים נכשלה';}
}
function renderAreas(){
  if(!AREAS||!AREAS.list)return;
  const q=($('#q').value||'').trim();
  const box=$('#list'); box.innerHTML='';
  let shown=0;
  AREAS.list.d.forEach((d,di)=>{
    const items=[];
    for(const a of AREAS.list.a){ if(a[0]!==di)continue; if(q&&a[1].indexOf(q)<0)continue; items.push(a[1]); }
    if(!items.length)return;
    shown+=items.length;
    const det=document.createElement('details');
    const sm=document.createElement('summary'); sm.textContent=d+' ('+items.length+')';
    det.appendChild(sm);
    for(const label of items){
      const row=document.createElement('label'); row.className='row';
      const cb=document.createElement('input'); cb.type='checkbox'; cb.dataset.l=label;
      const sp=document.createElement('span'); sp.textContent=label;
      const sel=document.createElement('select');
      sel.innerHTML='<option value="">—</option><option value="p">ראשי</option><option value="s">משני</option>';
      row.appendChild(cb); row.appendChild(sp); row.appendChild(sel);
      det.appendChild(row);
    }
    box.appendChild(det);
  });
  if(!shown)box.innerHTML='<div class="hint">לא נמצאו אזורים</div>';
}
function markSelection(){
  document.querySelectorAll('#list input[type=checkbox]').forEach(cb=>{
    const l=cb.dataset.l;
    const sel=cb.parentElement.querySelector('select');
    if(AREAS.p.has(l)){cb.checked=true;sel.value='p';}
    else if(AREAS.s.has(l)){cb.checked=true;sel.value='s';}
  });
}
function selected(){
  const p=[],s=[];
  document.querySelectorAll('#list input[type=checkbox]:checked').forEach(cb=>{
    const v=cb.parentElement.querySelector('select').value;
    if(v==='p')p.push(cb.dataset.l); else if(v==='s')s.push(cb.dataset.l);
  });
  return {p,s};
}
function selectAllVisible(on){
  document.querySelectorAll('#list input[type=checkbox]').forEach(cb=>{
    cb.checked=on;
    if(on&&!cb.parentElement.querySelector('select').value)
      cb.parentElement.querySelector('select').value='p';
  });
}
async function scan(){
  const b=$('#scanBtn'); b.disabled=true; msg('סורק רשתות…');
  try{
    const nets=await jget('/scan.json');
    const sel=$('#nets'); sel.innerHTML='<option value="">—</option>';
    nets.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
      const o=document.createElement('option'); o.value=n.ssid;
      o.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.sec?' 🔒':'');
      sel.appendChild(o);
    });
    msg('נמצאו '+nets.length+' רשתות');
  }catch(e){msg('הסריקה נקלטה אבל התשובה לא חזרה – נסה שוב','err');}
  b.disabled=false;
}
function pickNet(){const v=$('#nets').value; if(v)$('#ssid').value=v;}
async function test(m){
  const q=new URLSearchParams({m:m,pin:$('#led_pin').value,count:$('#led_count').value,bri:$('#led_bri').value});
  try{await fetch('/test?'+q); msg('מריץ בדיקה עם הערכים שמופיעים בטופס…');}catch(e){}
}
async function save(){
  const sel=selected();
  const body=new URLSearchParams({
    ssid:$('#ssid').value, pass:$('#pass').value,
    areas:JSON.stringify(sel),
    led_pin:$('#led_pin').value, led_count:$('#led_count').value, led_bri:$('#led_bri').value
  });
  msg('שומר…');
  try{
    const r=await fetch('/save',{method:'POST',body});
    const t=await r.text();
    if(r.ok)msg('נשמר! המכשיר מפעיל מחדש…');
    else msg(t.replace(/<[^>]+>/g,''),'err');
  }catch(e){msg('שמירה נכשלה','err');}
}
async function resetCfg(){
  if(!confirm('למחוק את כל ההגדרות?'))return;
  try{await fetch('/reset',{method:'POST'}); msg('נמחק – מפעיל מחדש…');}catch(e){}
}
async function showStatus(){
  try{
    const st=await jget('/status');
    const el=$('#status'); el.style.display='block';
    el.textContent=JSON.stringify(st,null,1);
  }catch(e){}
}
init();
</script>
</body></html>)HTML";

void handleRoot() {
  web.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

}  // namespace

// ──────────────────────────────────────────────
// portal
// ──────────────────────────────────────────────
namespace portal {

void begin() {
  Serial.println("[portal] starting configuration access point…");
  WiFi.mode(WIFI_AP_STA);  // STA stays available for /scan
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);

  dns.start(53, "*", apIP);

  buildAreasJson();

  web.on("/", handleRoot);
  web.on("/areas.json", handleAreas);
  web.on("/scan.json", handleScan);
  web.on("/status", handleStatus);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/test", handleTest);
  web.on("/reset", HTTP_POST, handleReset);
  web.onNotFound(handleNotFound);
  web.begin();

  running = true;
  Serial.println("[portal] AP '" + String(AP_SSID) + "' up, http://192.168.4.1/");
}

void tick() {
  if (!running) return;
  dns.processNextRequest();
  web.handleClient();
}

bool active() { return running; }

}  // namespace portal
