// SMS -> Telegram forwarder for LilyGO T-A7670 (ESP32 + A7670 modem).
// Incoming SMS stay on the SIM until Telegram confirms delivery, so nothing is lost while WiFi is down.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>  // Library Manager: "WiFiManager" by tzapu
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_system.h>
#include <algorithm>
#include <map>
#include <vector>
#include "config.h"
#include "pdu.h"
#include "web_page.h"

// LilyGO T-A7670 ESP32 pins
#define MODEM_TX_PIN       26
#define MODEM_RX_PIN       27
#define MODEM_PWRKEY_PIN   4
#define BOARD_POWERON_PIN  12
#define MODEM_RESET_PIN    5
#define MODEM_DTR_PIN      25
#define SerialAT           Serial1

static const uint32_t INBOX_SCAN_INTERVAL_MS = 60UL * 1000;
static const uint32_t MULTIPART_TIMEOUT_MS   = 3UL * 60 * 1000;
static const uint32_t WIFI_RESTART_AFTER_MS  = 15UL * 60 * 1000;
static const size_t   TELEGRAM_MAX_BYTES     = 4000;
static const uint32_t MODEM_INFO_INTERVAL_MS = 30UL * 1000;
static const size_t   HISTORY_MAX            = 50;
static const size_t   SMS_MAX_PARTS          = 10;
static const size_t   MAX_RULES              = 30;

bool smsPending = true;
uint32_t lastScan = 0;
uint32_t wifiDownSince = 0;
int modemFailures = 0;
std::map<String, uint32_t> partialFirstSeen;  // multipart key -> millis() when first seen incomplete
String rxBuf;
String botToken;       // entered in the setup portal, stored in flash
String defaultChatId;  // same
String lastAtError;
String lastError;
uint32_t lastErrorAt = 0;
uint32_t forwardedCount = 0, failedCount = 0;
uint32_t lastModemInfo = 0;
bool modemReady = false;
esp_reset_reason_t resetReason;
WebServer server(80);

// Types and prototypes with default args up front, so the Arduino IDE's auto-generated prototypes compile
struct Route {
  String chatId;
  String text;
};

enum SendResult { SEND_OK, SEND_RETRY, SEND_REJECTED, SEND_BAD_TOKEN };

struct InboxEntry {
  int index;
  SmsPdu sms;
};

enum DeliverResult { DELIVER_FAIL = 0, DELIVER_OK, DELIVER_FALLBACK };

// Routing rules, edited in the web panel and stored in flash. Checked top to bottom, first match wins.
enum MatchType { STARTS_WITH, CONTAINS, FROM };
struct Rule {
  MatchType type;
  String pattern;
  String chatId;
};
std::vector<Rule> rules;

struct ModemInfo {
  bool sim = false;
  bool registered = false;
  int csq = -1;
  String op;
  int simUsed = 0, simTotal = 0;
  String clock;
};
ModemInfo modemInfo;

// Recent SMS for the web panel (RAM only, cleared on reboot)
struct HistoryItem {
  uint32_t id;
  bool outgoing;
  String peer;    // sender or recipient
  String time;
  String text;
  String chatId;  // Telegram chat it was forwarded to
  String status;  // sent / fallback / pending / failed
};
std::vector<HistoryItem> history;
uint32_t historySeq = 0;

bool ensureWiFi(uint32_t waitMs = 15000);
SendResult telegramSend(const String& chatId, const String& text, String* errDesc = nullptr);
bool sendAT(const String& cmd, String* resp = nullptr, uint32_t timeoutMs = 3000);

// ------------------------------------------------------------
// Text helpers
// ------------------------------------------------------------

// Lowercases ASCII, Latin-1, Latin Extended-A (č ć š ž đ) and Cyrillic.
// Every mapping keeps the UTF-8 byte length, so byte offsets match the original string.
String lowerUtf8(const String& s) {
  String o = s;
  for (unsigned i = 0; i < o.length(); i++) {
    uint8_t c = o[i];
    if (c >= 'A' && c <= 'Z') {
      o.setCharAt(i, c + 32);
    } else if ((c & 0xE0) == 0xC0 && i + 1 < o.length()) {
      uint16_t cp = ((c & 0x1F) << 6) | (o[i + 1] & 0x3F);
      uint16_t lc = cp;
      if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) lc = cp + 0x20;
      else if (cp >= 0x400 && cp <= 0x40F) lc = cp + 0x50;
      else if (cp >= 0x410 && cp <= 0x42F) lc = cp + 0x20;
      else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) lc = (cp & 1) ? cp + 1 : cp;
      else if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) lc = (cp & 1) ? cp : cp + 1;
      o.setCharAt(i, 0xC0 | (lc >> 6));
      o.setCharAt(i + 1, 0x80 | (lc & 0x3F));
      i++;
    }
  }
  return o;
}

bool isWordByte(uint8_t c) { return isalnum(c) || c == '_' || c >= 0x80; }

String digitsOf(const String& s) {
  String d;
  for (unsigned i = 0; i < s.length(); i++) if (isdigit((uint8_t)s[i])) d += s[i];
  return d;
}

bool looksLikePhone(const String& s) {
  for (unsigned i = 0; i < s.length(); i++) if (!strchr("+0123456789 -()", s[i])) return false;
  return digitsOf(s).length() > 0;
}

