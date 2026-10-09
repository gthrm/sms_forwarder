#pragma once
// Minimal SMS PDU codec (3GPP TS 23.040 / 23.038).
// Decodes SMS-DELIVER (GSM-7 / UCS-2 / 8-bit text, alphanumeric senders, concatenated SMS headers)
// and encodes SMS-SUBMIT as UCS-2, split into concatenated parts when needed.
#include <Arduino.h>
#include <algorithm>
#include <vector>

struct SmsPdu {
  String sender;
  String timestamp;  // "dd.mm.yy hh:mm", network local time
  String text;
  bool isConcat = false;
  uint16_t ref = 0;
  uint8_t total = 1;
  uint8_t seq = 1;
};

enum DecodeResult { DECODE_OK, DECODE_NOT_DELIVER, DECODE_MALFORMED };

namespace pdu {
struct SubmitPdu {
  String hex;    // full PDU for AT+CMGS, including the empty SMSC field
  int tpduLen;   // length for AT+CMGS=<n>, without the SMSC field
};
}

namespace pdu {

// GSM 03.38 default alphabet -> Unicode
static const uint16_t GSM7_BASIC[128] = {
  0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, 0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
  0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, 0x03A3, 0x0398, 0x039E, 0x00A0, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
  0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
  0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
  0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
  0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
  0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
  0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

static inline uint16_t gsm7Extended(uint8_t c) {
  switch (c) {
    case 0x0A: return '\n';
    case 0x14: return '^';
    case 0x28: return '{';
    case 0x29: return '}';
    case 0x2F: return '\\';
    case 0x3C: return '[';
    case 0x3D: return '~';
    case 0x3E: return ']';
    case 0x40: return '|';
    case 0x65: return 0x20AC;  // €
    default:   return GSM7_BASIC[c & 0x7F];
  }
}

static inline void appendUtf8(String& out, uint32_t cp) {
  if (cp == 0) return;
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

static inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static inline bool hexToBytes(const String& hex, std::vector<uint8_t>& out) {
  if (hex.length() % 2) return false;
  out.reserve(hex.length() / 2);
  for (unsigned i = 0; i < hex.length(); i += 2) {
    int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((hi << 4) | lo);
  }
  return true;
}

static inline void unpackSeptets(const uint8_t* data, size_t len, size_t septets, std::vector<uint8_t>& out) {
  for (size_t i = 0; i < septets; i++) {
    size_t bit = i * 7, byte = bit / 8, shift = bit % 8;
    if (byte >= len) break;
    uint16_t v = data[byte] >> shift;
    if (shift > 1 && byte + 1 < len) v |= data[byte + 1] << (8 - shift);
    out.push_back(v & 0x7F);
  }
}

static inline String gsm7ToUtf8(const std::vector<uint8_t>& s, size_t start = 0) {
  String out;
  for (size_t i = start; i < s.size(); i++) {
    if (s[i] == 0x1B && i + 1 < s.size()) appendUtf8(out, gsm7Extended(s[++i]));
    else appendUtf8(out, GSM7_BASIC[s[i]]);
  }
  return out;
}

static inline String ucs2ToUtf8(const uint8_t* d, size_t len) {
  String out;
  for (size_t i = 0; i + 1 < len; i += 2) {
    uint32_t u = (d[i] << 8) | d[i + 1];
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
      uint32_t lo = (d[i + 2] << 8) | d[i + 3];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    appendUtf8(out, u);
  }
  return out;
}

static inline uint8_t bcdSwapped(uint8_t b) { return (b & 0x0F) * 10 + (b >> 4); }

static inline DecodeResult decodeDeliver(const String& hex, SmsPdu& out) {
  std::vector<uint8_t> b;
  if (!hexToBytes(hex, b) || b.empty()) return DECODE_MALFORMED;
  const size_t n = b.size();
  size_t p = 1 + b[0];  // skip SMSC
  auto need = [&](size_t k) { return p + k <= n; };

  if (!need(1)) return DECODE_MALFORMED;
  uint8_t fo = b[p++];
  if ((fo & 0x03) != 0x00) return DECODE_NOT_DELIVER;
  bool udhi = fo & 0x40;

  // Originating address
  if (!need(2)) return DECODE_MALFORMED;
  uint8_t oaDigits = b[p++], toa = b[p++];
  size_t oaBytes = (oaDigits + 1) / 2;
  if (!need(oaBytes)) return DECODE_MALFORMED;
  if ((toa & 0x70) == 0x50) {
    std::vector<uint8_t> sept;
    unpackSeptets(&b[p], oaBytes, oaDigits * 4 / 7, sept);
    out.sender = gsm7ToUtf8(sept);
  } else {
    if ((toa & 0x70) == 0x10) out.sender = "+";
    static const char DIGITS[] = "0123456789*#abc";
    for (size_t i = 0; i < oaDigits; i++) {
      uint8_t nib = (i % 2) ? (b[p + i / 2] >> 4) : (b[p + i / 2] & 0x0F);
      if (nib < 15) out.sender += DIGITS[nib];
    }
  }
  p += oaBytes;

  // PID, DCS, SCTS, UDL
  if (!need(1 + 1 + 7 + 1)) return DECODE_MALFORMED;
  p++;  // PID
  uint8_t dcs = b[p++];
  char ts[20];
  snprintf(ts, sizeof ts, "%02u.%02u.%02u %02u:%02u",
           bcdSwapped(b[p + 2]), bcdSwapped(b[p + 1]), bcdSwapped(b[p]), bcdSwapped(b[p + 3]), bcdSwapped(b[p + 4]));
  out.timestamp = ts;
  p += 7;
  uint8_t udl = b[p++];
  const uint8_t* ud = b.data() + p;
  size_t udBytes = n - p;

  // 0 = GSM-7, 1 = 8-bit, 2 = UCS-2
  uint8_t group = dcs >> 4, alphabet = 0;
  if (group <= 0x7) alphabet = (dcs >> 2) & 0x03;
  else if (group == 0xE) alphabet = 2;
  else if (group == 0xF) alphabet = (dcs & 0x04) ? 1 : 0;
  if (alphabet == 3) alphabet = 0;

  // User data header (concatenation info)
  size_t hdrBytes = 0;
  if (udhi && udBytes > 0) {
    hdrBytes = ud[0] + 1;
    if (hdrBytes > udBytes) return DECODE_MALFORMED;
    for (size_t i = 1; i + 1 < hdrBytes;) {
      uint8_t iei = ud[i], iel = ud[i + 1];
      const uint8_t* v = ud + i + 2;
      if (i + 2 + iel > hdrBytes) break;
      if (iei == 0x00 && iel == 3) {
        out.isConcat = true; out.ref = v[0]; out.total = v[1]; out.seq = v[2];
      } else if (iei == 0x08 && iel == 4) {
        out.isConcat = true; out.ref = (v[0] << 8) | v[1]; out.total = v[2]; out.seq = v[3];
      }
      i += 2 + iel;
    }
  }

  if (alphabet == 0) {
    std::vector<uint8_t> sept;
    unpackSeptets(ud, udBytes, udl, sept);
    out.text = gsm7ToUtf8(sept, (hdrBytes * 8 + 6) / 7);
  } else {
    size_t len = udl < udBytes ? udl : udBytes;
    if (hdrBytes > len) return DECODE_MALFORMED;
    if (alphabet == 2) {
      out.text = ucs2ToUtf8(ud + hdrBytes, len - hdrBytes);
    } else {
      for (size_t i = hdrBytes; i < len; i++) appendUtf8(out.text, ud[i]);  // treat as Latin-1
    }
  }
  return DECODE_OK;
}

// ---- Encoding (SMS-SUBMIT) ----

static inline std::vector<uint16_t> utf8ToUtf16(const String& s) {
  std::vector<uint16_t> out;
  for (unsigned i = 0; i < s.length();) {
    uint8_t c = s[i++];
    uint32_t cp;
    int extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else { cp = c & 0x07; extra = 3; }
    for (int k = 0; k < extra && i < s.length(); k++) cp = (cp << 6) | (s[i++] & 0x3F);
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(0xD800 | (cp >> 10));
      out.push_back(0xDC00 | (cp & 0x3FF));
    } else {
      out.push_back(cp);
    }
  }
  return out;
}

static const size_t UCS2_SINGLE_CHARS = 70;  // one SMS
static const size_t UCS2_PART_CHARS   = 67;  // per part of a long SMS (6 bytes go to the header)

static inline size_t partsNeeded(size_t units) {
  return units <= UCS2_SINGLE_CHARS ? 1 : (units + UCS2_PART_CHARS - 1) / UCS2_PART_CHARS;
}

// number: international ("+<country code><number>") or local digits
static inline std::vector<SubmitPdu> encodeSubmit(const String& number, const std::vector<uint16_t>& units, uint8_t ref) {
  std::vector<std::pair<size_t, size_t>> chunks;  // (start, count)
  if (units.size() <= UCS2_SINGLE_CHARS) {
    chunks.push_back({0, units.size()});
  } else {
    for (size_t i = 0; i < units.size();) {
      size_t n = std::min(UCS2_PART_CHARS, units.size() - i);
      if (i + n < units.size() && units[i + n - 1] >= 0xD800 && units[i + n - 1] <= 0xDBFF) n--;  // don't split a surrogate pair
      chunks.push_back({i, n});
      i += n;
    }
  }

  String digits;
  for (unsigned i = 0; i < number.length(); i++) if (isdigit((uint8_t)number[i])) digits += number[i];
  bool intl = number.startsWith("+");
  bool multi = chunks.size() > 1;

  std::vector<SubmitPdu> out;
  for (size_t k = 0; k < chunks.size(); k++) {
    std::vector<uint8_t> t;
    t.push_back(multi ? 0x51 : 0x11);  // SMS-SUBMIT, relative validity period, UDHI for long SMS
    t.push_back(0x00);                 // message reference (set by modem)
    t.push_back(digits.length());
    t.push_back(intl ? 0x91 : 0x81);
    for (unsigned i = 0; i < digits.length(); i += 2) {
      uint8_t lo = digits[i] - '0';
      uint8_t hi = i + 1 < digits.length() ? digits[i + 1] - '0' : 0x0F;
      t.push_back((hi << 4) | lo);
    }
    t.push_back(0x00);  // PID
    t.push_back(0x08);  // DCS: UCS-2
    t.push_back(0xAA);  // validity: 4 days
    size_t start = chunks[k].first, n = chunks[k].second;
    t.push_back((multi ? 6 : 0) + n * 2);
    if (multi) {
      uint8_t udh[] = {0x05, 0x00, 0x03, ref, (uint8_t)chunks.size(), (uint8_t)(k + 1)};
      t.insert(t.end(), udh, udh + sizeof udh);
    }
    for (size_t i = start; i < start + n; i++) {
      t.push_back(units[i] >> 8);
      t.push_back(units[i] & 0xFF);
    }

    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String hex = "00";  // use the SMSC stored on the SIM
    hex.reserve(2 + t.size() * 2);
    for (uint8_t b : t) {
      hex += HEX_DIGITS[b >> 4];
      hex += HEX_DIGITS[b & 0x0F];
    }
    out.push_back({hex, (int)t.size()});
  }
  return out;
}

}  // namespace pdu
