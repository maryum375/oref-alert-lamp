#include "log.h"

#include "settings.h"

namespace logger {
namespace {

constexpr size_t MAX_ENTRIES = 150;
constexpr size_t MAX_MSG = 160;  // bytes of message kept per line
constexpr size_t MAX_TAG = 12;   // bytes of tag kept per line

struct Entry {
  uint32_t seq;  // monotonically increasing, starts at 1
  uint32_t t;    // millis() when the line was captured
  uint8_t lv;    // Level
  char tag[MAX_TAG + 1];
  char msg[MAX_MSG + 1];
};

Entry ring[MAX_ENTRIES];        // ≈ 150 × 184 B ≈ 27 KB of static RAM
size_t head = 0;                // slot to write next (== oldest slot once full)
size_t count = 0;               // entries currently stored (0..MAX_ENTRIES)
uint32_t nextSeq = 1;           // sequence number the next captured line gets
int capLevel = INFO;            // capture filter: lines "below" it are serial-only

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Copy at most dstSize-1 bytes, never cutting a UTF-8 sequence in half
// (Hebrew messages are UTF-8; a partial trailing byte would show as ?).
void copyBounded(char* dst, size_t dstSize, const char* src) {
  size_t i = 0;
  while (src[i] && i < dstSize - 1) i++;
  if (src[i]) {  // truncated: step back over any partial UTF-8 sequence
    while (i > 0 && ((uint8_t)src[i] & 0xC0) == 0x80) i--;
  }
  memcpy(dst, src, i);
  dst[i] = '\0';
}

// JSON string escaping for messages/tags: " \ and control chars are
// escaped; every other byte (incl. UTF-8 Hebrew) passes through raw.
void escapeInto(const char* s, String& out) {
  for (size_t i = 0; s[i]; i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char esc[8];
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      out += esc;
    } else {
      out += (char)c;
    }
  }
}

// Snapshot of the read cursor under the lock; the entries themselves are
// then read lock-free (appends only ever touch the slot at `head`).
void snapshot(size_t& oldest, size_t& n, uint32_t& last) {
  portENTER_CRITICAL(&mux);
  n = count;
  oldest = (head + MAX_ENTRIES - count) % MAX_ENTRIES;
  last = nextSeq - 1;
  portEXIT_CRITICAL(&mux);
}

}  // namespace

void begin() {
  // The capture level is persisted by cfg::load()/cfg::save().
  setLevel((Level)g_settings.logLevel);
}

void log(Level lv, const char* tag, const String& msg) {
  if (lv > capLevel) return;  // below the filter: serial already has it

  Entry e;
  e.t = millis();
  e.lv = (uint8_t)lv;
  copyBounded(e.tag, sizeof(e.tag), tag ? tag : "");
  copyBounded(e.msg, sizeof(e.msg), msg.c_str());

  portENTER_CRITICAL(&mux);
  e.seq = nextSeq++;
  ring[head] = e;
  head = (head + 1) % MAX_ENTRIES;
  if (count < MAX_ENTRIES) count++;
  portEXIT_CRITICAL(&mux);
}

void setLevel(Level lv) {
  if (lv < ERR) lv = ERR;
  if (lv > DEBUG) lv = DEBUG;
  capLevel = lv;
}

Level level() { return (Level)capLevel; }

const char* levelName(Level lv) {
  switch (lv) {
    case ERR: return "ERROR";
    case WARN: return "WARN";
    case INFO: return "INFO";
    default: return "DEBUG";
  }
}

char levelChar(Level lv) {
  return lv == ERR ? 'E' : lv == WARN ? 'W' : lv == INFO ? 'I' : 'D';
}

uint32_t lastSeq() {
  portENTER_CRITICAL(&mux);
  uint32_t s = nextSeq - 1;
  portEXIT_CRITICAL(&mux);
  return s;
}

void since(uint32_t seq, String& jsonOut, uint32_t& dropped) {
  size_t oldest, n;
  uint32_t last;
  snapshot(oldest, n, last);

  // Lines with seq in (seq, oldestSeq) existed once but were evicted by
  // the ring wrapping. seq == 0 asks for the whole buffer → nothing dropped.
  dropped = 0;
  uint32_t oldestSeq = last + 1 - n;
  if (n && seq && seq + 1 < oldestSeq) dropped = oldestSeq - seq - 1;

  jsonOut = "";
  jsonOut.reserve(n * 128 + 64);
  jsonOut += F("{\"seq\":");
  jsonOut += last;
  jsonOut += F(",\"dropped\":");
  jsonOut += dropped;
  jsonOut += F(",\"lines\":[");

  bool first = true;
  for (size_t i = 0; i < n; i++) {
    const Entry& e = ring[(oldest + i) % MAX_ENTRIES];
    if (e.seq <= seq) continue;
    if (first) {
      first = false;
    } else {
      jsonOut += ',';
    }
    jsonOut += F("{\"seq\":");
    jsonOut += e.seq;
    jsonOut += F(",\"t\":");
    jsonOut += e.t;
    jsonOut += F(",\"lv\":\"");
    jsonOut += levelChar((Level)e.lv);
    jsonOut += F("\",\"tag\":\"");
    escapeInto(e.tag, jsonOut);
    jsonOut += F("\",\"msg\":\"");
    escapeInto(e.msg, jsonOut);
    jsonOut += F("\"}");
  }
  jsonOut += F("]}");
}

void toText(String& out) {
  size_t oldest, n;
  uint32_t last;
  snapshot(oldest, n, last);

  out = "";
  out.reserve(n * 200);
  for (size_t i = 0; i < n; i++) {
    const Entry& e = ring[(oldest + i) % MAX_ENTRIES];
    out += '[';
    out += e.t;
    out += F("ms] ");
    out += levelChar((Level)e.lv);
    out += ' ';
    if (e.tag[0]) {
      out += e.tag;
      out += F(": ");
    }
    out += e.msg;
    out += '\n';
  }
}

void clear() {
  portENTER_CRITICAL(&mux);
  head = 0;
  count = 0;
  // nextSeq is deliberately NOT reset: sequence numbers keep increasing,
  // so live viewers' `since` cursors stay valid after a clear.
  portEXIT_CRITICAL(&mux);
}

}  // namespace logger