// Phone numbers: compare the last 9 digits so "+7999..." matches "8999...".
// Names: case-insensitive equality.
bool senderMatches(const String& sender, const String& pattern) {
  if (looksLikePhone(sender) && looksLikePhone(pattern)) {
    String a = digitsOf(sender), b = digitsOf(pattern);
    if (a.length() >= 9 && b.length() >= 9) return a.substring(a.length() - 9) == b.substring(b.length() - 9);
    return a == b;
  }
  return lowerUtf8(sender) == lowerUtf8(pattern);
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 16);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': break;
      case '\t': o += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          o += buf;
        } else {
          o += c;
        }
    }
  }
  return o;
}

String truncateUtf8(const String& s, size_t maxBytes) {
  if (s.length() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && ((uint8_t)s[cut] & 0xC0) == 0x80) cut--;
  return s.substring(0, cut) + "…";
}

void noteError(const String& e) {
  lastError = e;
  lastErrorAt = millis();
  Serial.printf("[err] %s\n", e.c_str());
}

void recordHistory(bool outgoing, const String& peer, const String& time, const String& text, const String& chatId,
                   const char* status) {
  // A pending incoming SMS is retried every minute; update its entry instead of adding a new one
  if (!outgoing) {
    for (HistoryItem& h : history) {
      if (!h.outgoing && h.peer == peer && h.time == time && h.text == text) {
        h.chatId = chatId;
        h.status = status;
        return;
      }
    }
  }
  history.push_back({++historySeq, outgoing, peer, time, text, chatId, status});
  if (history.size() > HISTORY_MAX) history.erase(history.begin());
}

// ------------------------------------------------------------
// Rules storage: one rule per line, "<type>\t<pattern>\t<chat>"
// ------------------------------------------------------------

const char* ruleTypeName(MatchType t) {
  switch (t) {
    case STARTS_WITH: return "STARTS_WITH";
    case CONTAINS:    return "CONTAINS";
    default:          return "FROM";
  }
}

bool parseRuleType(const String& name, MatchType& out) {
  if (name == "STARTS_WITH") out = STARTS_WITH;
  else if (name == "CONTAINS") out = CONTAINS;
  else if (name == "FROM") out = FROM;
  else return false;
  return true;
}

void loadRules() {
  Preferences p;
  p.begin("rules", true);
  String list = p.getString("list", "");
  p.end();
  rules.clear();
  for (int pos = 0; pos < (int)list.length();) {
    int nl = list.indexOf('\n', pos);
    if (nl < 0) nl = list.length();
    String line = list.substring(pos, nl);
    pos = nl + 1;
    int t1 = line.indexOf('\t'), t2 = line.indexOf('\t', t1 + 1);
    MatchType type;
    if (t1 < 0 || t2 < 0 || !parseRuleType(line.substring(0, t1), type)) continue;
    rules.push_back({type, line.substring(t1 + 1, t2), line.substring(t2 + 1)});
  }
  Serial.printf("[cfg] rules: %u\n", (unsigned)rules.size());
}

bool saveRules() {
  String list;
  for (const Rule& r : rules) list += String(ruleTypeName(r.type)) + "\t" + r.pattern + "\t" + r.chatId + "\n";
  Preferences p;
  p.begin("rules", false);
  bool ok = p.putString("list", list) == list.length();
  p.end();
  return ok;
}

// ------------------------------------------------------------
// Routing
// ------------------------------------------------------------

Route route(const String& sender, const String& text) {
  String lowText = lowerUtf8(text);
  unsigned lead = 0;
  while (lead < text.length() && isspace((uint8_t)text[lead])) lead++;

  for (const Rule& r : rules) {
    String pat = lowerUtf8(r.pattern);
    if (pat.length() == 0) continue;

    if (r.type == STARTS_WITH && lowText.substring(lead).startsWith(pat)) {
      unsigned end = lead + pat.length();
      // "WORK" must not match "WORKSHOP"; patterns ending in a symbol (e.g. "#w:") need no boundary
      bool needsBoundary = isWordByte(pat[pat.length() - 1]);
      if (needsBoundary && end < text.length() && isWordByte(text[end])) continue;
      if (!STRIP_PREFIX) return {r.chatId, text};
      while (end < text.length() && strchr(" \t\r\n:-,.", text[end])) end++;
      return {r.chatId, text.substring(end)};
    }
    if (r.type == CONTAINS && lowText.indexOf(pat) >= 0) return {r.chatId, text};
    if (r.type == FROM && senderMatches(sender, r.pattern)) return {r.chatId, text};
  }
  return {defaultChatId, text};
}

// ------------------------------------------------------------
// WiFi + Telegram
// ------------------------------------------------------------

bool ensureWiFi(uint32_t waitMs) {
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < waitMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

SendResult telegramSend(const String& chatId, const String& text, String* errDesc) {
  if (!ensureWiFi()) {
    Serial.println("[tg] no WiFi");
    return SEND_RETRY;
  }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, String("https://api.telegram.org/bot") + botToken + "/sendMessage")) return SEND_RETRY;
  http.addHeader("Content-Type", "application/json");

  String body = String("{\"chat_id\":\"") + jsonEscape(chatId) + "\",\"text\":\"" +
                jsonEscape(truncateUtf8(text, TELEGRAM_MAX_BYTES)) + "\",\"link_preview_options\":{\"is_disabled\":true}}";
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();

  if (code == 200) return SEND_OK;
  noteError("Telegram: " + (code > 0 ? "HTTP " + String(code) + " " + resp : String("no connection (") + code + ")"));
  if (code == 400 || code == 403) {
    if (errDesc) {
      int s = resp.indexOf("\"description\":\"");
      *errDesc = s >= 0 ? resp.substring(s + 15, resp.indexOf('"', s + 15)) : String(code);
    }
    return SEND_REJECTED;
  }
  if (code == 401 || code == 404) return SEND_BAD_TOKEN;
  return SEND_RETRY;  // network error, 429, 5xx
}

