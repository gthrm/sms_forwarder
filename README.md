# SMS → Telegram forwarder

Firmware for the **LilyGO T-A7670 (E/G/SA) ESP32** board. It forwards every SMS that arrives on the board's SIM card to Telegram. Each message goes to a different channel depending on simple rules: a keyword at the start of the text, a keyword anywhere in the text, or the sender.

- WiFi, bot token and default channel are entered from your phone through a setup hotspot. Nothing is hardcoded.
- Routing rules (e.g. SMS starting with `[work]:` → its own chat) are managed in a password-protected web panel on your local network.
- The web panel also shows modem/WiFi status and recent SMS, and can send SMS and USSD requests (e.g. `*100#`).
- SMS stay on the SIM until Telegram confirms delivery, so nothing is lost while WiFi or Telegram is down.
- Cyrillic and other non-Latin text, long multi-part SMS, and named senders (e.g. `Sberbank`) are decoded correctly.
- If a rule's channel rejects a message (wrong ID, bot not an admin), the message goes to the default channel with a warning.

## What you need

- LilyGO T-A7670 ESP32 board (A7670E / A7670G / A7670SA)
- A SIM card with the PIN disabled (or set `SIM_PIN` in `config.h`). Mobile data is not needed.
- The LTE antenna, screwed into the connector marked **SIM**
- An 18650 battery in the holder is recommended. The modem draws current peaks of up to 2 A, and USB alone can cause resets.
- A **USB-A → USB-C** cable. LilyGO notes that a Mac's USB-C port can't power the board directly.

## 1. Arduino IDE setup

1. **USB driver (CH9102).** On macOS, install the [WCH CH34x driver](https://www.wch-ic.com/downloads/CH34XSER_MAC_ZIP.html), then allow it in **System Settings → General → Login Items & Extensions → Driver Extensions**. Replug the board; it should show up as `/dev/cu.wchusbserial…`.
2. **ESP32 boards.** In **Settings → Additional boards manager URLs**, add:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
   Then in **Boards Manager**, install **esp32 by Espressif Systems**.
3. **Library.** In **Manage Libraries**, install **WiFiManager** by tzapu.
4. **Tools settings:**

   | Setting          | Value                             |
   | ---------------- | --------------------------------- |
   | Board            | ESP32 Dev Module                  |
   | Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS)  |
   | PSRAM            | Enable                            |
   | Flash Size       | 4MB                               |
   | Upload Speed     | 921600 (use 115200 if it fails)   |
   | Port             | `/dev/cu.wchusbserial…`           |

5. Remove the SD card if one is inserted (it shares GPIO2 and blocks uploading), open `sms_forwarder/sms_forwarder.ino` and click **Upload**.

## 2. Telegram setup

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy its token.
2. Add the bot to each channel as an **admin** with permission to post messages.
3. Find each channel's ID. A public channel can use `@channelname`. For a private channel, forward any post from it to a bot such as [@userinfobot](https://t.me/userinfobot), or open `https://api.telegram.org/bot<TOKEN>/getUpdates` after posting in the channel. Private channel IDs start with `-100`.

## 3. First start

1. Power the board. Because there are no settings yet, it opens a WiFi hotspot called **SMS-Forwarder** (password `12345678`).
2. Connect to it from your phone. A setup page opens automatically; if it doesn't, go to `http://192.168.4.1`.
3. Tap **Configure WiFi**. Pick your network from the list of networks nearby, enter its password, the **bot token** and the **default channel ID**, then save.
4. The board connects and posts **"✅ Пересылка SMS запущена"** ("SMS forwarding started") with the signal level, operator and the web panel address to the default channel.

If something is wrong, the board opens the hotspot again so you can fix it:

| Problem                                       | What happens                                      |
| --------------------------------------------- | ------------------------------------------------- |
| Wrong WiFi password / network not reachable   | Hotspot opens again                               |
| Telegram rejects the bot token                | Token is erased, hotspot opens again              |
| Telegram rejects the channel ID / bot not admin | Channel ID is erased, hotspot opens again       |
| Nobody uses the hotspot for 5 minutes         | Board reboots and retries the saved WiFi          |

## Web panel

Once the board is connected, open **http://sms-forwarder.local** on any device in the same network. If `.local` names don't work on your device (some Android phones), use the IP address from the startup message in Telegram. The browser asks for a login: `admin` / `12345678` by default (`WEB_USER` / `WEB_PASS` in `config.h`, change the password).

The panel shows:

- **Modem:** SIM, network registration, operator, signal, SMS waiting on the SIM, network time
- **WiFi and board:** network, signal, address, uptime, last reboot reason, forwarded / failed attempts, last error
- **Messages:** the last 50 incoming and outgoing SMS with the chat each one went to and its status. They're kept in memory only and cleared on reboot.

And lets you:

- manage **forwarding rules** (below)
- **send an SMS** from the board's SIM, with Cyrillic and long messages supported
- run **USSD** requests (`*100#`), including multi-step operator menus
- change the **bot token** or **default channel** (checked with a test message before saving)
- send a test message, reboot, or factory-reset the board

