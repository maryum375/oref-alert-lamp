#pragma once

#include <Arduino.h>

// ──────────────────────────────────────────────
// In-memory log ring buffer backing the /dev page "live log" section.
//
// Every message is printed to the USB serial console first (ALL levels,
// byte-identical to the previous direct Serial.println calls – the serial
// console stays a full debugging console) and then captured into a
// fixed-size RAM ring buffer, unless its level is below the capture
// filter (default INFO). The filter is persisted in NVS as
// Settings::logLevel and can be changed live from /dev.
//
// ~150 entries × ~184 bytes ≈ 27 KB of static RAM; each message is kept
// as one 160-byte, UTF-8-safe line.
//
// Appends happen from loop() context only; the sequence-number/index
// update is nevertheless guarded by a portMUX critical section so the
// reader side (portal handlers, same task) always sees consistent
// head/count/nextSeq snapshots.
//
// NOTE: never name a function `log()` – math.h already declares
// log(double); everything lives inside namespace logger.
// ──────────────────────────────────────────────

namespace logger {

enum Level : int { ERR = 0, WARN = 1, INFO = 2, DEBUG = 3 };

void begin();  // seed the capture level from g_settings.logLevel (call after cfg::load())

void log(Level lv, const char* tag, const String& msg);

void setLevel(Level lv);
Level level();
const char* levelName(Level lv);  // "ERROR" / "WARN" / "INFO" / "DEBUG"
char levelChar(Level lv);         // 'E' / 'W' / 'I' / 'D'

uint32_t lastSeq();  // sequence number of the newest captured line (0 = nothing captured)

// JSON for GET /logs.json?since=N – only lines with seq > N, oldest first:
//   {"seq":<lastSeq>,"dropped":<lines evicted since N>,"lines":[
//     {"seq":N,"t":<uptime ms>,"lv":"I","tag":"wifi","msg":"…"}, …]}
// seq = 0 (or a missing query arg) returns the whole buffer.
void since(uint32_t seq, String& jsonOut, uint32_t& dropped);

// Plain-text dump for GET /logs.txt, oldest first, one line per entry:
//   [123456ms] I wifi: connecting to 'MyNetwork' …
void toText(String& out);

void clear();  // drop every captured line (sequence numbers keep rising)

}  // namespace logger

// ── logging macros ────────────────────────────
// Print "[tag] message" to serial (ALL levels, byte-identical to the old
// Serial.println("[tag] …") calls) AND append to the ring buffer
// (level-filtered). The message expression is evaluated exactly once.
//
// Some ESP-IDF headers have historically defined printf-style LOGE/LOGI
// macros – make sure our definitions win regardless of include order.
#ifdef LOGE
#undef LOGE
#endif
#ifdef LOGW
#undef LOGW
#endif
#ifdef LOGI
#undef LOGI
#endif
#ifdef LOGD
#undef LOGD
#endif

#define LOG_AT(lv, tag_, msg_)               \
  do {                                       \
    const char* _logTag = (tag_);            \
    String _logMsg = (msg_);                 \
    Serial.print('[');                       \
    Serial.print(_logTag);                   \
    Serial.print("] ");                      \
    Serial.println(_logMsg);                 \
    logger::log((lv), _logTag, _logMsg);     \
  } while (0)

#define LOGE(tag_, msg_) LOG_AT(logger::ERR, tag_, msg_)
#define LOGW(tag_, msg_) LOG_AT(logger::WARN, tag_, msg_)
#define LOGI(tag_, msg_) LOG_AT(logger::INFO, tag_, msg_)
#define LOGD(tag_, msg_) LOG_AT(logger::DEBUG, tag_, msg_)