// If a routed channel rejects the message (bot not admin, wrong id), fall back to the default chat.
DeliverResult deliver(const String& chatId, const String& text) {
  String err;
  SendResult r = telegramSend(chatId, text, &err);
  if (r == SEND_OK) return DELIVER_OK;
  if (r == SEND_REJECTED && chatId != defaultChatId) {
    String note = text + "\n\n⚠️ Could not send to " + chatId + ": " + err;
    if (telegramSend(defaultChatId, note) == SEND_OK) return DELIVER_FALLBACK;
  }
  return DELIVER_FAIL;
}

// ------------------------------------------------------------
// Modem AT layer
// ------------------------------------------------------------

// Non-blocking: returns true once a complete non-empty line is available.
bool pollLine(String& line) {
  while (SerialAT.available()) {
    char c = SerialAT.read();
    if (c == '\n') {
      line = rxBuf;
      line.trim();
      rxBuf = "";
      if (line.length()) return true;
    } else if (c != '\r') {
      rxBuf += c;
    }
  }
  return false;
}

bool handleUrc(const String& line) {
  if (line.startsWith("+CMTI:")) {
    Serial.println("[modem] new SMS");
    smsPending = true;
    return true;
  }
  return false;
}

// Reads lines until OK / ERROR / timeout. Other lines go to resp, URCs are handled on the way.
bool waitFinal(const String& cmd, String* resp, uint32_t timeoutMs) {
  String line;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!pollLine(line)) {
      delay(2);
      continue;
    }
    if (handleUrc(line) || line == cmd) continue;  // URC or echo
    if (line == "OK") return true;
    if (line == "ERROR" || line.startsWith("+CME ERROR") || line.startsWith("+CMS ERROR")) {
      lastAtError = line;
      Serial.printf("[modem] %s -> %s\n", cmd.c_str(), line.c_str());
      return false;
    }
    if (resp) {
      *resp += line;
      *resp += '\n';
    }
  }
  lastAtError = "no response from modem";
  Serial.printf("[modem] %s -> timeout\n", cmd.c_str());
  return false;
}

bool sendAT(const String& cmd, String* resp, uint32_t timeoutMs) {
  String line;
  while (pollLine(line)) handleUrc(line);
  if (resp) *resp = "";
  SerialAT.print(cmd);
  SerialAT.print("\r\n");
  return waitFinal(cmd, resp, timeoutMs);
}

void modemPowerOn() {
  Serial.println("[modem] powering on");
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);  // already HIGH since setup(), kept here for restarts

  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  delay(2600);
  digitalWrite(MODEM_RESET_PIN, LOW);

  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);  // keep modem awake

  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
}

bool waitForModem(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (sendAT("AT", nullptr, 1000)) return true;
  }
  return false;
}

bool modemSetup() {
  if (!waitForModem(30000)) {
    noteError("Modem not responding");
    return false;
  }
  sendAT("ATE0");
  sendAT("AT+CMEE=2");
  sendAT("AT+CTZU=1");  // take time from the network, for the web panel

  String r;
  bool simReady = false;
  for (int i = 0; i < 15 && !simReady; i++) {
    if (sendAT("AT+CPIN?", &r)) {
      simReady = r.indexOf("READY") >= 0;
      if (!simReady && r.indexOf("SIM PIN") >= 0 && strlen(SIM_PIN)) {
        sendAT(String("AT+CPIN=\"") + SIM_PIN + "\"", nullptr, 5000);
      }
    }
    if (!simReady) delay(1000);
  }
  if (!simReady) {
    noteError("SIM not ready: no SIM card or PIN required");
    return false;
  }

  // SMS subsystem may need a few seconds after boot ("SMS DONE")
  for (int i = 0; i < 20; i++) {
    if (sendAT("AT+CMGF=0") && sendAT("AT+CPMS=\"SM\",\"SM\",\"SM\"") && sendAT("AT+CNMI=2,1,0,0,0")) {
      Serial.println("[modem] ready to receive SMS");
      return true;
    }
    delay(1500);
  }
  return false;
}

bool isRegistered() {
  String r;
  for (const char* cmd : {"AT+CEREG?", "AT+CREG?"}) {
    if (!sendAT(cmd, &r)) continue;
    int comma = r.indexOf(',');
    int stat = comma >= 0 ? r.substring(comma + 1).toInt() : 0;
    if (stat == 1 || stat == 5) return true;
  }
  return false;
}

bool waitForNetwork(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (isRegistered()) return true;
    delay(2000);
  }
  return false;
}

