/*
  Jar of Thoughts - STAGE 5: any-color pulses, idle glow, optional double click
  Board:    ESP32 Dev Module
  Libraries (same as before, nothing new to install):
    - Adafruit NeoPixel, UniversalTelegramBot (Brian Lough),
      ArduinoJson (6.21.x, NOT 7.x), WiFiManager (tzapu)

  secrets.h (same folder) needs: BOT_TOKEN, CHAT_ID, SETUP_AP_NAME,
  SETUP_AP_PASSWORD, SETTINGS_PASSWORD (use "" for no password).

  FROM TELEGRAM (you)
    Pulse                  Glow in her "incoming pulse" color until she presses the jar
    /pulse red             Glow in any color for this pulse only. Accepts names
                           (red, pink, blue, mint, ...), hex (#ff8800) or r g b (255 120 0)
    /doubleclick enable    Turns on the second pulse + double click (see below)
    /doubleclick disable   Turns it off again
    /pulse2 [color]        Second pulse (only when double click is enabled).
                           The "Pulse 2" keyboard button does the same.
    /settings              Link to her settings page
    /status                Signal, time, quiet hours, double click state

  ON HER SETTINGS PAGE (http://jar.local on her WiFi)
    Incoming pulse color, and the message you receive when she presses the jar
    Idle glow: color, on/off, how bright (off automatically in quiet hours / night)
    Brightness, timezone, quiet hours
    Only after /doubleclick enable:  second pulse color + message sent on double click

  BUTTON
    Short press   Acknowledges a pulse. With no pulse, sends you her "press" message.
    Double press  If double click is ENABLED: acknowledges a pulse, or with no pulse
                  sends you her "double click" message.
                  If NOT enabled: toggles manual night mode (dark).
    Hold 10 s     Erase saved WiFi and restart into setup (white fill from 3 s)

  The "Jar online" message is now sent only after a power-up or crash, never after
  a WiFi blip or the nightly restart.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "secrets.h"

// Types are defined up here because the Arduino IDE auto-declares functions
// near the top of the file.
enum ButtonEvent : uint8_t { BTN_NONE, BTN_SHORT, BTN_DOUBLE, BTN_RESET };
enum EventType : uint8_t { EVT_PULSE = 1, EVT_PULSE2 = 2, EVT_DBL_ON = 3, EVT_DBL_OFF = 4 };
enum JarState : uint8_t { ST_IDLE, ST_PULSING, ST_ACK_FADE, ST_SOFT_GLOW, ST_PREVIEW };

struct OutMsg {
  char text[96];
};

struct InEvent {
  uint8_t type;
  bool hasColor;
  uint32_t rgb;     // 0xRRGGBB
};

// Snapshot refreshed every second and kept in RTC memory. It survives a crash or
// watchdog reset (not a full power loss), so after a reboot we can report what
// the jar looked like just before it died.
struct CrashInfo {
  uint32_t magic;
  uint32_t upSec;
  uint32_t freeHeap;
  uint32_t minHeap;
  uint32_t netStackFree;
  uint32_t loopStackFree;
  int32_t  rssi;
  uint32_t telegramAgeSec;
  uint32_t stage;           // 0 = still in setup(), 1 = main loop running
};

struct Settings {
  uint32_t pulseRGB;      // incoming pulse color
  uint32_t pulse2RGB;     // second incoming pulse color
  uint32_t idleRGB;       // idle glow color
  bool     idleOn;
  uint8_t  idlePct;       // idle glow level, 3-50 (percent of the brightness setting)
  uint8_t  brightnessPct; // 20-100, scaled under the hard cap below
  bool     quietOn;
  uint8_t  quietStart;
  uint8_t  quietEnd;
  uint8_t  tzIdx;
  bool     dblOn;         // double click / second pulse enabled by you
  uint8_t  timeoutIdx;    // index into TIMEOUT_MIN: when an unseen pulse gives up
  bool     pulseInDark;   // let pulses glow during quiet hours / night mode
  char     msg1[64];      // sent to you on a short press with no pulse
  char     msg2[64];      // sent to you on a double click with no pulse
};

struct NamedColor {
  const char* name;
  uint32_t rgb;
};

// ---- Settings you can safely change -------------------------------------
const int LED_PIN        = 18;
const int LED_COUNT      = 7;
const int BUTTON_PIN     = 4;
const int MAX_BRIGHTNESS = 100;          // hard cap, 0-255. Never exceeded.

const float BREATH_SECONDS           = 4.0;
// (pulse timeout is now a setting on her page, see TIMEOUT_MIN below)
const unsigned long POLL_MS          = 2500;
const unsigned long FAULT_AFTER_MS   = 120000;
const float FAULT_LEVEL              = 0.15;

const unsigned long HOLD_SHOW_MS  = 3000;
const unsigned long HOLD_RESET_MS = 10000;
const unsigned long DOUBLE_MS     = 400;

const unsigned long NET_STALL_MS  = 240000UL;
const int RESTART_HOUR            = 4;

const char* BUILD_TAG = "s5-r8";   // shown in the boot log and /status so you can tell which build is running
const bool NIGHTLY_RESTART = false; // heap is stable, so the 4 AM restart is off
const char* DEFAULT_MSG1 = "She pressed the jar";
const char* DEFAULT_MSG2 = "She double clicked the jar";
// -------------------------------------------------------------------------

// Color names you can type after /pulse
const NamedColor NAMED[] = {
  {"red", 0xFF0000}, {"orange", 0xFF5A00}, {"yellow", 0xFFC800}, {"green", 0x00FF00},
  {"mint", 0x00FF78}, {"teal", 0x00C896}, {"cyan", 0x00DCFF}, {"blue", 0x003CFF},
  {"purple", 0x8200FF}, {"lavender", 0x963CFF}, {"magenta", 0xFF00C8}, {"pink", 0xFF3C8C},
  {"white", 0xFFFFFF}, {"warm", 0xFF9632}, {"amber", 0xFF7800}
};
const int NAMED_COUNT = sizeof(NAMED) / sizeof(NAMED[0]);

// Idle glow presets (lamp-like and mood colors), shown as swatches under the idle color.
const NamedColor LAMP[] = {
  {"Candle", 0xFF8912}, {"Warm white", 0xFFA957}, {"Soft white", 0xFFB46B},
  {"Neutral", 0xFFD1A3}, {"Cool white", 0xFFE4CE}, {"Daylight", 0xFFF7F2},
  {"Honey", 0xFFB000}, {"Amber", 0xFF7800}, {"Sunset", 0xFF5A1E},
  {"Peach", 0xFF9680}, {"Rose", 0xFF4F78}, {"Lavender", 0xB07CFF},
  {"Moonlight", 0x8FB3FF}, {"Mint", 0x7DFFB8}
};
const int LAMP_COUNT = sizeof(LAMP) / sizeof(LAMP[0]);

// Pulse color presets, shown under the incoming (and second) pulse color.
const NamedColor PULSE_PRESETS[] = {
  {"Red", 0xFF0000}, {"Orange", 0xFF5A00}, {"Yellow", 0xFFC800}, {"Green", 0x00FF00},
  {"Mint", 0x00FF78}, {"Cyan", 0x00DCFF}, {"Blue", 0x003CFF}, {"Purple", 0x8200FF},
  {"Magenta", 0xFF00C8}, {"Pink", 0xFF285A}, {"Warm white", 0xFFA957}, {"White", 0xFFFFFF}
};
const int PULSE_PRESET_COUNT = sizeof(PULSE_PRESETS) / sizeof(PULSE_PRESETS[0]);

// How long an unseen pulse keeps going before it gives up (she picks one).
const int TIMEOUT_COUNT = 6;
const char* TIMEOUT_NAMES[TIMEOUT_COUNT] = {"Never (until you press the jar)", "1 hour", "3 hours", "6 hours", "12 hours", "24 hours"};
const uint16_t TIMEOUT_MIN[TIMEOUT_COUNT] = {0, 60, 180, 360, 720, 1440};

// Tap-to-pulse color panel in Telegram (inline buttons under a message).
const int PANEL_COUNT = 8;
const char* PANEL_EMOJI[PANEL_COUNT] = {"\xF0\x9F\x94\xB4", "\xF0\x9F\x9F\xA0", "\xF0\x9F\x9F\xA1", "\xF0\x9F\x9F\xA2",
                                        "\xF0\x9F\x94\xB5", "\xF0\x9F\x9F\xA3", "\xF0\x9F\x92\x97", "\xE2\x9A\xAA"};
const char* PANEL_NAME[PANEL_COUNT]  = {"red", "orange", "yellow", "green", "blue", "purple", "pink", "white"};

// Timezones (POSIX strings handle daylight saving automatically).
const int TZ_COUNT = 10;
const char* TZ_NAMES[TZ_COUNT] = {
  "US Eastern", "US Central", "US Mountain", "US Pacific", "US Arizona",
  "US Alaska", "US Hawaii", "UK / Ireland", "Central Europe", "UTC"
};
const char* TZ_POSIX[TZ_COUNT] = {
  "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0",
  "PST8PDT,M3.2.0,M11.1.0", "MST7", "AKST9AKDT,M3.2.0,M11.1.0", "HST10",
  "GMT0BST,M3.5.0/1,M10.5.0", "CET-1CEST,M3.5.0,M10.5.0/3", "UTC0"
};

Adafruit_NeoPixel ring(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
WiFiManager wm;
WebServer server(80);
Preferences prefs;
Settings cfg;

QueueHandle_t eventQueue;   // network task -> main loop (InEvent)
QueueHandle_t outQueue;     // main loop    -> network task (OutMsg)

volatile bool wifiUp = false;
volatile bool darkFlag = false;      // true while night mode OR quiet hours
volatile bool dblFlag = false;       // mirror of cfg.dblOn for the network task
volatile bool portalFlag = false;
volatile unsigned long lastTelegramOk = 0;
volatile unsigned long netBeat = 0;
volatile uint32_t netStackFree = 0;   // lowest free bytes ever on the network task stack

const uint32_t CRASH_MAGIC = 0xC0FFEE42;
RTC_NOINIT_ATTR CrashInfo lastInfo;
CrashInfo prevInfo;
bool havePrev = false;

bool nightMode = false;
bool quietNow = false;

void queueMessage(const char* text) {
  OutMsg m;
  strncpy(m.text, text, sizeof(m.text) - 1);
  m.text[sizeof(m.text) - 1] = 0;
  xQueueSend(outQueue, &m, 0);
}

// ---- Small helpers ----------------------------------------------------------
bool timeKnown() {
  return time(nullptr) > 1600000000L;
}

String timeNow() {
  if (!timeKnown()) return "not synced yet";
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return String(buf);
}

String hourLabel(int h) {
  int x = h % 12;
  if (x == 0) x = 12;
  return String(x) + (h < 12 ? " AM" : " PM");
}

bool computeQuiet() {
  if (!cfg.quietOn || cfg.quietStart == cfg.quietEnd || !timeKnown()) return false;
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  int h = tmv.tm_hour;
  if (cfg.quietStart < cfg.quietEnd) return h >= cfg.quietStart && h < cfg.quietEnd;
  return h >= cfg.quietStart || h < cfg.quietEnd;
}

// Reads a color typed as a name, #rrggbb / rrggbb, or "r g b".
bool parseColor(String s, uint32_t& out) {
  s.trim();
  s.toLowerCase();
  if (s.length() == 0) return false;

  for (int i = 0; i < NAMED_COUNT; i++) {
    if (s == NAMED[i].name) {
      out = NAMED[i].rgb;
      return true;
    }
  }

  String h = s;
  if (h.startsWith("#")) h.remove(0, 1);
  if (h.length() == 6) {
    bool ok = true;
    for (int i = 0; i < 6; i++) {
      if (!isxdigit((unsigned char)h[i])) ok = false;
    }
    if (ok) {
      out = (uint32_t)strtoul(h.c_str(), nullptr, 16);
      return true;
    }
  }

  int r, g, b;
  if (sscanf(s.c_str(), "%d%*[ ,]%d%*[ ,]%d", &r, &g, &b) == 3 &&
      r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
    out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return true;
  }
  return false;
}

String hexOf(uint32_t rgb) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%06x", (unsigned int)(rgb & 0xFFFFFF));
  return String(buf);
}

// Trims, limits to 60 bytes without cutting an emoji or accented letter in half.
String clipMsg(String s) {
  s.trim();
  int n = 60;
  if ((int)s.length() > n) {
    while (n > 0 && (((uint8_t)s[n]) & 0xC0) == 0x80) n--;
    s = s.substring(0, n);
  }
  return s;
}

String htmlEsc(const char* s) {
  String o;
  for (; *s; s++) {
    char c = *s;
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

void copyMsg(char* dest, size_t size, const String& src, const char* fallback) {
  String m = clipMsg(src);
  if (m.length() == 0) m = fallback;
  strncpy(dest, m.c_str(), size - 1);
  dest[size - 1] = 0;
}

// ---- Saved settings ---------------------------------------------------------
void loadSettings() {
  prefs.begin("jar", false);
  cfg.pulseRGB      = prefs.getUInt("pc", 0xFF7800);
  cfg.pulse2RGB     = prefs.getUInt("pc2", 0xFF285A);
  cfg.idleRGB       = prefs.getUInt("ic", 0xFF9632);
  cfg.idleOn        = prefs.getBool("ion", true);
  cfg.idlePct       = prefs.getUChar("ipct", 12);
  cfg.brightnessPct = prefs.getUChar("bright", 100);
  cfg.quietOn       = prefs.getBool("qon", false);
  cfg.quietStart    = prefs.getUChar("qs", 22);
  cfg.quietEnd      = prefs.getUChar("qe", 7);
  cfg.tzIdx         = prefs.getUChar("tz", 3);
  cfg.dblOn         = prefs.getBool("dbl", false);
  cfg.timeoutIdx    = prefs.getUChar("tmo", 0);
  cfg.pulseInDark   = prefs.getBool("pind", true);
  copyMsg(cfg.msg1, sizeof(cfg.msg1), prefs.getString("m1", DEFAULT_MSG1), DEFAULT_MSG1);
  copyMsg(cfg.msg2, sizeof(cfg.msg2), prefs.getString("m2", DEFAULT_MSG2), DEFAULT_MSG2);

  if (cfg.idlePct < 3) cfg.idlePct = 3;
  if (cfg.idlePct > 50) cfg.idlePct = 50;
  if (cfg.brightnessPct < 20) cfg.brightnessPct = 20;
  if (cfg.brightnessPct > 100) cfg.brightnessPct = 100;
  if (cfg.quietStart > 23) cfg.quietStart = 22;
  if (cfg.quietEnd > 23) cfg.quietEnd = 7;
  if (cfg.tzIdx >= TZ_COUNT) cfg.tzIdx = 3;
  if (cfg.timeoutIdx >= TIMEOUT_COUNT) cfg.timeoutIdx = 0;
}

void saveSettings() {
  prefs.putUInt("pc", cfg.pulseRGB);
  prefs.putUInt("pc2", cfg.pulse2RGB);
  prefs.putUInt("ic", cfg.idleRGB);
  prefs.putBool("ion", cfg.idleOn);
  prefs.putUChar("ipct", cfg.idlePct);
  prefs.putUChar("bright", cfg.brightnessPct);
  prefs.putBool("qon", cfg.quietOn);
  prefs.putUChar("qs", cfg.quietStart);
  prefs.putUChar("qe", cfg.quietEnd);
  prefs.putUChar("tz", cfg.tzIdx);
  prefs.putBool("dbl", cfg.dblOn);
  prefs.putUChar("tmo", cfg.timeoutIdx);
  prefs.putBool("pind", cfg.pulseInDark);
  prefs.putString("m1", cfg.msg1);
  prefs.putString("m2", cfg.msg2);
}

// The active pulse is remembered in flash so it survives a reboot or power cut.
void savePulse(uint8_t kind, uint32_t rgb) {
  prefs.putUChar("pk", kind);
  prefs.putUInt("prgb", rgb);
}

void clearPulse() {
  prefs.putUChar("pk", 0);
}

// ---- Log (kept in the jar's own flash, newest at the end) -------------------
// logLine() is only called from the main loop. The network task only reads.
void logLine(const String& msg) {
  File f = LittleFS.open("/log.txt", FILE_APPEND);
  if (!f) return;
  String line = "T+" + String(millis() / 60000) + "m " +
                (timeKnown() ? timeNow() : String("--:--")) + " " + msg + "\n";
  f.print(line);
  size_t sz = f.size();
  f.close();
  if (sz > 12000) {                        // keep the file small: rotate
    LittleFS.remove("/log.old");
    LittleFS.rename("/log.txt", "/log.old");
  }
}

String logTail(size_t maxChars) {
  File f = LittleFS.open("/log.txt", FILE_READ);
  if (!f) return "(no log yet)";
  size_t sz = f.size();
  if (sz > maxChars) {
    f.seek(sz - maxChars);
    f.readStringUntil('\n');               // drop the partial first line
  }
  String out = f.readString();
  f.close();
  if (out.length() == 0) out = "(log is empty)";
  return out;
}

void restartNow(const char* why) {
  Serial.println(why);
  logLine(String("restart: ") + why);
  WiFi.disconnect(true);   // shut WiFi down cleanly first
  delay(300);
  ESP.restart();
}

const char* resetName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default:               return "unknown";
  }
}

String prevInfoText() {
  if (!havePrev) return "";
  return String("\nBefore reset") + (prevInfo.stage == 0 ? " (DURING STARTUP)" : "") + ": up " + String(prevInfo.upSec / 60) + " min, heap free " +
         String(prevInfo.freeHeap) + " (lowest " + String(prevInfo.minHeap) +
         "), net stack free " + String(prevInfo.netStackFree) + " B, loop stack free " +
         String(prevInfo.loopStackFree) + " B, signal " + String(prevInfo.rssi) +
         " dBm, Telegram last OK " + String(prevInfo.telegramAgeSec) + " s before";
}

// Brightness scale applied by us (not by NeoPixel) so dim colors keep their hue: NeoPixel truncates each
// channel after scaling, which turns dim pink/purple into plain red. Hard cap still holds: max channel = MAX_BRIGHTNESS.
float gScale = 1.0f;
void applyBrightness() {
  gScale = (cfg.brightnessPct / 100.0f) * (MAX_BRIGHTNESS / 255.0f);
  ring.setBrightness(255);   // 255 = pass-through in Adafruit_NeoPixel; scaling happens in scaleCh()
}
uint8_t scaleCh(uint8_t v, float level) {
  float x = v * level * gScale + 0.5f;
  if (x > MAX_BRIGHTNESS) x = MAX_BRIGHTNESS;
  return (uint8_t)x;
}

void applyTimezone() {
  configTzTime(TZ_POSIX[cfg.tzIdx], "pool.ntp.org", "time.nist.gov");
}

// ---- Network task (runs on core 0) ----------------------------------------
void sendKeyboard(UniversalTelegramBot& bot, const String& text) {
  const char* kb = dblFlag ? "[[\"Pulse\",\"Pulse 2\"]]" : "[[\"Pulse\"]]";
  bot.sendMessageWithReplyKeyboard(CHAT_ID, text, "", kb, true);
}

String settingsLink() {
  return "http://jar.local\n(or http://" + WiFi.localIP().toString() + ")";
}

// Text for the "Jar online" message, or "" when the restart was routine.
String announceText() {
  const char* why = nullptr;
  switch (esp_reset_reason()) {
    case ESP_RST_SW:       return "";   // nightly or self-heal restart: stay quiet
    case ESP_RST_POWERON:  return "Jar powered on";
    case ESP_RST_PANIC:    why = "software crash (panic)"; break;
    case ESP_RST_INT_WDT:  why = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: why = "task watchdog (something blocked too long)"; break;
    case ESP_RST_WDT:      why = "other watchdog"; break;
    case ESP_RST_BROWNOUT: why = "brownout (power dipped)"; break;
    default:               why = "unknown reset"; break;
  }
  return String("Jar restarted: ") + why + "\nHeap free " + String(ESP.getFreeHeap()) +
         ", lowest " + String(ESP.getMinFreeHeap()) + prevInfoText() +
         "\nSend /log for the full log.";
}

String statusText() {
  return "Signal: " + String(WiFi.RSSI()) + " dBm\nUptime: " +
         String(millis() / 60000) + " min\nJar time: " + timeNow() +
         "\nQuiet now: " + String(darkFlag ? "yes" : "no") +
         "\nQuiet hours: " + String(cfg.quietOn
             ? hourLabel(cfg.quietStart) + " to " + hourLabel(cfg.quietEnd)
             : String("off")) +
         "\nDouble click: " + String(dblFlag ? "on" : "off") +
         "\nBuild: " + BUILD_TAG;
}

String panelButton(const String& label, const String& data) {
  return "{\"text\":\"" + label + "\",\"callback_data\":\"" + data + "\"}";
}

String panelJson() {
  String j = "[";
  for (int r = 0; r < 2; r++) {
    if (r > 0) j += ",";
    j += "[";
    for (int c = 0; c < 4; c++) {
      int idx = r * 4 + c;
      if (c > 0) j += ",";
      j += panelButton(PANEL_EMOJI[idx], String("c:") + PANEL_NAME[idx]);
    }
    j += "]";
  }
  if (dblFlag) {
    const int pick[4] = {0, 3, 4, 6};
    j += ",[";
    for (int c = 0; c < 4; c++) {
      if (c > 0) j += ",";
      int idx = pick[c];
      j += panelButton(String("2 ") + PANEL_EMOJI[idx], String("c2:") + PANEL_NAME[idx]);
    }
    j += "]";
  }
  j += ",[" + panelButton("Pulse", "p:1");
  if (dblFlag) j += "," + panelButton("Pulse 2", "p:2");
  j += "],[" + panelButton("Status", "s:status") + "," + panelButton("Settings", "s:settings") +
       "," + panelButton("Log", "s:log") + "]]";
  return j;
}

void sendPanel(UniversalTelegramBot& bot) {
  String title = dblFlag ? "Tap a color to light her jar (top = pulse, row 3 = second pulse):"
                         : "Tap a color to light her jar:";
  bot.sendMessageWithInlineKeyboard(CHAT_ID, title, "", panelJson());
}

void sendPulseEvent(UniversalTelegramBot& bot, uint8_t type, const String& colorText) {
  InEvent ev;
  ev.type = type;
  ev.hasColor = false;
  ev.rgb = 0;

  if (colorText.length() > 0) {
    uint32_t rgb;
    if (!parseColor(colorText, rgb)) {
      bot.sendMessage(CHAT_ID,
        "Couldn't read that color. Try a name (red, pink, blue, mint, white, warm...), "
        "hex like #ff8800, or three numbers like 255 120 0.", "");
      return;
    }
    ev.hasColor = true;
    ev.rgb = rgb;
  }

  xQueueSend(eventQueue, &ev, 0);
  if (darkFlag && !cfg.pulseInDark) {
    bot.sendMessage(CHAT_ID, "Jar is in quiet mode. It will glow when quiet mode ends.", "");
  } else {
    bot.sendMessage(CHAT_ID, colorText.length() > 0 ? String("Glowing ") + colorText : String("Glowing now"), "");
  }
}

void handleMessage(UniversalTelegramBot& bot, int i) {
  String chat = bot.messages[i].chat_id;
  String text = bot.messages[i].text;
  text.trim();
  text.toLowerCase();

  if (chat != String(CHAT_ID)) {
    Serial.println("Ignored a message from an unknown chat");
    return;
  }

  // Taps on the inline panel arrive as "callback_query" updates.
  if (bot.messages[i].type == "callback_query") {
    if (text.startsWith("c:")) {
      sendPulseEvent(bot, EVT_PULSE, text.substring(2));
    } else if (text.startsWith("c2:")) {
      if (dblFlag) sendPulseEvent(bot, EVT_PULSE2, text.substring(3));
    } else if (text == "p:1") {
      sendPulseEvent(bot, EVT_PULSE, "");
    } else if (text == "p:2") {
      if (dblFlag) sendPulseEvent(bot, EVT_PULSE2, "");
    } else if (text == "s:status") {
      bot.sendMessage(CHAT_ID, statusText(), "");
    } else if (text == "s:settings") {
      bot.sendMessage(CHAT_ID, "Settings page (open on the jar's WiFi):\n" + settingsLink(), "");
    } else if (text == "s:log") {
      bot.sendMessage(CHAT_ID, logTail(3500), "");
    }
    return;
  }

  // Split into a command word and the rest.
  String cmd = text;
  String rest = "";
  int sp = text.indexOf(' ');
  if (sp >= 0) {
    cmd = text.substring(0, sp);
    rest = text.substring(sp + 1);
    rest.trim();
  }
  if (cmd.startsWith("/")) cmd.remove(0, 1);
  int at = cmd.indexOf('@');
  if (at > 0) cmd = cmd.substring(0, at);

  // "pulse 2 blue" is the same as "pulse2 blue"
  if (cmd == "pulse" && (rest == "2" || rest.startsWith("2 "))) {
    cmd = "pulse2";
    rest = rest.substring(1);
    rest.trim();
  }

  if (cmd == "pulse") {
    sendPulseEvent(bot, EVT_PULSE, rest);
  } else if (cmd == "pulse2") {
    if (!dblFlag) {
      bot.sendMessage(CHAT_ID, "Second pulse is off. Send /doubleclick enable first.", "");
    } else {
      sendPulseEvent(bot, EVT_PULSE2, rest);
    }
  } else if (cmd == "doubleclick") {
    if (rest == "enable") {
      InEvent ev;
      ev.type = EVT_DBL_ON;
      ev.hasColor = false;
      ev.rgb = 0;
      xQueueSend(eventQueue, &ev, 0);
      dblFlag = true;
      sendKeyboard(bot, "Double click enabled. She can now set a second color and message "
                        "in her settings. Use Pulse 2, or /pulse2 [color].");
      sendPanel(bot);
    } else if (rest == "disable") {
      InEvent ev;
      ev.type = EVT_DBL_OFF;
      ev.hasColor = false;
      ev.rgb = 0;
      xQueueSend(eventQueue, &ev, 0);
      dblFlag = false;
      sendKeyboard(bot, "Double click disabled. A double press toggles night mode again.");
      sendPanel(bot);
    } else {
      bot.sendMessage(CHAT_ID, String("Double click is ") + (dblFlag ? "on" : "off") +
                      ". Use /doubleclick enable or /doubleclick disable.", "");
    }
  } else if (cmd == "menu" || cmd == "panel") {
    sendPanel(bot);
  } else if (cmd == "start" || cmd == "help") {
    sendKeyboard(bot,
      "Tap Pulse to light her jar.\n"
      "/pulse red  (any color: name, #hex, or r g b)\n"
      "/doubleclick enable | disable\n"
      "/pulse2 [color]  (when double click is on)\n"
      "/menu  /settings  /status  /log  /clearlog");
    sendPanel(bot);
  } else if (cmd == "log") {
    bot.sendMessage(CHAT_ID, logTail(3500), "");
  } else if (cmd == "clearlog") {
    LittleFS.remove("/log.txt");
    LittleFS.remove("/log.old");
    bot.sendMessage(CHAT_ID, "Log cleared.", "");
  } else if (cmd == "settings") {
    bot.sendMessage(CHAT_ID, "Settings page (open on the jar's WiFi):\n" + settingsLink(), "");
  } else if (cmd == "status") {
    bot.sendMessage(CHAT_ID, statusText(), "");
  }
}

void networkTask(void* param) {
  WiFiClientSecure client;
  client.setInsecure();   // skips certificate checking
  UniversalTelegramBot bot(BOT_TOKEN, client);

  unsigned long lastPoll = 0;
  unsigned long lastHealth = 0;
  unsigned long lastWifiTry = 0;
  bool bootAnnounced = false;   // once per boot, never repeated after a WiFi blip

  for (;;) {
    netBeat = millis();
    {
      uint32_t sf = uxTaskGetStackHighWaterMark(NULL);
      if (netStackFree == 0 || sf < netStackFree) netStackFree = sf;
    }

    if (WiFi.status() != WL_CONNECTED) {
      wifiUp = false;
      if (!portalFlag && millis() - lastWifiTry > 10000) {
        lastWifiTry = millis();
        WiFi.reconnect();
      }
      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue;
    }

    if (!wifiUp) {
      wifiUp = true;
      lastTelegramOk = millis();
    }

    if (!bootAnnounced) {
      bootAnnounced = true;
      String a = announceText();
      if (a.length() > 0) {
        Serial.print("WiFi connected, IP ");
        Serial.println(WiFi.localIP());
        String s = a + "\nSignal: " + String(WiFi.RSSI()) + " dBm\nSettings:\n" + settingsLink();
        sendKeyboard(bot, s);
      }
    }

    OutMsg m;
    while (xQueueReceive(outQueue, &m, 0) == pdTRUE) {
      bot.sendMessage(CHAT_ID, String(m.text), "");
    }

    if (millis() - lastPoll >= POLL_MS) {
      lastPoll = millis();
      int n = bot.getUpdates(bot.last_message_received + 1);
      while (n) {
        for (int i = 0; i < n; i++) {
          handleMessage(bot, i);
        }
        n = bot.getUpdates(bot.last_message_received + 1);
      }
    }

    if (millis() - lastHealth >= 30000) {
      lastHealth = millis();
      if (bot.getMe()) {
        lastTelegramOk = millis();
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ---- Settings web page ------------------------------------------------------
bool previewRequested = false;
uint32_t previewRGB = 0;
bool webStarted = false;
bool wasConnected = false;
bool loggedConnected = true;

bool authed() {
  if (strlen(SETTINGS_PASSWORD) == 0) return true;
  if (server.authenticate("jar", SETTINGS_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

void addOption(String& h, int val, const String& label, bool selected) {
  h += "<option value='" + String(val) + "'" + (selected ? " selected" : "") + ">" + label + "</option>";
}

void addColorField(String& h, const char* label, const char* name, uint32_t rgb) {
  h += "<label>" + String(label) + "</label><input type='color' name='" + String(name) +
       "' value='" + hexOf(rgb) + "'>";
}

void addTextField(String& h, const char* label, const char* name, const char* value) {
  h += "<label>" + String(label) + "</label><input type='text' name='" + String(name) +
       "' maxlength='60' value='" + htmlEsc(value) + "'>";
}

// Tap-to-fill swatches that set a color picker to one of a list of preset colors.
void addPresets(String& h, const char* target, const NamedColor* list, int count) {
  h += "<div class='sws'>";
  for (int i = 0; i < count; i++) {
    uint32_t rgb = list[i].rgb;
    int lum = (int)((((rgb >> 16) & 0xFF) * 299 + ((rgb >> 8) & 0xFF) * 587 + (rgb & 0xFF) * 114) / 1000);
    String fg = (lum < 120) ? "#ffffff" : "#1c1917";
    h += "<button type='button' class='sw' style='background:" + hexOf(rgb) + ";color:" + fg +
         "' onclick=\"document.getElementsByName('" + String(target) + "')[0].value='" +
         hexOf(rgb) + "'\">" + String(list[i].name) + "</button>";
  }
  h += "</div>";
}

void handleRoot() {
  if (!authed()) return;
  String h;
  h.reserve(7000);
  h += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Mikayla's Jar</title><style>"
         "body{font-family:-apple-system,system-ui,sans-serif;background:#1c1917;color:#f5f5f4;"
         "margin:0 auto;padding:20px;max-width:480px}"
         "h1{font-size:22px}h2{font-size:17px;margin:28px 0 0;color:#fbbf24}"
         "label{display:block;margin:16px 0 6px;font-size:15px;color:#d6d3d1}"
         "select,input[type=range],input[type=text]{width:100%;font-size:17px;padding:10px;"
         "border-radius:10px;border:0;background:#292524;color:#f5f5f4;box-sizing:border-box}"
         "input[type=color]{width:100%;height:48px;border:0;border-radius:10px;background:#292524;padding:4px;"
         "box-sizing:border-box}"
         "button{width:100%;font-size:17px;padding:14px;margin-top:12px;border:0;border-radius:12px;"
         "background:#f59e0b;color:#1c1917;font-weight:600}"
         "button.alt{background:#44403c;color:#f5f5f4}"
         ".sws{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}"
         "button.sw{width:calc(33.3% - 6px);margin-top:0;padding:10px 4px;font-size:13px;color:#1c1917}"
         ".ok{background:#14532d;padding:10px;border-radius:10px}"
         ".row{display:flex;gap:10px}.row>div{flex:1}"
         "small{color:#a8a29e}</style></head><body>");
  h += F("<h1>Mikayla's Jar</h1>");
  if (server.hasArg("saved")) h += F("<p class='ok'>Saved.</p>");
  h += "<small>Jar time: " + timeNow() + "</small>";

  h += F("<form method='post' action='/save'>");

  h += F("<h2>When a pulse arrives</h2>");
  addColorField(h, "Incoming pulse color", "pc", cfg.pulseRGB);
  addPresets(h, "pc", PULSE_PRESETS, PULSE_PRESET_COUNT);
  addTextField(h, "Message sent when you press the jar", "m1", cfg.msg1);

  if (cfg.dblOn) {
    h += F("<h2>Second pulse</h2>");
    addColorField(h, "Second incoming pulse color", "pc2", cfg.pulse2RGB);
    addPresets(h, "pc2", PULSE_PRESETS, PULSE_PRESET_COUNT);
    addTextField(h, "Message sent when you double click the jar", "m2", cfg.msg2);
  }

  h += F("<h2>Pulse behavior</h2><label>Stop a pulse if it isn't seen after</label><select name='tmo'>");
  for (int i = 0; i < TIMEOUT_COUNT; i++) addOption(h, i, TIMEOUT_NAMES[i], i == cfg.timeoutIdx);
  h += F("</select>");
  h += F("<label><input type='checkbox' name='pind' value='1'");
  if (cfg.pulseInDark) h += F(" checked");
  h += F("> Let pulses glow during quiet hours</label>");

  h += F("<h2>Idle glow</h2>");
  h += F("<label><input type='checkbox' name='ion' value='1'");
  if (cfg.idleOn) h += F(" checked");
  h += F("> Keep a soft glow when nothing is happening</label>");
  addColorField(h, "Idle color", "ic", cfg.idleRGB);
  addPresets(h, "ic", LAMP, LAMP_COUNT);
  h += "<label>Idle brightness</label><input type='range' name='ipct' min='3' max='50' step='1' value='" +
       String(cfg.idlePct) + "'>";

  h += F("<h2>General</h2>");
  h += "<label>Overall brightness</label><input type='range' name='bright' min='20' max='100' step='5' value='" +
       String(cfg.brightnessPct) + "'>";

  h += F("<label>Timezone</label><select name='tz'>");
  for (int i = 0; i < TZ_COUNT; i++) addOption(h, i, TZ_NAMES[i], i == cfg.tzIdx);
  h += F("</select>");

  h += F("<label><input type='checkbox' name='quiet' value='1'");
  if (cfg.quietOn) h += F(" checked");
  h += F("> Quiet hours (jar stays dark)</label>");

  h += F("<div class='row'><div><label>From</label><select name='qs'>");
  for (int i = 0; i < 24; i++) addOption(h, i, hourLabel(i), i == cfg.quietStart);
  h += F("</select></div><div><label>Until</label><select name='qe'>");
  for (int i = 0; i < 24; i++) addOption(h, i, hourLabel(i), i == cfg.quietEnd);
  h += F("</select></div></div>");

  h += F("<button type='submit'>Save</button></form>");

  h += F("<form method='post' action='/test'><h2>Preview (saved colors)</h2>"
         "<button class='alt' type='submit' name='which' value='0'>Incoming pulse</button>");
  if (cfg.dblOn) h += F("<button class='alt' type='submit' name='which' value='1'>Second pulse</button>");
  h += F("<button class='alt' type='submit' name='which' value='2'>Idle glow</button></form>"
         "<p><small>Save first, then preview. Each preview lasts a few seconds.</small></p>"
         "</body></html>");
  server.send(200, "text/html", h);
}

uint32_t argColor(const char* name, uint32_t fallback) {
  String v = server.arg(name);
  if (v.length() == 7 && v[0] == '#') {
    return (uint32_t)strtoul(v.c_str() + 1, nullptr, 16);
  }
  return fallback;
}

void handleSave() {
  if (!authed()) return;
  int oldTz = cfg.tzIdx;

  cfg.pulseRGB = argColor("pc", cfg.pulseRGB);
  if (server.hasArg("m1")) copyMsg(cfg.msg1, sizeof(cfg.msg1), server.arg("m1"), DEFAULT_MSG1);
  if (cfg.dblOn) {
    cfg.pulse2RGB = argColor("pc2", cfg.pulse2RGB);
    if (server.hasArg("m2")) copyMsg(cfg.msg2, sizeof(cfg.msg2), server.arg("m2"), DEFAULT_MSG2);
  }

  cfg.timeoutIdx  = constrain(server.arg("tmo").toInt(), 0, TIMEOUT_COUNT - 1);
  cfg.pulseInDark = server.hasArg("pind");

  cfg.idleOn  = server.hasArg("ion");
  cfg.idleRGB = argColor("ic", cfg.idleRGB);
  cfg.idlePct = constrain(server.arg("ipct").toInt(), 3, 50);

  cfg.brightnessPct = constrain(server.arg("bright").toInt(), 20, 100);
  cfg.tzIdx         = constrain(server.arg("tz").toInt(), 0, TZ_COUNT - 1);
  cfg.quietOn       = server.hasArg("quiet");
  cfg.quietStart    = constrain(server.arg("qs").toInt(), 0, 23);
  cfg.quietEnd      = constrain(server.arg("qe").toInt(), 0, 23);

  saveSettings();
  applyBrightness();
  if (cfg.tzIdx != oldTz) applyTimezone();
  quietNow = computeQuiet();
  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

void handleTest() {
  if (!authed()) return;
  int which = server.arg("which").toInt();
  if (which == 1 && cfg.dblOn) previewRGB = cfg.pulse2RGB;
  else if (which == 2) previewRGB = cfg.idleRGB;
  else previewRGB = cfg.pulseRGB;
  previewRequested = true;
  server.sendHeader("Location", "/");
  server.send(303);
}

void startWeb() {
  MDNS.end();
  if (MDNS.begin("jar")) {
    MDNS.addService("http", "tcp", 80);
  }
  if (!webStarted) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/test", HTTP_POST, handleTest);
    server.begin();
    webStarted = true;
  }
}

// ---- Ring helpers -----------------------------------------------------------
void showColor(uint8_t r, uint8_t g, uint8_t b, float level) {
  if (level < 0) level = 0;
  if (level > 1) level = 1;
  uint32_t c = ring.Color(scaleCh(r, level), scaleCh(g, level), scaleCh(b, level));
  for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, c);
  ring.show();
}

void showRGB(uint32_t rgb, float level) {
  showColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, level);
}

float breathWave(unsigned long now, float seconds) {
  unsigned long period = (unsigned long)(seconds * 1000);
  float t = (now % period) / (float)period;
  return (1.0 - cos(t * 2.0 * PI)) / 2.0;
}

void showWhiteFill(int count) {
  for (int i = 0; i < LED_COUNT; i++) {
    ring.setPixelColor(i, i < count ? ring.Color(scaleCh(255, 1.0f), scaleCh(255, 1.0f), scaleCh(255, 1.0f)) : 0);
  }
  ring.show();
}

// ---- Button: short, double and long presses ------------------------------------
bool rawLast = HIGH;
bool isDown = false;
unsigned long rawChanged = 0;
unsigned long pressStart = 0;
bool waitingSecond = false;
unsigned long firstReleaseAt = 0;
bool resetFired = false;
unsigned long heldMs = 0;

ButtonEvent readButton(unsigned long now) {
  bool raw = digitalRead(BUTTON_PIN);
  if (raw != rawLast) {
    rawLast = raw;
    rawChanged = now;
  }

  ButtonEvent ev = BTN_NONE;

  if (now - rawChanged > 30) {
    bool down = (raw == LOW);
    if (down && !isDown) {
      isDown = true;
      pressStart = now;
      resetFired = false;
    } else if (!down && isDown) {
      isDown = false;
      unsigned long held = now - pressStart;
      if (held < HOLD_SHOW_MS) {
        if (waitingSecond && (now - firstReleaseAt) < DOUBLE_MS) {
          waitingSecond = false;
          ev = BTN_DOUBLE;
        } else {
          waitingSecond = true;
          firstReleaseAt = now;
        }
      } else {
        waitingSecond = false;
      }
    }
  }

  heldMs = isDown ? (now - pressStart) : 0;

  if (isDown && heldMs >= HOLD_RESET_MS && !resetFired) {
    resetFired = true;
    waitingSecond = false;
    ev = BTN_RESET;
  }

  if (ev == BTN_NONE && waitingSecond && !isDown &&
      (now - firstReleaseAt) >= DOUBLE_MS) {
    waitingSecond = false;
    ev = BTN_SHORT;
  }

  return ev;
}

// ---- Main loop state ------------------------------------------------------------
JarState state = ST_IDLE;
unsigned long stateStart = 0;
uint32_t stateRGB = 0xFF7800;   // color used by the current state
uint8_t activeKind = 1;         // 1 = pulse, 2 = second pulse
float lastLevel = 0;
float fadeFrom = 0;

bool pulsePending = false;
uint8_t pendingKind = 1;
uint32_t pendingRGB = 0xFF7800;

bool prevDark = false;

unsigned long wifiDownSince = 0;
unsigned long portalSeenAt = 0;
unsigned long lastHousekeep = 0;
unsigned long lowHeapSince = 0;
unsigned long lastSample = 0;

// Pulses may glow when it is not dark, or when she allowed glowing in the dark.
bool canGlow() {
  return !prevDark || cfg.pulseInDark;
}

void startPulse(unsigned long now, uint8_t kind, uint32_t rgb) {
  state = ST_PULSING;
  stateStart = now;
  stateRGB = rgb;
  activeKind = kind;
  pulsePending = false;
  Serial.println(kind == 2 ? "Second pulse started" : "Pulse started");
  logLine(kind == 2 ? "second pulse started" : "pulse started");
}

// She pressed the jar: `second` is true for a double click.
void handlePress(unsigned long now, bool second) {
  if (state == ST_PULSING) {
    char buf[80];
    unsigned long mins = (now - stateStart) / 60000UL;
    const char* what = (activeKind == 2) ? "She saw the second pulse" : "She saw it";
    if (mins == 0) {
      snprintf(buf, sizeof(buf), "%s", what);
    } else {
      snprintf(buf, sizeof(buf), "%s (after %lu min)", what, mins);
    }
    queueMessage(buf);
    clearPulse();
    fadeFrom = lastLevel;
    state = ST_ACK_FADE;
    stateStart = now;
  } else if (state == ST_IDLE) {
    queueMessage(second ? cfg.msg2 : cfg.msg1);
    if (!prevDark) {
      state = ST_SOFT_GLOW;
      stateRGB = second ? cfg.pulse2RGB : cfg.pulseRGB;
      stateStart = now;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Jar of Thoughts - stage 5");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  LittleFS.begin(true);   // true = format on first use
  if (lastInfo.magic == CRASH_MAGIC) {
    prevInfo = lastInfo;
    havePrev = true;
  }
  lastInfo.upSec = 0;
  lastInfo.freeHeap = ESP.getFreeHeap();
  lastInfo.minHeap = ESP.getMinFreeHeap();
  lastInfo.netStackFree = 0;
  lastInfo.loopStackFree = 0;
  lastInfo.rssi = 0;
  lastInfo.telegramAgeSec = 0;
  lastInfo.stage = 0;
  lastInfo.magic = CRASH_MAGIC;
  loadSettings();
  dblFlag = cfg.dblOn;
  {
    uint8_t pk = prefs.getUChar("pk", 0);
    if (pk == 1 || (pk == 2 && cfg.dblOn)) {
      pendingKind = pk;
      pendingRGB = prefs.getUInt("prgb", cfg.pulseRGB);
      pulsePending = true;          // starts as soon as the main loop runs
    } else if (pk != 0) {
      clearPulse();
    }
  }

  ring.begin();
  applyBrightness();
  ring.clear();
  ring.show();

  eventQueue = xQueueCreate(6, sizeof(InEvent));
  outQueue   = xQueueCreate(6, sizeof(OutMsg));

  showColor(0, 40, 255, 0.15);

  WiFi.mode(WIFI_STA);
  applyTimezone();
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(0);
  if (wm.autoConnect(SETUP_AP_NAME, SETUP_AP_PASSWORD)) {
    Serial.println("Connected with saved WiFi");
  } else {
    Serial.println("No WiFi yet: setup portal is open");
  }

  logLine(String("boot: ") + resetName() + " [" + BUILD_TAG + "], heap " + String(ESP.getFreeHeap()) +
          prevInfoText());
  netBeat = millis();
  xTaskCreatePinnedToCore(networkTask, "net", 16384, NULL, 1, NULL, 0);
  lastInfo.stage = 1;
}

void loop() {
  unsigned long now = millis();

  wm.process();

  bool portalOpen = wm.getConfigPortalActive();
  portalFlag = portalOpen;

  // Settings page: only once WiFi is up and the setup portal is closed.
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !portalOpen) {
    if (!webStarted || !wasConnected) startWeb();
    server.handleClient();
  }
  wasConnected = connected && !portalOpen;
  if (connected != loggedConnected) {
    loggedConnected = connected;
    logLine(connected ? "wifi up" : "wifi down");
  }

  if (portalOpen) {
    if (portalSeenAt == 0) portalSeenAt = now;
    if (now - portalSeenAt > 300000UL && wm.getWiFiIsSaved()) {
      restartNow("Portal open 5 min with saved WiFi, restarting");
    }
  } else {
    portalSeenAt = 0;
  }

  if (!connected) {
    if (wifiDownSince == 0) wifiDownSince = now;
    if (!portalOpen && now - wifiDownSince > 600000UL) {
      restartNow("No WiFi for 10 min, restarting");
    }
  } else {
    wifiDownSince = 0;
  }

  // Once-a-second housekeeping: quiet hours, watchdogs, nightly restart.
  if (now - lastHousekeep >= 1000) {
    lastHousekeep = now;
    quietNow = computeQuiet();

    // Keep a fresh snapshot in RTC memory so a crash can be explained afterwards.
    lastInfo.upSec = now / 1000;
    lastInfo.freeHeap = ESP.getFreeHeap();
    lastInfo.minHeap = ESP.getMinFreeHeap();
    lastInfo.netStackFree = netStackFree;
    lastInfo.loopStackFree = uxTaskGetStackHighWaterMark(NULL);
    lastInfo.rssi = WiFi.RSSI();
    long tgAge = (long)(now - lastTelegramOk);
    if (tgAge < 0) tgAge = 0;
    lastInfo.telegramAgeSec = tgAge / 1000;
    lastInfo.magic = CRASH_MAGIC;

    // A line in the log every 5 minutes.
    if (now - lastSample >= 300000UL) {
      lastSample = now;
      logLine("ok heap " + String(lastInfo.freeHeap) + " min " + String(lastInfo.minHeap) +
              " netstack " + String(netStackFree) + " loopstack " + String(lastInfo.loopStackFree) +
              " rssi " + String(lastInfo.rssi));
    }

    bool idle = (state == ST_IDLE && !pulsePending);

    if (now > NET_STALL_MS && (long)(now - netBeat) > (long)NET_STALL_MS) {
      restartNow("Network task stalled, restarting");
    }

    if (ESP.getFreeHeap() < 15000) {
      if (lowHeapSince == 0) lowHeapSince = now;
      if (idle && now - lowHeapSince > 60000UL) {
        restartNow("Memory low, restarting");
      }
    } else {
      lowHeapSince = 0;
    }

    if (NIGHTLY_RESTART && idle && now > 3600000UL && timeKnown()) {
      time_t t = time(nullptr);
      struct tm tmv;
      localtime_r(&t, &tmv);
      if (tmv.tm_hour == RESTART_HOUR && tmv.tm_min < 5) {
        restartNow("Nightly restart");
      }
    }
  }

  // 1. Commands arriving from Telegram.
  InEvent ie;
  while (xQueueReceive(eventQueue, &ie, 0) == pdTRUE) {
    if (ie.type == EVT_PULSE || ie.type == EVT_PULSE2) {
      uint8_t kind = (ie.type == EVT_PULSE) ? 1 : 2;
      if (kind == 2 && !cfg.dblOn) continue;
      uint32_t rgb = ie.hasColor ? ie.rgb : (kind == 1 ? cfg.pulseRGB : cfg.pulse2RGB);
      savePulse(kind, rgb);   // remember it in case of a reboot
      if (!canGlow()) {
        pendingKind = kind;
        pendingRGB = rgb;
        pulsePending = true;
        Serial.println("Pulse held until quiet mode ends");
      } else {
        startPulse(now, kind, rgb);
      }
    } else if (ie.type == EVT_DBL_ON) {
      cfg.dblOn = true;
      nightMode = false;   // double press no longer toggles night mode
      prefs.putBool("dbl", true);
      Serial.println("Double click ENABLED");
    } else if (ie.type == EVT_DBL_OFF) {
      cfg.dblOn = false;
      prefs.putBool("dbl", false);
      Serial.println("Double click DISABLED");
    }
  }

  // 2. Button.
  ButtonEvent ev = readButton(now);

  if (ev == BTN_RESET) {
    Serial.println("Hold 10 s: erasing WiFi and restarting");
    showColor(255, 255, 255, 1.0);
    delay(300);
    wm.resetSettings();
    ESP.restart();
  } else if (ev == BTN_DOUBLE) {
    if (cfg.dblOn) {
      Serial.println("double click");
      handlePress(now, true);
    } else {
      nightMode = !nightMode;
      Serial.println(nightMode ? "Night mode ON" : "Night mode OFF");
    }
  } else if (ev == BTN_SHORT) {
    Serial.println("button pressed");
    handlePress(now, false);
  }

  // Dark = manual night mode OR quiet hours. React when it changes.
  bool dark = nightMode || quietNow;
  if (dark != prevDark) {
    prevDark = dark;
    darkFlag = dark;
    Serial.println(dark ? "Dark ON" : "Dark OFF");
    logLine(dark ? "dark on" : "dark off");
    if (dark && state != ST_PULSING) state = ST_IDLE;
  }

  // A pulse that may not glow right now waits; a waiting pulse starts once it may.
  if (state == ST_PULSING && !canGlow()) {
    pendingKind = activeKind;
    pendingRGB = stateRGB;
    pulsePending = true;
    state = ST_IDLE;
  }
  if (pulsePending && state == ST_IDLE && canGlow()) {
    startPulse(now, pendingKind, pendingRGB);
  }

  // Preview request from the settings page.
  if (previewRequested) {
    previewRequested = false;
    if (state == ST_IDLE) {
      state = ST_PREVIEW;
      stateRGB = previewRGB;
      stateStart = now;
    }
  }

  // 3. Decide what the ring shows. Highest priority first.

  // Priority 1: reset hold.
  if (isDown && heldMs >= HOLD_SHOW_MS) {
    float frac = (float)(heldMs - HOLD_SHOW_MS) / (float)(HOLD_RESET_MS - HOLD_SHOW_MS);
    if (frac > 1) frac = 1;
    int count = (int)ceil(frac * LED_COUNT);
    if (count < 1) count = 1;
    showWhiteFill(count);
    lastLevel = 0;
    delay(16);
    return;
  }

  // Priority 2: setup mode.
  if (portalOpen) {
    showColor(0, 40, 255, 0.05 + 0.5 * breathWave(now, 3.0));
    lastLevel = 0;
    delay(16);
    return;
  }

  // Priority 3: fault lights (never while dark).
  if (!prevDark) {
    bool noWifi = (wifiDownSince != 0) && (now - wifiDownSince > FAULT_AFTER_MS);
    bool noTelegram = wifiUp && ((long)(now - lastTelegramOk) > (long)FAULT_AFTER_MS);
    if (noWifi) {
      bool on = ((now / 1000) % 2) == 0;
      showColor(255, 0, 0, on ? FAULT_LEVEL : 0);
      lastLevel = 0;
      delay(16);
      return;
    }
    if (noTelegram) {
      bool on = ((now / 250) % 2) == 0;
      showColor(255, 0, 0, on ? FAULT_LEVEL : 0);
      lastLevel = 0;
      delay(16);
      return;
    }
  }

  // Priorities 4-6: pulse, acknowledge, idle glow (or dark).
  float level = 0;
  float elapsed = (now - stateStart) / 1000.0;
  uint32_t drawRGB = stateRGB;

  switch (state) {
    case ST_IDLE:
      drawRGB = cfg.idleRGB;
      level = (cfg.idleOn && !prevDark) ? (cfg.idlePct / 100.0) : 0;
      break;

    case ST_PULSING:
      level = 0.12 + 0.88 * breathWave(now, BREATH_SECONDS);
      {
        uint32_t tmo = TIMEOUT_MIN[cfg.timeoutIdx];
        if (tmo > 0 && now - stateStart > (unsigned long)tmo * 60000UL) {
          state = ST_IDLE;
          clearPulse();
          Serial.println("Pulse timed out");
          logLine("pulse timed out");
          queueMessage("Pulse expired: she hasn't pressed the jar yet");
        }
      }
      break;

    case ST_ACK_FADE:
      level = fadeFrom * (1.0 - elapsed / 1.0);
      if (elapsed >= 1.0) {
        state = ST_IDLE;
        level = 0;
      }
      break;

    case ST_SOFT_GLOW:
      level = 0.6 * (1.0 - elapsed / 0.6);
      if (elapsed >= 0.6) {
        state = ST_IDLE;
        level = 0;
      }
      break;

    case ST_PREVIEW:
      level = 0.12 + 0.88 * breathWave(now, 2.5);
      if (elapsed >= 5.0) {
        state = ST_IDLE;
        level = 0;
      }
      break;
  }

  if (level < 0) level = 0;
  lastLevel = level;
  showRGB(drawRGB, level);
  delay(16);
}