## Forwarding rules

Rules are added in the web panel and stored in the board's flash. They are checked top to bottom, and the first match wins. Use ↑ / ↓ to reorder. Anything that matches no rule goes to the default channel.

| Condition       | Matches when…                                                                  |
| --------------- | ------------------------------------------------------------------------------ |
| Начинается с (starts with) | the text starts with this tag or word. With `[work]:`, the SMS `[work]: привет как дела!` goes to that rule's chat as `привет как дела!` (the tag is removed; set `STRIP_PREFIX false` in `config.h` to keep it). A word like `WORK` matches "work: call Bob" but not "WORKSHOP". |
| Содержит (contains) | the text contains this word or phrase anywhere                             |
| Отправитель (sender) | the sender is this name (`Sberbank`) or phone number. Numbers are compared by their last 9 digits, so `+7999…` matches `8999…`. |

Matching ignores case for Latin, Cyrillic and Latin-with-diacritics letters (č, š, ž…).

When you add a rule, the board posts a test message to its chat. If Telegram rejects it (wrong ID, or the bot isn't an admin there), the rule isn't saved and the panel shows why. If a saved rule's chat later rejects a message, the message goes to the default channel with a warning.

## Other options in `config.h`

| Option         | Default           | Meaning                                                              |
| -------------- | ----------------- | -------------------------------------------------------------------- |
| `AP_NAME`      | `"SMS-Forwarder"` | Name of the setup hotspot                                            |
| `AP_PASS`      | `"12345678"`      | Hotspot password, at least 8 characters. Empty `""` makes an open network. |
| `HOSTNAME`     | `"sms-forwarder"` | Web panel address: `http://<HOSTNAME>.local`                         |
| `WEB_USER` / `WEB_PASS` | `"admin"` / `"12345678"` | Web panel login                                     |
| `SIM_PIN`      | `""`              | SIM PIN, if the SIM has one                                          |
| `SHOW_HEADER`  | `true`            | Prefix each forwarded message with sender and time                   |
| `STRIP_PREFIX` | `true`            | Remove the "starts with" tag from the forwarded text                 |

> **Change the passwords.** Anyone who knows the hotspot password could enter their own bot token while the hotspot is up and redirect your SMS. Anyone in your network who knows the web password can read your SMS and send SMS from your SIM.

## Factory reset

Press **RST**, then press it again within 3 seconds, or use **Сброс до заводских** in the web panel. This erases the WiFi credentials, bot token, default channel and all rules, and the board opens the setup hotspot.

A single RST press is just a reboot. Reboots caused by a power dip, crash or watchdog never trigger a reset. The startup message in Telegram shows the reboot reason when it wasn't a normal power-on.

## How it works

- The modem is put in PDU mode and reports new SMS to the ESP32. The inbox is also checked every 60 seconds, and at boot, to catch anything missed.
- Each SMS is decoded, routed and sent to Telegram. It's deleted from the SIM only after Telegram accepts it. If WiFi or Telegram is down, it's retried on the next check.
- Parts of a long SMS are collected and sent as one message. If a part never arrives, the rest is sent after 3 minutes with `[…]` in place of the missing part.
- The modem is restarted automatically if it stops responding. The board reboots if WiFi stays down for 15 minutes.

## Troubleshooting

Open **Serial Monitor** at **115200** baud. Log lines are prefixed with `[modem]`, `[wifi]`, `[cfg]`, `[sms]`, `[ussd]`, `[web]` and `[err]`, and the messages are in Russian. The first lines after boot show the reboot reason.

| Symptom                                   | Check                                                                 |
| ----------------------------------------- | --------------------------------------------------------------------- |
| `SIM не готова` ("SIM not ready")         | SIM inserted correctly, PIN disabled or `SIM_PIN` set                 |
| `модем не отвечает` ("modem not responding") | Battery inserted / better USB power                                |
| `сеть не найдена`, `НЕ зарегистрирован в сети` ("not registered on the network") | Antenna on the **SIM** connector; the SIM works in a phone; the modem variant supports your operator's bands |
| No port in Arduino IDE                    | Data-capable cable, CH9102 driver allowed                             |
| Upload fails                              | Remove the SD card, lower Upload Speed to 115200, or hold **IO0** while plugging in |
| Web panel doesn't open                    | Same WiFi network as the board; try the IP from the startup message instead of `sms-forwarder.local` |
| Settings were wiped unexpectedly          | Check the reboot reason at the top of the Serial log or in the startup message |

## Files

```
sms_forwarder/
  sms_forwarder.ino   main sketch: modem, setup portal, rules, routing, Telegram, web panel API
  web_page.h          web panel page (HTML/JS)
  pdu.h               SMS PDU encoder/decoder (GSM-7 / UCS-2, multi-part SMS)
  config.h            hotspot and web panel login, SIM PIN, options
```