// Network time as "dd.mm.yy hh:mm", empty until the network has sent it
String modemClock() {
  String r;
  if (!sendAT("AT+CCLK?", &r)) return "";
  int q = r.indexOf('"');  // +CCLK: "26/10/09,14:32:10+12"
  if (q < 0 || (int)r.length() < q + 15) return "";
  String yy = r.substring(q + 1, q + 3);
  if (yy == "70" || yy == "80") return "";  // not synced yet
  return r.substring(q + 7, q + 9) + "." + r.substring(q + 4, q + 6) + "." + yy + " " + r.substring(q + 10, q + 15);
}

void refreshModemInfo() {
  lastModemInfo = millis();
  String r;
  modemInfo.sim = sendAT("AT+CPIN?", &r) && r.indexOf("READY") >= 0;
  modemInfo.registered = isRegistered();
  modemInfo.csq = sendAT("AT+CSQ", &r) ? r.substring(r.indexOf(':') + 1).toInt() : -1;
  modemInfo.op = "";
  if (sendAT("AT+COPS?", &r, 5000)) {
    int q = r.indexOf('"');
    if (q >= 0) modemInfo.op = r.substring(q + 1, r.indexOf('"', q + 1));
  }
  if (sendAT("AT+CPMS?", &r)) {  // +CPMS: "SM",3,50,...
    int c1 = r.indexOf(','), c2 = r.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > 0) {
      modemInfo.simUsed = r.substring(c1 + 1).toInt();
      modemInfo.simTotal = r.substring(c2 + 1).toInt();
    }
  }
  modemInfo.clock = modemClock();
}

String modemStatus() {
  refreshModemInfo();
  String s = "signal " + (modemInfo.csq < 0 || modemInfo.csq == 99 ? String("none") : String(modemInfo.csq) + "/31");
  if (modemInfo.op.length()) s += ", operator " + modemInfo.op;
  s += modemInfo.registered ? ", registered" : ", NOT registered on the network";
  return s;
}

// ------------------------------------------------------------
// Outgoing SMS and USSD
// ------------------------------------------------------------

bool modemSendPdu(const pdu::SubmitPdu& p, String& err) {
  String line;
  while (pollLine(line)) handleUrc(line);
  SerialAT.printf("AT+CMGS=%d\r", p.tpduLen);

  // The modem asks for the PDU with a "> " prompt that has no line ending
  String seen;
  bool prompt = false;
  uint32_t start = millis();
  while (!prompt && millis() - start < 5000) {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      if (c == '>') {
        prompt = true;
        break;
      }
      seen += c;
    }
    if (!prompt) delay(5);
  }
  if (seen.indexOf("+CMTI") >= 0) smsPending = true;
  if (!prompt) {
    SerialAT.write(0x1B);  // cancel
    seen.trim();
    err = seen.length() ? seen : String("modem not ready to send");
    return false;
  }
  SerialAT.print(p.hex);
  SerialAT.write(0x1A);
  if (waitFinal("AT+CMGS", nullptr, 60000)) return true;
  err = lastAtError;
  return false;
}

bool sendSms(const String& number, const String& text, int& parts, String& err) {
  std::vector<uint16_t> units = pdu::utf8ToUtf16(text);
  if (pdu::partsNeeded(units.size()) > SMS_MAX_PARTS) {
    err = "text too long (max " + String(SMS_MAX_PARTS) + " SMS)";
    return false;
  }
  static uint8_t ref = 0;
  std::vector<pdu::SubmitPdu> pdus = pdu::encodeSubmit(number, units, ++ref);
  parts = pdus.size();
  for (const pdu::SubmitPdu& p : pdus) {
    if (!modemSendPdu(p, err)) {
      noteError("SMS to " + number + " not sent: " + err);
      return false;
    }
  }
  Serial.printf("[sms] sent to %s (%d SMS)\n", number.c_str(), parts);
  return true;
}

bool isHexString(const String& s) {
  if (s.length() == 0 || s.length() % 4) return false;
  for (unsigned i = 0; i < s.length(); i++) if (!isxdigit((uint8_t)s[i])) return false;
  return true;
}

// Sends a USSD code (or a reply inside an open USSD menu) and waits for the operator's answer.
bool ussdRequest(const String& code, String& text, bool& sessionOpen) {
  sessionOpen = false;
  String resp;
  if (!sendAT("AT+CUSD=1,\"" + code + "\",15", &resp, 10000)) {
    text = lastAtError;
    return false;
  }
  // The answer comes as "+CUSD: <m>,"<text>",<dcs>"; the text may span several lines
  int at = resp.indexOf("+CUSD:");
  String cusd = at >= 0 ? resp.substring(at) : String();
  cusd.trim();
  auto complete = [](const String& c) {
    int quotes = 0;
    for (unsigned i = 0; i < c.length(); i++) if (c[i] == '"') quotes++;
    return c.length() && (quotes >= 2 || (quotes == 0 && c.indexOf(',') < 0));
  };
  uint32_t start = millis();
  String line;
  while (!complete(cusd) && millis() - start < 30000) {
    if (!pollLine(line)) {
      delay(5);
      continue;
    }
    if (handleUrc(line)) continue;
    if (cusd.length()) cusd += "\n" + line;
    else if (line.startsWith("+CUSD:")) cusd = line;
  }
  if (!cusd.length()) {
    text = "no response from the operator";
    return false;
  }

  int m = cusd.substring(6).toInt();
  int q1 = cusd.indexOf('"'), q2 = cusd.lastIndexOf('"');
  String body = (q1 >= 0 && q2 > q1) ? cusd.substring(q1 + 1, q2) : String();
  int dcs = q2 > 0 ? cusd.substring(cusd.indexOf(',', q2) + 1).toInt() : 15;
  if (((dcs & 0x0C) == 0x08 || dcs == 72) && isHexString(body)) {
    std::vector<uint8_t> bytes;
    pdu::hexToBytes(body, bytes);
    body = pdu::ucs2ToUtf8(bytes.data(), bytes.size());
  }
  sessionOpen = (m == 1);
  if (m == 2) text = body.length() ? body : String("the operator closed the request");
  else if (m == 4) text = "the operator does not support this request";
  else if (m == 5) text = "network timeout";
  else text = body;
  return m == 0 || m == 1 || m == 2;
}

