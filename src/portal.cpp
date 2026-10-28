#include "portal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "alerts.h"
#include "areas_data.h"
#include "led.h"
#include "log.h"
#include "ota.h"
#include "settings.h"
#include "version.h"

namespace {

DNSServer dns;
WebServer web(80);        // configuration portal
WebServer runtimeWeb(80); // status/settings UI while running on WiFi
// The same zero-arg handlers are registered on BOTH servers (so /settings,
// /dev and the live log also answer while the lamp is running); they reply
// on g_srv, the server that is currently dispatching. tick()/runtimeTick()
// and the registration helpers re-point it before use.
WebServer* g_srv = &web;
bool running = false;
bool runtimeOn = false;
bool wantPortal = false;

const char* AP_SSID = "Homefront-Alert-Lamp";
IPAddress apIP(192, 168, 4, 1);

// Log tags (see log.h)
const char* TAG = "portal";
const char* TAG_WEB = "web";

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

// WebServer handles ONE client at a time and, while waiting for the first
// request bytes, holds it for HTTP_MAX_DATA_WAIT (5 s) — long enough to stall
// every other connection. Browsers routinely open parallel sockets that send
// nothing for a while (preconnect), which used to make the page "hang forever".
// Close such idle sockets after a short grace period instead.
void dropIdleClient(WebServer& srv, uint32_t& idleSince) {
  NetworkClient& c = srv.client();
  if (c.connected() && c.available() == 0) {
    if (!idleSince) idleSince = millis();
    else if (millis() - idleSince > 500) {  // real requests arrive in ~1-5 ms
      c.stop();
      idleSince = 0;
    }
  } else {
    idleSince = 0;
  }
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

String buildStatusJson();

void handleStatus() {
  web.send(200, "application/json; charset=utf-8", buildStatusJson());
}

String buildStatusJson() {
  JsonDocument doc;
  doc["version"] = FW_VERSION;
  doc["ssid"] = g_settings.ssid;
  doc["hasPass"] = g_settings.pass.length() > 0;
  doc["led_pin"] = g_settings.ledPin;
  doc["led_count"] = g_settings.ledCount;
  doc["led_bri"] = g_settings.ledBri;
  doc["update_interval"] = g_settings.updateIntervalSec;
  doc["log_level"] = g_settings.logLevel;
  doc["heap"] = ESP.getFreeHeap();
  doc["uptime_s"] = millis() / 1000;

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

  String o = ota::status();
  JsonDocument od;
  if (!deserializeJson(od, o)) doc["ota"] = od.as<JsonVariant>();

  String out;
  serializeJson(doc, out);
  return out;
}

// ── step 1: join a WiFi network (test only – nothing is persisted here) ──
void handleConnect() {
  String ssid = web.arg("ssid");
  ssid.trim();
  String pass = web.arg("pass");
  if (!ssid.length()) {
    web.send(400, "text/plain; charset=utf-8", "חסר שם רשת");
    return;
  }
  // Empty password for the already-configured network → reuse the stored one
  // (the password fields are never pre-filled in the UI).
  if (!pass.length() && ssid == g_settings.ssid && g_settings.hasWifi())
    pass = g_settings.pass;
  LOGI(TAG, "trying to join '" + ssid + "' …");
  WiFi.mode(WIFI_AP_STA);  // the AP stays up so the page keeps answering
  WiFi.begin(ssid.c_str(), pass.c_str());
  web.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

// ── initial setup finish: WiFi + areas → save and reboot into normal mode ──
void handleSave() {
  String ssid = web.arg("ssid");
  ssid.trim();
  String pass = web.arg("pass");
  if (!ssid.length()) {
    web.send(400, "text/plain; charset=utf-8", "חסר שם רשת");
    return;
  }
  g_settings.ssid = ssid;
  // An empty password field keeps the stored one, unless the UI explicitly
  // confirmed saving an open network (clearpass=1).
  if (pass.length()) g_settings.pass = pass;
  else if (web.hasArg("clearpass")) g_settings.pass = "";

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

  cfg::save();
  LOGI(TAG, "saved: " + cfg::dump());

  web.send(200, "text/html; charset=utf-8",
           "<!DOCTYPE html><html lang='he'><head><meta charset='utf-8'></head>"
           "<body dir='rtl' style='font-family:sans-serif;padding:30px'>"
           "<h3>ההגדרות נשמרו</h3><p>המכשיר מפעיל מחדש ומתחבר לרשת…</p></body></html>");
  delay(700);
  ESP.restart();
}

// ── settings › networking: persist new credentials and reboot to apply ──
void handleSaveNet() {
  String ssid = web.arg("ssid");
  ssid.trim();
  String pass = web.arg("pass");
  if (!ssid.length()) {
    web.send(400, "text/plain; charset=utf-8", "חסר שם רשת");
    return;
  }
  g_settings.ssid = ssid;
  // Empty field → keep the stored password, unless the UI confirmed an
  // open network (clearpass=1).
  if (pass.length()) g_settings.pass = pass;
  else if (web.hasArg("clearpass")) g_settings.pass = "";
  cfg::save();
  LOGI(TAG, "wifi settings saved: " + cfg::dump());

  web.send(200, "text/html; charset=utf-8",
           "<!DOCTYPE html><html lang='he'><head><meta charset='utf-8'></head>"
           "<body dir='rtl' style='font-family:sans-serif;padding:30px'>"
           "<h3>נשמר</h3><p>המכשיר מפעיל מחדש ומתחבר לרשת החדשה…</p></body></html>");
  delay(700);
  ESP.restart();
}

// ── settings › areas: persist and apply live (matching reads the settings directly) ──
void handleSaveAreas() {
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
  cfg::save();
  LOGI(TAG, "areas saved: " + cfg::dump());
  web.send(200, "text/plain; charset=utf-8", "נשמר");
}

// ── settings › LED: brightness, applied live ──
void handleSaveLed() {
  if (web.hasArg("bri")) g_settings.ledBri = constrain(web.arg("bri").toInt(), 1, 100);
  cfg::save();
  led::begin(g_settings.ledPin, g_settings.ledCount, g_settings.ledBri);
  LOGI(TAG, "led settings saved: " + cfg::dump());
  web.send(200, "text/plain; charset=utf-8", "נשמר");
}

// ── dev page: hardware + update-interval + log-level settings, applied live ──
// Uses (*g_srv) instead of the global `web` so it works on whichever server
// is currently active (portal or runtime).
void handleSaveDev() {
  if (web.hasArg("pin")) g_settings.ledPin = constrain(web.arg("pin").toInt(), 0, 48);
  if (web.hasArg("count")) g_settings.ledCount = constrain(web.arg("count").toInt(), 1, 1024);
  if (web.hasArg("bri")) g_settings.ledBri = constrain(web.arg("bri").toInt(), 1, 100);
  if (web.hasArg("upd_int")) {
    int secs = constrain(web.arg("upd_int").toInt(), 0, 86400);
    if (secs > 0 && secs < 60) secs = 60;  // GitHub rate limit: 60 calls/hour
    g_settings.updateIntervalSec = secs;
  }
  if (web.hasArg("loglevel")) {
    g_settings.logLevel = constrain(web.arg("loglevel").toInt(), 0, 3);
    logger::setLevel((logger::Level)g_settings.logLevel);  // apply live, NVS via cfg::save()
  }
  cfg::save();
  led::begin(g_settings.ledPin, g_settings.ledCount, g_settings.ledBri);
  LOGI(TAG, "dev settings saved: " + cfg::dump());
  web.send(200, "text/plain; charset=utf-8", "נשמר");
}

// ── dev page: alert simulation through the real matching code ──
void handleSim() {
  String m = web.arg("m");
  if (m == "p") alerts::simulatePrimary();
  else if (m == "s") alerts::simulateSecondary();
  else alerts::clearSimulation();
  web.send(200, "application/json", "{\"ok\":true}");
}

// ── dev page: reboot ──
void handleRestart() {
  web.send(200, "text/plain; charset=utf-8", "restarting");
  delay(300);
  ESP.restart();
}

// ── dev page: live log – lines captured since ?since=N (0/absent = all) ──
void handleLogsJson() {
  uint32_t since = (*g_srv).hasArg("since") ? (uint32_t)(*g_srv).arg("since").toInt() : 0;
  String out;
  uint32_t dropped = 0;
  logger::since(since, out, dropped);
  (*g_srv).send(200, "application/json; charset=utf-8", out);
}

// ── dev page: live log – download the whole buffer as a text file ──
void handleLogsTxt() {
  String out;
  logger::toText(out);
  (*g_srv).sendHeader("Content-Disposition", "attachment; filename=alert-lamp.log");
  (*g_srv).send(200, "text/plain; charset=utf-8", out);
}

// ── dev page: live log – drop every captured line ──
void handleLogClear() {
  logger::clear();
  web.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleTestFor(WebServer& srv) {
  // Apply whatever is currently typed in the LED fields, so wiring can be
  // tested before (or without) saving and rebooting.
  if (srv.hasArg("pin") && srv.hasArg("count") && srv.hasArg("bri")) {
    int pin = constrain(srv.arg("pin").toInt(), 0, 48);
    int count = constrain(srv.arg("count").toInt(), 1, 1024);
    int bri = constrain(srv.arg("bri").toInt(), 1, 100);
    if (pin != g_settings.ledPin || count != g_settings.ledCount || bri != g_settings.ledBri) {
      led::begin(pin, count, bri);  // re-init the driver with the form's values
    }
  } else if (srv.hasArg("bri")) {
    // brightness-only preview (settings page): keep the saved pin/count
    int bri = constrain(srv.arg("bri").toInt(), 1, 100);
    if (bri != g_settings.ledBri) led::begin(g_settings.ledPin, g_settings.ledCount, bri);
  }

  String m = srv.arg("m");
  if (m == "primary") led::overrideFor(LedMode::ALERT_PRIMARY, 6000);
  else if (m == "secondary") led::overrideFor(LedMode::ALERT_SECONDARY, 6000);
  else if (m == "pixels") led::overrideFor(LedMode::TEST, 8000);
  else if (m == "off") led::overrideFor(LedMode::OFF, 3000);
  else led::overrideFor(LedMode::PORTAL, 3000);
  srv.send(200, "application/json", "{\"ok\":true}");
}

void handleTest() { handleTestFor(*g_srv); }

void handleUpdCheck() {
  // Queue the check; the portal loop's ota::tick(false) runs it right after
  // this response is sent, so the browser isn't left hanging on a download.
  ota::checkNow();
  web.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
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
  // Redirect to the same host the browser used (AP IP while joined to the
  // hotspot, STA IP if the portal is open while already on the home network).
  web.sendHeader("Location",
                 String("http://") + web.client().localIP().toString() + "/", true);
  web.send(302, "text/plain", "");
}

// ──────────────────────────────────────────────
// Shared stylesheet + helpers (served once, used by all portal pages)
// ──────────────────────────────────────────────

const char STYLE_CSS[] PROGMEM = R"CSS(*{box-sizing:border-box}
body{font-family:system-ui,"Segoe UI",Arial,sans-serif;margin:0;background:#f5f6f8;color:#1b1f24;direction:rtl}
header{background:#171a1f;color:#fff;padding:13px 16px;display:flex;justify-content:space-between;align-items:center}
header .tag{font-size:12px;background:#2f6fed;border-radius:10px;padding:2px 9px;margin-inline-start:8px}
main{max-width:760px;margin:0 auto;padding:12px}
section{background:#fff;border:1px solid #e3e6ea;border-radius:10px;padding:14px;margin-bottom:12px}
h2{font-size:16px;margin:0 0 10px}
label{display:block;font-size:13px;color:#66707b;margin:8px 0 3px}
input,select{width:100%;padding:9px;border:1px solid #ccd2d9;border-radius:6px;font-size:15px}
input[type=range]{padding:0}
button{padding:10px 14px;border:0;border-radius:6px;background:#2f6fed;color:#fff;font-size:15px;cursor:pointer}
button:disabled{opacity:.5}
.ghost{background:#e9edf2;color:#222}
a.ghost{display:inline-block;padding:10px 14px;border-radius:6px;background:#e9edf2;color:#222;font-size:15px;text-decoration:none}
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
pre{font-size:13px;white-space:pre-wrap;background:#f7f8fa;border:1px solid #e6e9ee;border-radius:8px;padding:8px;max-height:220px;overflow:auto;direction:ltr;text-align:left}
.hint{font-size:13px;color:#66707b}
.step{display:none}
.step.open{display:block}
.cnt{font-size:13px;color:#66707b;margin-top:6px}
/* sidebar layout (settings page) */
.layout{display:flex;gap:12px;align-items:flex-start}
.side{flex:0 0 190px;background:#fff;border:1px solid #e3e6ea;border-radius:10px;padding:8px}
.side button{display:block;width:100%;text-align:right;background:none;color:#1b1f24;padding:10px;margin-bottom:4px;border-radius:6px;font-size:15px;cursor:pointer}
.side button.on{background:#2f6fed;color:#fff}
.side hr{border:0;border-top:1px solid #eceff3;margin:8px 2px}
.side .danger{background:#e63946;color:#fff;text-align:center}
.content{flex:1;min-width:0}
@media (max-width:620px){.layout{flex-direction:column}.side{flex:0 0 auto;width:100%;display:flex;gap:6px;flex-wrap:wrap}.side button{margin:0;width:auto}}
)CSS";

void handleStyle() {
  web.send_P(200, "text/css; charset=utf-8", STYLE_CSS);
}

const char APP_JS[] PROGMEM = R"JS(// ── shared helpers (setup + settings pages) ──
const $=s=>document.querySelector(s);
const $$=s=>[...document.querySelectorAll(s)];
const jget=async u=>(await fetch(u,{cache:'no-store'})).json();
function msg(el,t,err){el.textContent=t||'';el.className='msg'+(err?' err':'');}
function jurl(d){return new URLSearchParams(d);}

// ── official oref areas list ──
let SELMAP=new Map(); // label→'p'|'s'|'' (persistent source of truth, survives re-renders)
async function loadAreas(box){
  try{
    const st=await jget('/status');
    SELMAP=new Map();
    for(const a of st.primary||[]) SELMAP.set(a,'p');
    for(const a of st.secondary||[]) SELMAP.set(a,'s');
  }catch(e){}
  try{
    SELMAP.list=await jget('/areas.json');
    renderAreas(box,'');
    markSelection();
  }catch(e){box.textContent='טעינת האזורים נכשלה';}
  return SELMAP;
}
function renderAreas(box,q){
  if(!SELMAP||!SELMAP.list)return;
  box.innerHTML='';
  let shown=0;
  SELMAP.list.d.forEach((d,di)=>{
    const items=[];
    for(const a of SELMAP.list.a){ if(a[0]!==di)continue; if(q&&a[1].indexOf(q)<0)continue; items.push(a[1]); }
    if(!items.length)return;
    shown+=items.length;
    const det=document.createElement('details');
    const sm=document.createElement('summary'); sm.textContent=d+' ('+items.length+')';
    det.appendChild(sm);
    for(const label of items){
      const row=document.createElement('div'); row.className='row';
      const cb=document.createElement('input'); cb.type='checkbox'; cb.dataset.l=label;
      const sp=document.createElement('span'); sp.textContent=label;
      const sl=document.createElement('select');
      sl.innerHTML='<option value="">—</option><option value="p">ראשי</option><option value="s">משני</option>';
      // checkbox+span wrapped in a label for tap-to-toggle; select is a sibling
      // outside the inner label so clicking the select does NOT toggle the checkbox
      // (mobile‑safe – no label‑forwarding).
      const lbl=document.createElement('label'); lbl.appendChild(cb); lbl.appendChild(sp);
      row.appendChild(lbl); row.appendChild(sl);
      det.appendChild(row);
    }
    box.appendChild(det);
  });
  if(!shown)box.innerHTML='<div class="hint">לא נמצאו אזורים</div>';
}
function markSelection(){
  $$('#list input[type=checkbox]').forEach(cb=>{
    const l=cb.dataset.l;
    const sel=cb.closest('.row')?.querySelector('select');
    if(SELMAP.has(l)){
      cb.checked=true;
      if(sel)sel.value=SELMAP.get(l);
    }else{
      cb.checked=false;
      if(sel)sel.value='';
    }
  });
}
function selected(){
  const p=[],s=[];
  SELMAP.forEach((v,l)=>{if(v==='p')p.push(l);else if(v==='s')s.push(l);});
  return {p,s};
}
// Sync DOM checkbox/select changes back into SELMAP (the single source of truth).
function syncSel(){
  $$('#list input[type=checkbox]').forEach(cb=>{
    const l=cb.dataset.l;
    const sel=cb.closest('.row')?.querySelector('select');
    const v = sel ? sel.value : (cb.checked?'p':'');
    if(cb.checked && v) SELMAP.set(l,v);
    else SELMAP.delete(l);
  });
}
function selectAllVisible(on){
  $$('#list input[type=checkbox]').forEach(cb=>{
    cb.checked=on;
    if(on){
      const v=SELMAP.get(cb.dataset.l)||'p';
      const sel=cb.closest('.row')?.querySelector('select');
      if(sel)sel.value=v;
    }
  });
}
// syncSel removed – SELMAP is the single source of truth, independent of DOM.

// ── wifi scan ──
async function scanWifi(selEl,msgEl){
  msg(msgEl,'סורק רשתות…');
  try{
    const n=await jget('/scan.json');
    selEl.innerHTML='';
    n.forEach(x=>{
      const o=document.createElement('option');
      o.value=x.ssid;o.textContent=x.ssid+' ('+x.rssi+' dBm)';
      selEl.appendChild(o);
    });
    msg(msgEl,n.length?('נמצאו '+n.length+' רשתות – בחר מהרשימה'):'לא נמצאו רשתות');
  }catch(e){msg(msgEl,'הסריקה נכשלה – נסה שוב','err');}
}

// ── connect test: the device joins in the background, AP stays up ──
let _connTimer=null;
function connectWifi(ssid,pass,onOk,onFail){
  clearInterval(_connTimer);
  fetch('/connect',{method:'POST',body:jurl({ssid,pass})}).then(async r=>{
    if(!r.ok){onFail(await r.text());return;}
    const t0=Date.now();
    _connTimer=setInterval(async ()=>{
      let st;try{st=await jget('/status');}catch(e){return;}
      if(st.wifi_status===3){clearInterval(_connTimer);onOk(st);return;}
      if(st.wifi_status===4){clearInterval(_connTimer);onFail('החיבור נכשל – בדוק את הסיסמה');return;}
      if(st.wifi_status===1){clearInterval(_connTimer);onFail('הרשת לא נמצאה');return;}
      if(Date.now()-t0>20000){clearInterval(_connTimer);onFail('לא התחבר תוך 20 שניות');}
    },800);
  }).catch(()=>onFail('אין תגובה מהמכשיר'));
}
)JS";

void handleAppJs() {
  web.send_P(200, "application/javascript; charset=utf-8", APP_JS);
}

// ──────────────────────────────────────────────
// Page 1 – initial setup (only while the configuration is incomplete)
// ──────────────────────────────────────────────

const char SETUP_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="he"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>הגדרת מנורת התרעה</title>
<link rel="stylesheet" href="/style.css">
</head><body>
<header><b>הגדרת מנורת התרעה</b><span id="ver" class="hint"></span></header>
<main>

<section class="step open">
 <h2>1 · רשת WiFi</h2>
 <div class="hint">המכשיר תומך רק ברשת 2.4GHz.</div>
 <label>רשתות שנמצאו</label>
 <button class="ghost" id="scanBtn" onclick="doScan()">סרוק רשתות</button>
 <select id="nets" onchange="$('#ssid').value=this.value"><option value="">—</option></select>
 <label>שם רשת (SSID)</label><input id="ssid" autocomplete="off">
 <label>סיסמה</label><input id="pass" type="password" autocomplete="off">
 <div class="actions"><button id="connBtn" onclick="doConnect()">חיבור ובדיקה</button></div>
 <div class="msg" id="wmsg"></div>
 <div class="hint">לאחר החיבור ייתכן שהטלפון יתנתק לרגע מנקודת הגישה ויחזור אליה.</div>
</section>

<section class="step" id="step2">
 <h2>2 · אזורי התרעה</h2>
 <div class="hint">לאחר החיבור לרשת – בחר את אזורי ההתרעה.</div>
 <label>חיפוש אזור</label><input id="q" placeholder="למשל: מודיעין" oninput="syncSel();renderAreas($('#list'),this.value);markSelection()">
 <div class="actions">
  <button class="ghost" onclick="selectAllVisible(true)">סמן הכול</button>
  <button class="ghost" onclick="selectAllVisible(false)">נקה</button>
 </div>
 <div id="list"></div>
 <div class="cnt" id="cnt"></div>
</section>

<div class="actions"><button id="go" disabled onclick="doSave()">שמירה והפעלה</button></div>
<div class="msg" id="smsg"></div>

</main>
<script src="/app.js"></script>
<script>
async function init(){
  const st=await jget('/status');
  $('#ver').textContent='v'+st.version;
  $('#ssid').value=st.ssid||'';
  await loadAreas($('#list'));
  updCount();
  $('#list').addEventListener('change',()=>{syncSel();updCount();});
  // already joined during an earlier attempt → open step 2 right away
  if(st.wifi_status===3){
    $('#step2').classList.add('open');$('#go').disabled=false;
    msg($('#wmsg'),'מחובר: '+st.ip+' · '+st.rssi+' dBm');
  }
}
function updCount(){const s=selected();$('#cnt').textContent='ראשי: '+s.p.length+' · משני: '+s.s.length;}
async function doScan(){$('#scanBtn').disabled=true;await scanWifi($('#nets'),$('#wmsg'));$('#scanBtn').disabled=false;}
function doConnect(){
  const ssid=$('#ssid').value.trim();
  if(!ssid){msg($('#wmsg'),'הזן שם רשת','err');return;}
  $('#connBtn').disabled=true;
  msg($('#wmsg'),'מתחבר ל'+ssid+'…');
  connectWifi(ssid,$('#pass').value, st=>{
    $('#connBtn').disabled=false;
    msg($('#wmsg'),'מחובר! כתובת '+st.ip+' · אות '+st.rssi+' dBm');
    $('#step2').classList.add('open');
    $('#go').disabled=false;
  }, err=>{ $('#connBtn').disabled=false; msg($('#wmsg'),err,'err'); });
}
async function doSave(){
  const sel=selected();
  if(!sel.p.length&&!sel.s.length){
    if(!confirm('לא נבחרו אזורים – המכשיר לא יידלק בהתרעות. לשמור בכל זאת?'))return;
  }
  $('#go').disabled=true;
  try{
    const r=await fetch('/save',{method:'POST',body:jurl({ssid:$('#ssid').value.trim(),pass:$('#pass').value,areas:JSON.stringify(sel)})});
    if(!r.ok){msg($('#smsg'),await r.text(),'err');$('#go').disabled=false;return;}
    msg($('#smsg'),'נשמר! המכשיר מפעיל מחדש ומתחבר לרשת…');
  }catch(e){msg($('#smsg'),'שגיאה בשמירה','err');$('#go').disabled=false;}
}
init();
</script>
</body></html>)HTML";

// ──────────────────────────────────────────────
// Page 2 – settings (sidebar: areas · networking · LED)
// ──────────────────────────────────────────────

const char SETTINGS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="he"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>הגדרות מנורת התרעה</title>
<link rel="stylesheet" href="/style.css">
</head><body>
<header><b>הגדרות</b><span id="ver" class="hint"></span></header>
<main class="layout">

<aside class="side">
 <button data-sec="areas" class="on">אזורי התרעה</button>
 <button data-sec="net">רשת</button>
 <button data-sec="led">תאורה</button>
 <hr>
 <button class="danger" onclick="doReset()">איפוס מוחלט</button>
</aside>

<div class="content">

 <section id="sec-areas">
  <h2>אזורי התרעה</h2>
  <label>חיפוש אזור</label><input id="q" placeholder="למשל: מודיעין" oninput="syncSel();renderAreas($('#list'),this.value);markSelection()">
  <div class="actions">
   <button class="ghost" onclick="selectAllVisible(true)">סמן הכול</button>
   <button class="ghost" onclick="selectAllVisible(false)">נקה</button>
   <button onclick="doSaveAreas()">שמירה</button>
  </div>
  <div id="list"></div>
  <div class="cnt" id="cnt"></div>
  <div class="msg" id="amsg"></div>
 </section>

 <section id="sec-net" hidden>
  <h2>רשת</h2>
  <label>רשת נוכחית</label><div class="hint" id="cur">…</div>
  <label>רשתות שנמצאו</label>
  <button class="ghost" id="scanBtn" onclick="doScan()">סרוק רשתות</button>
  <select id="nets" onchange="$('#ssid').value=this.value"><option value="">—</option></select>
  <label>שם רשת (SSID)</label><input id="ssid" autocomplete="off">
  <label>סיסמה (ריק = לשמור את הסיסמה הנוכחית)</label><input id="pass" type="password" autocomplete="off">
  <div class="actions">
   <button id="connBtn" onclick="doConnect()">חיבור ובדיקה</button>
   <button onclick="doSaveNet()">שמירה והפעלה מחדש</button>
  </div>
  <div class="msg" id="wmsg"></div>
  <div class="hint">לאחר החיבור ייתכן שהטלפון יתנתק לרגע מנקודת הגישה ויחזור אליה.</div>
 </section>

 <section id="sec-led" hidden>
  <h2>תאורה</h2>
  <label>בהירות (<span id="briv">…</span>%)</label>
  <input type="range" min="1" max="100" id="bri" oninput="$('#briv').textContent=this.value">
  <div class="actions">
   <button onclick="doSaveLed()">שמירה</button>
   <button class="ghost" onclick="fetch('/test?m=portal&bri='+$('#bri').value)">תצוגה</button>
  </div>
  <div class="msg" id="lmsg"></div>
 </section>

</div>
</main>
<script src="/app.js"></script>
<script>
function show(sec){
  $$('.side button[data-sec]').forEach(b=>b.classList.toggle('on',b.dataset.sec===sec));
  ['areas','net','led'].forEach(s=>$('#sec-'+s).hidden=(s!==sec));
}
$$('.side button[data-sec]').forEach(b=>b.addEventListener('click',()=>{
  location.hash='#'+b.dataset.sec;show(b.dataset.sec);
}));
window.addEventListener('hashchange',()=>show((location.hash||'#areas').slice(1)));

async function init(){
  const st=await jget('/status');
  $('#ver').textContent='v'+st.version;
  // networking
  $('#ssid').value=st.ssid||'';
  $('#cur').textContent=st.ssid?(st.ssid+' · '+st.ip+' · '+st.rssi+' dBm'):'לא מוגדר';
  // led
  $('#bri').value=st.led_bri;$('#briv').textContent=st.led_bri;
  // areas
  await loadAreas($('#list'));
  $('#list').addEventListener('change',()=>{syncSel();updCount();});
  updCount();
  show((location.hash||'#areas').slice(1));
}
function updCount(){const s=selected();$('#cnt').textContent='ראשי: '+s.p.length+' · משני: '+s.s.length;}

async function doSaveAreas(){
  const sel=selected();
  try{
    const r=await fetch('/saveareas',{method:'POST',body:jurl({areas:JSON.stringify(sel)})});
    msg($('#amsg'),r.ok?'נשמר – פועל לאלתר':'השמירה נכשела',!r.ok);
    updCount();
  }catch(e){msg($('#amsg'),'שגיאה בשמירה','err');}
}
async function doScan(){$('#scanBtn').disabled=true;await scanWifi($('#nets'),$('#wmsg'));$('#scanBtn').disabled=false;}
function doConnect(){
  const ssid=$('#ssid').value.trim();
  if(!ssid){msg($('#wmsg'),'הזן שם רשת','err');return;}
  $('#connBtn').disabled=true;
  msg($('#wmsg'),'מתחבר ל'+ssid+'…');
  connectWifi(ssid,$('#pass').value, st=>{
    $('#connBtn').disabled=false;
    msg($('#wmsg'),'מחובר! כתובת '+st.ip+' · אות '+st.rssi+' dBm');
  }, err=>{ $('#connBtn').disabled=false; msg($('#wmsg'),err,'err'); });
}
async function doSaveNet(){
  const st=await jget('/status');
  let clearpass='';
  if(!$('#pass').value&&st.hasPass){
    if(!confirm('לשמור רשת פתוחה (ללא סיסמה)?'))return;
    clearpass='1';
  }
  if(!confirm('לשמור את ההגדרות ולהפעיל מחדש?'))return;
  try{
    const r=await fetch('/savenet',{method:'POST',body:jurl({ssid:$('#ssid').value.trim(),pass:$('#pass').value,clearpass})});
    if(!r.ok){msg($('#wmsg'),await r.text(),'err');return;}
    msg($('#wmsg'),'נשמר – המכשיר מפעיל מחדש…');
  }catch(e){msg($('#wmsg'),'שגיאה בשמירה','err');}
}
async function doSaveLed(){
  try{
    const r=await fetch('/saveled',{method:'POST',body:jurl({bri:$('#bri').value})});
    msg($('#lmsg'),r.ok?'נשמר':'השמירה נכשלה',!r.ok);
  }catch(e){msg($('#lmsg'),'שגיאה בשמירה','err');}
}
async function doReset(){
  if(!confirm('לאפס את המכשיר? כל ההגדרות יימחקו ותצטרך להגדיר הכול מחדש.'))return;
  try{await fetch('/reset',{method:'POST'});msg($('#amsg'),'נמחק – המכשיר מפעיל מחדש…');}catch(e){}
}
init();
</script>
</body></html>)HTML";

// ──────────────────────────────────────────────
// Page 3 – internal development / troubleshooting page.
// Never linked from any other page; reachable only by typing /dev.
// ──────────────────────────────────────────────

const char DEV_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="he"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>פנימי – פיתוח</title>
<link rel="stylesheet" href="/style.css">
</head><body>
<header><b>⚙ עמוד פנימי – פיתוח ותחזוקה</b><span id="ver" class="hint"></span></header>
<main>

<section>
 <h2>סטטוס חי</h2>
 <pre id="raw">…</pre>
 <div class="hint" id="otainfo"></div>
</section>

<section>
 <h2>נורית</h2>
 <div class="grid">
  <div><label>GPIO</label><input id="pin"></div>
  <div><label>פיקסלים</label><input id="count"></div>
  <div><label>בהירות</label><input id="bri"></div>
 </div>
 <div class="actions">
  <button onclick="t('primary')">דפוס ראשי</button>
  <button onclick="t('secondary')">דפוס משני</button>
  <button onclick="t('pixels')">פיקסלים</button>
  <button onclick="t('off')">כבה</button>
  <button onclick="saveDev()">שמירה</button>
 </div>
 <div class="msg" id="dmsg"></div>
</section>

<section>
 <h2>דימוי התרעה</h2>
 <div class="actions">
  <button onclick="sim('p')">הזרקת ראשית</button>
  <button onclick="sim('s')">הזרקת משנית</button>
  <button class="ghost" onclick="sim('c')">ניקוי</button>
 </div>
</section>

<section>
 <h2>עדכוני קושחה</h2>
 <label>מרווח בדיקה (שניות, 0 = כבוי)</label>
 <input id="upd_int">
 <div class="actions">
  <button onclick="saveDev()">שמירה</button>
  <button class="ghost" onclick="updcheck()">בדיקה עכשיו</button>
 </div>
 <div class="msg" id="umsg"></div>
</section>

<section>
 <h2>לוג חי</h2>
 <label>רמת תיעוד (נשמרת במכשיר)</label>
 <select id="loglvl" onchange="saveDev()">
  <option value="0">ERROR</option>
  <option value="1">WARN</option>
  <option value="2">INFO</option>
  <option value="3">DEBUG</option>
 </select>
 <div class="actions">
  <button class="ghost" id="pauseBtn" onclick="togglePause()">השהיה</button>
  <button class="ghost" onclick="clearLog()">ניקוי</button>
  <a class="ghost" href="/logs.txt" download="alert-lamp.log">הורדה</a>
 </div>
 <pre id="logview" style="max-height:340px"></pre>
 <div class="hint" id="logstat"></div>
 <div class="msg" id="gmsg"></div>
</section>

<section>
 <h2>פעולות</h2>
 <div class="actions">
  <button class="ghost" onclick="restart()">הפעלה מחדש</button>
  <button class="danger" onclick="doReset()">איפוס מוחלט</button>
 </div>
</section>

</main>
<script>
const $=s=>document.querySelector(s);
const jget=async u=>(await fetch(u,{cache:'no-store'})).json();
function msg(el,t,err){el.textContent=t||'';el.className='msg'+(err?' err':'');}
function jurl(d){return new URLSearchParams(d);}

async function refresh(){
  try{
    const st=await jget('/status');
    $('#ver').textContent='v'+st.version;
    $('#raw').textContent=JSON.stringify(st,null,1);
    if(!($('#pin').matches(':focus')||$('#count').matches(':focus')||$('#bri').matches(':focus')||$('#upd_int').matches(':focus')||$('#loglvl').matches(':focus'))){
      $('#pin').value=st.led_pin;$('#count').value=st.led_count;$('#bri').value=st.led_bri;
      $('#upd_int').value=st.update_interval;
      if(typeof st.log_level==='number')$('#loglvl').value=st.log_level;
    }
    const o=st.ota||{};
    if(o.updating)$('#otainfo').textContent='מתקין גרסה '+o.latest+'…';
    else if(o.last_error)$('#otainfo').textContent='עדכון: שגיאה – '+o.last_error;
    else if(o.latest)$('#otainfo').textContent='עדכון: נוכחי '+o.current+' · אחרון='+o.latest+(o.latest!==o.current?' (זמין)':' (מעודכן)');
  }catch(e){}
}
async function t(m){try{await fetch('/test?m='+m+'&pin='+$('#pin').value+'&count='+$('#count').value+'&bri='+$('#bri').value);}catch(e){}}
async function sim(m){try{await fetch('/sim',{method:'POST',body:jurl({m})});}catch(e){}}
async function saveDev(){
  try{
    const r=await fetch('/savedev',{method:'POST',body:jurl({pin:$('#pin').value,count:$('#count').value,bri:$('#bri').value,upd_int:$('#upd_int').value,loglevel:$('#loglvl').value})});
    const t=r.ok?'נשמר':'השמירה נכשלה';
    msg($('#dmsg'),t,!r.ok);msg($('#umsg'),t,!r.ok);msg($('#gmsg'),t,!r.ok);
  }catch(e){msg($('#dmsg'),'שגיאה בשמירה','err');}
}

// ── live log: 1 s short-poll of /logs.json?since=<lastSeq> ──
let lastSeq=0,logPaused=false;
function logLineText(l){return '['+l.t+'ms] '+l.lv+' '+(l.tag?l.tag+': ':'')+l.msg;}
async function pollLog(){
  if(logPaused)return;
  try{
    const d=await jget('/logs.json?since='+lastSeq);
    if(typeof d.seq==='number')lastSeq=d.seq;
    const v=$('#logview');
    const atBottom=v.scrollTop+v.clientHeight>=v.scrollHeight-4;
    if(d.dropped>0){
      const dr=document.createElement('div');
      dr.textContent='… '+d.dropped+' שורות נמחקו מהמאגר …';
      v.appendChild(dr);
    }
    (d.lines||[]).forEach(l=>{
      const div=document.createElement('div');
      div.textContent=logLineText(l);
      v.appendChild(div);
    });
    while(v.childNodes.length>300)v.removeChild(v.firstChild);
    if(atBottom)v.scrollTop=v.scrollHeight;
    $('#logstat').textContent='שורות: '+v.childNodes.length+' · מספר אחרון: '+lastSeq;
  }catch(e){}
}
function togglePause(){
  logPaused=!logPaused;
  $('#pauseBtn').textContent=logPaused?'המשך':'השהיה';
  if(!logPaused)pollLog();
}
async function clearLog(){
  try{await fetch('/logclear',{method:'POST'});}catch(e){}
  $('#logview').textContent='';
  lastSeq=0;
  $('#logstat').textContent='';
}
async function updcheck(){
  msg($('#umsg'),'בודק…');
  try{
    await fetch('/updcheck',{method:'POST'});
    for(let i=0;i<30;i++){
      await new Promise(r=>setTimeout(r,1000));
      const st=await jget('/status');
      const o=st.ota||{};
      if(o.checks>0&&o.checked_ms_ago<5000){
        if(o.updating){msg($('#umsg'),'מתקין גרסה '+o.latest+'…');return;}
        if(o.last_error){msg($('#umsg'),'שגיאה: '+o.last_error,'err');return;}
        msg($('#umsg'),o.latest&&(o.latest!==o.current)?('זמין גרסה '+o.latest):('מעודכן: '+o.current));
        refresh();return;
      }
    }
    msg($('#umsg'),'הבדיקה נמשכת זמן מה','err');
  }catch(e){msg($('#umsg'),'הבדיקה לא יצאה לפועל','err');}
}
async function restart(){
  if(!confirm('להפעיל מחדש?'))return;
  try{await fetch('/restart',{method:'POST'});}catch(e){}
}
async function doReset(){
  if(!confirm('לאפס את המכשיר? כל ההגדרות יימחקו.'))return;
  try{await fetch('/reset',{method:'POST'});}catch(e){}
}
refresh();setInterval(refresh,3000);
pollLog();setInterval(pollLog,1000);
</script>
</body></html>)HTML";

void handleRoot() {
  if (g_settings.isComplete()) {
    // Configured device – the portal is only open for maintenance:
    // land straight on the settings page.
    web.sendHeader("Location", "/settings", true);
    web.send(302, "text/plain", "");
    return;
  }
  web.send_P(200, "text/html; charset=utf-8", SETUP_HTML);
}

void handleSettings() {
  web.send_P(200, "text/html; charset=utf-8", SETTINGS_HTML);
}

void handleDev() {
  (*g_srv).send_P(200, "text/html; charset=utf-8", DEV_HTML);
}

// ── runtime status UI (served while the lamp runs on WiFi) ──
const char RUNTIME_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="he"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>מנורת התרעה</title>
<style>
*{box-sizing:border-box}
body{font-family:system-ui,"Segoe UI",Arial,sans-serif;margin:0;background:#f5f6f8;color:#1b1f24;direction:rtl}
main{max-width:560px;margin:0 auto;padding:16px}
h1{font-size:19px;margin:0 0 12px}
.card{background:#fff;border:1px solid #e3e6ea;border-radius:10px;padding:12px;margin-bottom:10px}
.k{color:#66707b;font-size:13px}
.v{font-size:17px;font-weight:700;margin-top:2px}
.alert{color:#c1121f}
.calm{color:#0a7a32}
button{padding:10px 14px;border:0;border-radius:6px;background:#2f6fed;color:#fff;font-size:15px;cursor:pointer}
.danger{background:#e63946}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
pre{font-size:12px;background:#fff;border:1px solid #e6e9ee;border-radius:8px;padding:8px;overflow:auto;direction:ltr;text-align:left}
</style></head><body><main>
<h1>מנורת התרעה <span id="ver" style="color:#66707b"></span></h1>

<div class="card"><div class="k">מצב התרעה</div><div class="v" id="al">…</div></div>
<div class="card"><div class="k">WiFi</div><div class="v" id="wifi">…</div></div>

<div class="card"><div class="k">בדיקת נורית</div>
 <div class="actions">
  <button onclick="t('primary')">דפוס ראשי</button>
  <button onclick="t('secondary')">דפוס משני</button>
  <button onclick="t('pixels')">פיקסלים</button>
  <button onclick="t('off')">כבה</button>
 </div>
</div>

<div class="card"><div class="k">פעולות</div>
 <div class="actions"><button class="danger" onclick="openPortal()">פתיחת עמוד הגדרה</button></div>
</div>

<pre id="raw">…</pre>
</main>
<script>
const $=s=>document.querySelector(s);
const jget=async u=>(await fetch(u,{cache:'no-store'})).json();
async function refresh(){
  try{
    const s=await jget('/status');
    $('#ver').textContent='v'+s.version;
    const a=s.alerts||{};
    let txt='רגוע';
    if(a.primary) txt='התרעה – אזור ראשי';
    else if(a.secondary) txt='התרעה – אזור משני';
    else if(a.prewarning) txt='התרעה מקדימה';
    if(a.sim) txt+=' (בדיקה)';
    $('#al').textContent=txt;
    $('#al').className='v '+(a.primary||a.secondary||a.prewarning?'alert':'calm');
    $('#wifi').textContent=s.ssid+' · '+s.ip+' · '+s.rssi+' dBm';
    $('#raw').textContent=JSON.stringify(s,null,1);
  }catch(e){ $('#al').textContent='אין תשובה מהמכשיר'; }
}
async function t(m){ try{ await fetch('/test?m='+m); }catch(e){} refresh(); }
async function openPortal(){
  if(!confirm('לפתוח את עמוד ההגדרה?')) return;
  try{ await fetch('/portal',{method:'POST'}); }catch(e){}
  setTimeout(()=>location.reload(),4000);
}
refresh(); setInterval(refresh,5000);
</script>
</body></html>)HTML";

void handleRuntimeRoot() {
  runtimeWeb.send_P(200, "text/html; charset=utf-8", RUNTIME_HTML);
}

void handleRuntimeStatus() {
  runtimeWeb.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void handleRuntimeTest() { handleTestFor(runtimeWeb); }

void handleRuntimePortal() {
  wantPortal = true;  // actually switch in runtimeTick(), not inside a handler
  runtimeWeb.send(200, "text/plain; charset=utf-8", "switching to portal");
}

}  // namespace

// ──────────────────────────────────────────────
// portal
// ──────────────────────────────────────────────
namespace portal {

void begin() {
  if (runtimeOn) { runtimeWeb.stop(); runtimeOn = false; }
  g_srv = &web;  // handlers dispatch on the portal server while the AP is up
  LOGI(TAG, "starting configuration access point…");
  WiFi.mode(WIFI_AP_STA);  // STA stays available for /scan and /connect
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);

  dns.start(53, "*", apIP);

  buildAreasJson();

  web.on("/", handleRoot);
  web.on("/settings", handleSettings);
  web.on("/dev", handleDev);
  web.on("/style.css", handleStyle);
  web.on("/app.js", handleAppJs);
  web.on("/areas.json", handleAreas);
  web.on("/scan.json", handleScan);
  web.on("/status", handleStatus);
  web.on("/logs.json", handleLogsJson);
  web.on("/logs.txt", handleLogsTxt);
  web.on("/connect", HTTP_POST, handleConnect);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/savenet", HTTP_POST, handleSaveNet);
  web.on("/saveareas", HTTP_POST, handleSaveAreas);
  web.on("/saveled", HTTP_POST, handleSaveLed);
  web.on("/savedev", HTTP_POST, handleSaveDev);
  web.on("/logclear", HTTP_POST, handleLogClear);
  web.on("/sim", HTTP_POST, handleSim);
  web.on("/restart", HTTP_POST, handleRestart);
  web.on("/test", handleTest);
  web.on("/reset", HTTP_POST, handleReset);
  web.on("/updcheck", HTTP_POST, handleUpdCheck);
  web.onNotFound(handleNotFound);
  web.begin();

  running = true;
  LOGI(TAG, "AP '" + String(AP_SSID) + "' up, http://192.168.4.1/");
}

void tick() {
  if (!running) return;
  g_srv = &web;  // handlers dispatch on the portal server while the AP is up
  dns.processNextRequest();
  web.handleClient();
  static uint32_t idleSince = 0;
  dropIdleClient(web, idleSince);
}

bool active() { return running; }

void beginRuntime() {
  if (runtimeOn) return;
  if (running) {
    dns.stop();
    web.stop();
    running = false;
    LOGI(TAG, "configuration portal closed");
  }

  g_srv = &runtimeWeb;  // handlers now dispatch on the running server

  static bool routesDone = false;
  if (!routesDone) {
    runtimeWeb.on("/", handleRuntimeRoot);
    runtimeWeb.on("/status", handleRuntimeStatus);
    runtimeWeb.on("/test", handleRuntimeTest);
    runtimeWeb.on("/portal", HTTP_POST, handleRuntimePortal);
    // Additional routes that also answer while the lamp is running:
    runtimeWeb.on("/dev", handleDev);
    runtimeWeb.on("/savedev", handleSaveDev);
    runtimeWeb.on("/logs.json", handleLogsJson);
    runtimeWeb.on("/logs.txt", handleLogsTxt);
    // Assets shared with the configuration portal while running:
    runtimeWeb.on("/settings", handleSettings);
    runtimeWeb.on("/style.css", handleStyle);
    routesDone = true;
  }

  runtimeWeb.begin();
  runtimeOn = true;
  LOGI(TAG_WEB, "status page: http://alertlamp.local/ (or http://" +
                    WiFi.localIP().toString() + "/)");
}

void runtimeTick() {
  if (!runtimeOn) return;
  g_srv = &runtimeWeb;  // handlers dispatch on the runtime server while running
  runtimeWeb.handleClient();
  static uint32_t idleSince = 0;
  dropIdleClient(runtimeWeb, idleSince);
  if (wantPortal) {
    wantPortal = false;
    begin();  // switch over to the configuration portal
  }
}

}  // namespace portal
