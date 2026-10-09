#pragma once

// ============================================================
//  SETTINGS
// ============================================================

// --- WiFi, bot token, default chat ---
// All of these are entered from your phone. While something is missing (or WiFi doesn't connect),
// the board opens a hotspot with this name and password. Connect to it and a page opens
// with the list of nearby networks and fields for the WiFi password, bot token and chat ID.
// If Telegram rejects the token or the chat, the hotspot opens again.
// Factory reset: press RST twice (the second press within 3 s).
#define AP_NAME "SMS-Forwarder"
#define AP_PASS "12345678"  // at least 8 characters

// --- Web panel on the local network ---
// Open http://sms-forwarder.local (or the board's IP from the startup message).
// The browser asks for a login and password; change the password.
#define HOSTNAME "sms-forwarder"
#define WEB_USER "admin"
#define WEB_PASS "12345678"

// SIM PIN. Leave empty "" if the PIN is disabled.
#define SIM_PIN ""

// Prefix each forwarded message with the sender and time
#define SHOW_HEADER true

// For "starts with" rules: remove the tag from the forwarded text.
// "[work]: hello" -> the chat gets "hello"
#define STRIP_PREFIX true

// Forwarding rules (tag / word / sender -> chat) are managed in the web panel
// and stored in the board's flash. A factory reset erases them too.