// ------------------------------------------------------------
// Inbox processing
// ------------------------------------------------------------

void deleteSms(int index) { sendAT("AT+CMGD=" + String(index), nullptr, 5000); }

bool handleMessage(const String& sender, const String& time, const String& text, const std::vector<int>& indices) {
  Route r = route(sender, text);
  Serial.printf("[sms] from %s -> %s\n", sender.c_str(), r.chatId.c_str());

  String msg = SHOW_HEADER ? "📩 " + sender + " · " + time + "\n\n" + r.text : r.text;
  if (msg.length() == 0) msg = "(empty SMS from " + sender + ")";
  DeliverResult d = deliver(r.chatId, msg);
  if (d == DELIVER_FAIL) {
    Serial.println("[sms] not sent, will retry later");
    failedCount++;
    recordHistory(false, sender, time, text, r.chatId, "pending");
    return false;
  }
  forwardedCount++;
  recordHistory(false, sender, time, text, d == DELIVER_FALLBACK ? defaultChatId : r.chatId,
                d == DELIVER_FALLBACK ? "fallback" : "sent");
  for (int i : indices) deleteSms(i);
  return true;
}

void processInbox() {
  String resp;
  if (!sendAT("AT+CMGL=4", &resp, 15000)) {
    modemFailures++;
    return;
  }
  modemFailures = 0;

  // Response: "+CMGL: <idx>,<stat>,,<len>\n<pdu hex>\n" per message
  std::vector<InboxEntry> entries;
  int pendingIdx = -1;
  for (int pos = 0; pos < (int)resp.length();) {
    int nl = resp.indexOf('\n', pos);
    if (nl < 0) nl = resp.length();
    String line = resp.substring(pos, nl);
    pos = nl + 1;

    if (line.startsWith("+CMGL:")) {
      pendingIdx = line.substring(6).toInt();
      continue;
    }
    if (pendingIdx < 0) continue;
    int idx = pendingIdx;
    pendingIdx = -1;

    InboxEntry e{idx, SmsPdu()};
    DecodeResult res = pdu::decodeDeliver(line, e.sms);
    if (res == DECODE_OK) {
      entries.push_back(e);
    } else if (res == DECODE_NOT_DELIVER) {
      deleteSms(idx);  // delivery reports etc.
    } else {
      noteError("Could not decode SMS #" + String(idx));
      if (deliver(defaultChatId, "⚠️ Could not decode an SMS, raw PDU:\n" + line)) deleteSms(idx);
    }
  }

  // Single-part messages go out right away, parts of long ones are grouped first
  std::map<String, std::vector<InboxEntry*>> groups;
  for (InboxEntry& e : entries) {
    if (!e.sms.isConcat || e.sms.total <= 1) {
      handleMessage(e.sms.sender, e.sms.timestamp, e.sms.text, {e.index});
    } else {
      groups[e.sms.sender + "#" + String(e.sms.ref) + "#" + String(e.sms.total)].push_back(&e);
    }
  }

  for (auto it = partialFirstSeen.begin(); it != partialFirstSeen.end();) {
    if (groups.count(it->first)) ++it;
    else it = partialFirstSeen.erase(it);
  }

  uint32_t now = millis();
  for (auto& g : groups) {
    std::vector<InboxEntry*>& parts = g.second;
    std::sort(parts.begin(), parts.end(), [](InboxEntry* a, InboxEntry* b) { return a->sms.seq < b->sms.seq; });

    uint8_t total = parts[0]->sms.total;
    String text;
    std::vector<int> indices;
    unsigned expected = 1, missing = 0;
    for (InboxEntry* p : parts) {
      indices.push_back(p->index);
      if (p->sms.seq < expected) continue;  // duplicate part
      for (; expected < p->sms.seq; expected++, missing++) text += "[…]";
      text += p->sms.text;
      expected = p->sms.seq + 1;
    }
    for (; expected <= total; expected++, missing++) text += "[…]";

    if (missing) {
      // Wait for the remaining parts, then give up and send what arrived
      auto seen = partialFirstSeen.emplace(g.first, now).first;
      if (now - seen->second < MULTIPART_TIMEOUT_MS) continue;
      Serial.printf("[sms] %u of %u parts never arrived, sending what we have\n", missing, total);
    }
    if (handleMessage(parts[0]->sms.sender, parts[0]->sms.timestamp, text, indices)) partialFirstSeen.erase(g.first);
  }
}

// ------------------------------------------------------------
// Settings: WiFi, bot token, default chat
// ------------------------------------------------------------

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on / RST button";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "power dip (brownout)";
    case ESP_RST_DEEPSLEEP: return "wake from sleep";
    default:               return "unknown";
  }
}

// RST is a hardware reset the sketch can't see, so factory reset = two RST presses within 3 s.
// Only button / power-on resets count: a brownout or crash reboot must never wipe the settings.
bool doubleResetDetected() {
  Preferences p;
  p.begin("drd", false);
  if (resetReason != ESP_RST_POWERON && resetReason != ESP_RST_EXT) {
    p.putBool("armed", false);
    p.end();
    return false;
  }
  if (p.getBool("armed", false)) {
    p.putBool("armed", false);
    p.end();
    return true;
  }
  p.putBool("armed", true);
  Serial.println("Press RST again within 3 s to factory-reset");
  delay(3000);
  p.putBool("armed", false);
  p.end();
  return false;
}

void factoryReset() {
  Serial.println("[cfg] factory reset: WiFi, token, default chat and rules erased");
  WiFiManager wm;
  wm.resetSettings();
  Preferences p;
  p.begin("cfg", false);
  p.clear();
  p.end();
  p.begin("rules", false);
  p.clear();
  p.end();
}

void forgetAndRestart(const char* key, const char* why) {
  Serial.printf("[cfg] %s, opening the setup hotspot\n", why);
  Preferences p;
  p.begin("cfg", false);
  p.remove(key);
  p.end();
  delay(1000);
  ESP.restart();
}

// Connects with saved settings; if something is missing or WiFi doesn't connect,
// raises the AP_NAME access point with a page listing nearby networks plus token/chat fields.
void setupWiFiAndConfig() {
  Preferences p;
  p.begin("cfg", false);
  botToken = p.getString("token", "");
  defaultChatId = p.getString("chat", "");

  WiFiManager wm;
  WiFiManagerParameter pToken("token", "Bot token (from @BotFather)", botToken.c_str(), 80);
  WiFiManagerParameter pChat("chat", "Default chat ID (-100... or @username)", defaultChatId.c_str(), 40);
  wm.addParameter(&pToken);
  wm.addParameter(&pChat);
  wm.setTitle("SMS → Telegram");
  wm.setHostname(HOSTNAME);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);  // no one configured it in 5 min -> reboot and retry saved WiFi

  bool missing = botToken.isEmpty() || defaultChatId.isEmpty();
  if (missing) Serial.printf("[cfg] no token or chat yet, connect to WiFi \"%s\" (password %s)\n", AP_NAME, AP_PASS);
  bool connected = missing ? wm.startConfigPortal(AP_NAME, AP_PASS) : wm.autoConnect(AP_NAME, AP_PASS);

  String token = pToken.getValue(), chat = pChat.getValue();
  token.trim();
  chat.trim();
  if (token != botToken) p.putString("token", botToken = token);
  if (chat != defaultChatId) p.putString("chat", defaultChatId = chat);
  p.end();

  if (!connected || botToken.isEmpty() || defaultChatId.isEmpty()) {
    Serial.println("[cfg] setup not finished, restarting");
    delay(1000);
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
}

// ------------------------------------------------------------
// Web panel (local network, basic auth)
// ------------------------------------------------------------

String jstr(const String& s) { return "\"" + jsonEscape(s) + "\""; }

bool webAuth() {
  if (server.authenticate(WEB_USER, WEB_PASS)) return true;
  server.requestAuthentication(BASIC_AUTH, "SMS-Forwarder");
  return false;
}

void replyJson(bool ok, const String& fields) {
  server.send(200, "application/json; charset=utf-8", String("{\"ok\":") + (ok ? "true" : "false") + fields + "}");
}

String statusJson() {
  int colon = botToken.indexOf(':');
  String maskedToken = colon > 0 ? botToken.substring(0, colon) + ":…" : String("—");
  uint32_t now = millis();

  String j = "{";
  j += "\"uptime\":" + String(now / 1000);
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"resetReason\":" + jstr(resetReasonName(resetReason));
  j += ",\"wifi\":{\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
       ",\"ssid\":" + jstr(WiFi.SSID()) + ",\"rssi\":" + String(WiFi.RSSI()) +
       ",\"ip\":" + jstr(WiFi.localIP().toString()) + ",\"host\":" + jstr(String(HOSTNAME) + ".local") + "}";
  j += ",\"modem\":{\"ready\":" + String(modemReady ? "true" : "false") +
       ",\"sim\":" + String(modemInfo.sim ? "true" : "false") +
       ",\"registered\":" + String(modemInfo.registered ? "true" : "false") +
       ",\"csq\":" + String(modemInfo.csq) + ",\"operator\":" + jstr(modemInfo.op) +
       ",\"simUsed\":" + String(modemInfo.simUsed) + ",\"simTotal\":" + String(modemInfo.simTotal) +
       ",\"clock\":" + jstr(modemInfo.clock) + "}";
  j += ",\"tg\":{\"token\":" + jstr(maskedToken) + ",\"chat\":" + jstr(defaultChatId) + "}";
  j += ",\"stats\":{\"forwarded\":" + String(forwardedCount) + ",\"failed\":" + String(failedCount) + "}";
  j += ",\"lastError\":" + jstr(lastError);
  j += ",\"lastErrorAgo\":" + String(lastErrorAt ? (now - lastErrorAt) / 1000 : 0);
  j += ",\"rules\":[";
  bool first = true;
  for (const Rule& r : rules) {
    if (!first) j += ",";
    first = false;
    j += String("{\"type\":\"") + ruleTypeName(r.type) + "\",\"pattern\":" + jstr(r.pattern) + ",\"chat\":" + jstr(r.chatId) + "}";
  }
  j += "]}";
  return j;
}

String historyJson() {
  String j = "[";
  for (int i = history.size() - 1; i >= 0; i--) {
    const HistoryItem& h = history[i];
    if (j.length() > 1) j += ",";
    j += "{\"id\":" + String(h.id) + ",\"out\":" + String(h.outgoing ? "true" : "false") +
         ",\"peer\":" + jstr(h.peer) + ",\"time\":" + jstr(h.time) + ",\"text\":" + jstr(h.text) +
         ",\"chat\":" + jstr(h.chatId) + ",\"status\":" + jstr(h.status) + "}";
  }
  return j + "]";
}

void webSendSms() {
  String to = server.arg("to"), text = server.arg("text");
  String number;
  for (unsigned i = 0; i < to.length(); i++) {
    if (isdigit((uint8_t)to[i]) || (to[i] == '+' && number.isEmpty())) number += to[i];
  }
  if (digitsOf(number).length() < 3 || text.isEmpty()) return replyJson(false, ",\"error\":\"number and text are required\"");
  if (!modemReady) return replyJson(false, ",\"error\":\"modem not ready\"");

  int parts = 0;
  String err;
  bool ok = sendSms(number, text, parts, err);
  recordHistory(true, number, modemClock(), text, "", ok ? "sent" : "failed");
  if (ok) replyJson(true, ",\"parts\":" + String(parts));
  else replyJson(false, ",\"error\":" + jstr(err));
}

void webUssd() {
  String raw = server.arg("code"), code;
  for (unsigned i = 0; i < raw.length(); i++) if (strchr("0123456789*#+", raw[i])) code += raw[i];
  if (code.isEmpty()) return replyJson(false, ",\"error\":\"empty request\"");
  if (!modemReady) return replyJson(false, ",\"error\":\"modem not ready\"");

  String text;
  bool session = false;
  bool ok = ussdRequest(code, text, session);
  Serial.printf("[ussd] %s -> %s\n", code.c_str(), text.c_str());
  if (ok) replyJson(true, ",\"text\":" + jstr(text) + ",\"session\":" + String(session ? "true" : "false"));
  else replyJson(false, ",\"error\":" + jstr(text));
}

// New token / default chat are checked with a test message before they're saved
void webConfig() {
  String token = server.arg("token"), chat = server.arg("chat");
  token.trim();
  chat.trim();
  if (token.isEmpty() && chat.isEmpty()) return replyJson(false, ",\"error\":\"nothing to change\"");

  String oldToken = botToken, oldChat = defaultChatId;
  if (token.length()) botToken = token;
  if (chat.length()) defaultChatId = chat;
  String err;
  SendResult r = telegramSend(defaultChatId, "✅ Telegram settings updated", &err);
  if (r != SEND_OK) {
    botToken = oldToken;
    defaultChatId = oldChat;
    String why;
    if (r == SEND_BAD_TOKEN) why = "Telegram rejected the bot token";
    else if (r == SEND_REJECTED) why = "Telegram rejected the chat: " + err;
    else why = "could not reach Telegram, try again";
    return replyJson(false, ",\"error\":" + jstr(why));
  }
  Preferences p;
  p.begin("cfg", false);
  p.putString("token", botToken);
  p.putString("chat", defaultChatId);
  p.end();
  replyJson(true, "");
}

String cleanField(String s) {
  s.replace("\t", " ");
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.trim();
  return s;
}

// The new rule's chat gets a test message first, so a wrong id or missing admin rights show up right away
void webRuleAdd() {
  MatchType type;
  String pattern = cleanField(server.arg("pattern")), chat = cleanField(server.arg("chat"));
  if (!parseRuleType(server.arg("type"), type)) return replyJson(false, ",\"error\":\"unknown rule type\"");
  if (pattern.isEmpty() || chat.isEmpty()) return replyJson(false, ",\"error\":\"condition and chat ID are required\"");
  if (rules.size() >= MAX_RULES) return replyJson(false, ",\"error\":\"max " + String(MAX_RULES) + " rules\"");

  String err;
  SendResult r = telegramSend(chat, "✅ This chat is now linked to the rule: " + pattern, &err);
  if (r != SEND_OK) {
    String why;
    if (r == SEND_REJECTED) why = "Telegram rejected the chat: " + err + ". Check the ID and that the bot is an admin there";
    else if (r == SEND_BAD_TOKEN) why = "Telegram rejected the bot token";
    else why = "could not reach Telegram, try again";
    return replyJson(false, ",\"error\":" + jstr(why));
  }
  rules.push_back({type, pattern, chat});
  if (!saveRules()) return replyJson(false, ",\"error\":\"could not save, too many rules\"");
  replyJson(true, "");
}

void webRuleDelete() {
  int i = server.arg("index").toInt();
  if (i < 0 || i >= (int)rules.size()) return replyJson(false, ",\"error\":\"no such rule\"");
  rules.erase(rules.begin() + i);
  saveRules();
  replyJson(true, "");
}

void webRuleMove() {
  int i = server.arg("index").toInt();
  int j = i + (server.arg("dir") == "up" ? -1 : 1);
  if (i < 0 || j < 0 || i >= (int)rules.size() || j >= (int)rules.size()) return replyJson(false, ",\"error\":\"cannot move\"");
  std::swap(rules[i], rules[j]);
  saveRules();
  replyJson(true, "");
}

void setupWeb() {
  server.on("/", HTTP_GET, [] {
    if (webAuth()) server.send(200, "text/html; charset=utf-8", WEB_PAGE);
  });
  server.on("/api/status", HTTP_GET, [] {
    if (webAuth()) server.send(200, "application/json; charset=utf-8", statusJson());
  });
  server.on("/api/history", HTTP_GET, [] {
    if (webAuth()) server.send(200, "application/json; charset=utf-8", historyJson());
  });
  server.on("/api/sms", HTTP_POST, [] {
    if (webAuth()) webSendSms();
  });
  server.on("/api/ussd", HTTP_POST, [] {
    if (webAuth()) webUssd();
  });
  server.on("/api/ussd-cancel", HTTP_POST, [] {
    if (webAuth()) replyJson(sendAT("AT+CUSD=2", nullptr, 5000), "");
  });
  server.on("/api/config", HTTP_POST, [] {
    if (webAuth()) webConfig();
  });
  server.on("/api/rules/add", HTTP_POST, [] {
    if (webAuth()) webRuleAdd();
  });
  server.on("/api/rules/delete", HTTP_POST, [] {
    if (webAuth()) webRuleDelete();
  });
  server.on("/api/rules/move", HTTP_POST, [] {
    if (webAuth()) webRuleMove();
  });
  server.on("/api/test", HTTP_POST, [] {
    if (!webAuth()) return;
    String err;
    SendResult r = telegramSend(defaultChatId, "🔔 Test message from the web panel", &err);
    if (r == SEND_OK) replyJson(true, "");
    else replyJson(false, ",\"error\":" + jstr(err.length() ? err : lastError));
  });
  server.on("/api/reboot", HTTP_POST, [] {
    if (!webAuth()) return;
    replyJson(true, "");
    delay(500);
    ESP.restart();
  });
  server.on("/api/factory-reset", HTTP_POST, [] {
    if (!webAuth()) return;
    replyJson(true, "");
    delay(500);
    factoryReset();
    ESP.restart();
  });
  server.begin();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
  Serial.printf("[web] panel: http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
}

// ------------------------------------------------------------

void setupModem() {
  bool warned = false;
  modemReady = false;
  while (true) {
    modemPowerOn();
    if (modemSetup()) {
      modemReady = true;
      return;
    }
    if (!warned && ensureWiFi(1000)) {
      warned = telegramSend(defaultChatId, "⚠️ Modem failed to start: " + lastError + ". Retrying…") == SEND_OK;
    }
    for (uint32_t t = millis(); millis() - t < 10000;) {  // keep the web panel responsive between attempts
      server.handleClient();
      delay(5);
    }
  }
}

void setup() {
  // LilyGO known issue: on battery power IO12 must go HIGH right after boot, otherwise the board resets
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  digitalWrite(BOARD_POWERON_PIN, HIGH);

  Serial.begin(115200);
  delay(500);
  resetReason = esp_reset_reason();
  Serial.printf("\n=== SMS -> Telegram ===\nreset reason: %s\n", resetReasonName(resetReason));

  if (doubleResetDetected()) factoryReset();
  setupWiFiAndConfig();
  loadRules();
  setupWeb();

  SerialAT.setRxBufferSize(8192);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  setupModem();

  if (!waitForNetwork(60000)) Serial.println("[modem] no network yet, will keep trying in the background");

  // The startup message doubles as a check of the token and chat id entered in the portal
  String hello = "✅ SMS forwarding started: " + modemStatus() + "\nWeb panel: http://" + HOSTNAME + ".local (" +
                 WiFi.localIP().toString() + ")";
  if (resetReason != ESP_RST_POWERON && resetReason != ESP_RST_SW)
    hello += "\n⚠️ Reset reason: " + String(resetReasonName(resetReason));
  SendResult r = telegramSend(defaultChatId, hello);
  if (r == SEND_BAD_TOKEN) forgetAndRestart("token", "Telegram rejected the bot token");
  if (r == SEND_REJECTED) forgetAndRestart("chat", "Telegram rejected the chat ID (or the bot is not an admin there)");
  smsPending = true;  // forward whatever arrived while we were off
}

void loop() {
  server.handleClient();

  String line;
  while (pollLine(line)) {
    if (!handleUrc(line)) Serial.printf("[modem] %s\n", line.c_str());
  }

  uint32_t now = millis();
  if (smsPending || now - lastScan > INBOX_SCAN_INTERVAL_MS) {
    smsPending = false;
    lastScan = now;
    processInbox();
  }

  if (modemReady && now - lastModemInfo > MODEM_INFO_INTERVAL_MS) refreshModemInfo();

  if (modemFailures >= 3) {
    Serial.println("[modem] stopped responding, restarting it");
    modemFailures = 0;
    setupModem();
    smsPending = true;
  }

  // WiFi auto-reconnect sometimes gets stuck; a reboot is the reliable fix
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
  } else if (wifiDownSince == 0) {
    wifiDownSince = now ? now : 1;
  } else if (now - wifiDownSince > WIFI_RESTART_AFTER_MS) {
    Serial.println("[wifi] down for too long, rebooting");
    ESP.restart();
  }

  delay(10);
}
