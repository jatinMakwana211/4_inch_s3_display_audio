# ESP32-S3 five-second voice recorder

On every boot the serial monitor counts down three seconds. When `Speak now`
appears, speak into the INMP441 for five seconds. After 100 ms of microphone
startup settling, the firmware captures 80,000 samples at 16 kHz, saves
`/spiffs/recording.wav`, closes it, and reopens it for speaker playback.
Reset the board to record again.

## Wiring

| INMP441 | ESP32-S3 |
| --- | --- |
| VDD | 3.3 V |
| GND | GND |
| SCK / BCLK | GPIO19 |
| WS / LRCLK | GPIO47 |
| SD | GPIO20 |
| L/R | GND (left channel) |

**Move the microphone wires from GPIO12/13/14 before flashing this version.**
Those pins are RGB LCD data signals. GPIO19/20 are also native USB signals;
use the board's UART programming/monitor connection with this microphone wiring,
and do not connect an active native USB data cable to GPIO19/20.

MAX98357A speaker output uses BCLK GPIO46, WS GPIO45 and DIN GPIO42 with standard Philips I2S timing. Connect amplifier VIN to its supported 3.3 V or 5 V supply and share GND with the ESP32-S3. MAX98357B requires left-justified timing instead.

The microphone uses Philips I2S with two 32-bit slots (64 clocks per frame)
and captures the left slot, following the
[INMP441 datasheet](https://www.invensense.com/wp-content/uploads/2015/02/INMP441.pdf).
Signed 24-bit samples become 16-bit mono with clipping protection. Adjust
`MIC_GAIN` in `main/voice_recorder.c` (default 4) if needed.

## Storage and playback

The WAV is 160,044 bytes, including its 44-byte header. Capture uses 160 KB
PSRAM (ordinary heap fallback), then writes SPIFFS to avoid flash stalls during
recording. RX overflow or read errors abort the recording. A temporary file
is fully written and closed before replacing the previous WAV. Replacement
is not power-loss atomic. If replacement fails, the new WAV remains at
`/spiffs/recording.tmp`; the next recording removes that temporary file.
Keep space for both recordings plus filesystem overhead.

Playback reads the saved WAV and duplicates mono to both speaker channels.
It respects NVS volume (default 16/21) and audio-enabled settings. When audio
is disabled, recording still saves but playback is skipped. This path owns
its I2S channels; do not call `init_audio()` concurrently. The existing MP3
API remains available; startup no longer plays `welcome.mp3`.

## Welcome display

After recording, saving and playback, the welcome screen and touch/Wi-Fi UI
start. Audio errors are logged but do not prevent display startup. Saved Wi-Fi
reconnects after audio completes.

GPIO47 serves microphone WS during boot capture and LCD control SDA afterward.
LCD CS (GPIO39) stays high during capture. The recorder deletes its I2S channel
and holds microphone BCLK (GPIO19) low before LCD setup begins. After LCD setup,
Agent Mode can reuse GPIO47 for microphone WS while LCD CS remains high and
control clock GPIO48 remains low. RGB refresh uses separate pins and sends no LCD control commands.
GPIO40/41 remain touch SDA/SCL.

## Agent Mode

Connect Wi-Fi, enable Agent Mode in Settings, and tap Back to open the Agent
screen. After clock synchronization, the device POSTs the browser's userData,
websiteName, language and fileName fields to
`https://api.test.ai-ivrs.bonrix.in/new/api/session/init`. This HTTPS endpoint
returns `websocketUrl`; the firmware connects to that secure WebSocket with
certificate verification. It retries failed sessions automatically and closes
the session when you leave the Agent screen.

The WebSocket event flow and audio rates follow the saved browser reference.
On connection it sends
`trigger.greeting` and starts listening without a wake phrase. Microphone audio
is 24 kHz signed little-endian PCM16 mono, base64 encoded in `input_audio_buffer.append`
(80 ms per packet: 1920 samples, 3840 PCM bytes). The microphone captures at 24 kHz.
The server detects speech and generates responses. `response.audio.delta`
plays through the MAX98357A at 24 kHz; user and assistant transcripts update
the You and Bonrix cards. Firmware pauses microphone uploads during replies
and for a 300 ms echo tail. Wait until 'Listening - speak to Bonrix' before
speaking again; this half duplex client does not support interrupting replies.
Replies buffer until `response.audio.done` or `response.done` before playback,
avoiding repeated underruns when server delivery is slower than playback. This
adds initial response latency. Exceptionally long replies begin playing at the
1.5 MiB high-water mark (about 33 seconds at 24 kHz) to keep reception moving.
The attempted `/jatin/api/init` and `/new/api/init` routes return HTTP 404.
Use an 8 kHz configuration only with a working endpoint that supports that format.

Edit `main/include/agent_config.h` to change identity, language (English, Hindi,
Gujarati), context filename, or microphone gain. L/R grounded selects the left
slot. Speaker output respects the saved audio-enabled and volume settings.
The separate five-second boot recorder and recorded-voice playback still run
before the display and Agent Mode start. Do not initialize the MP3 player while
Agent Mode owns the I2S channels.

Build and flash from an ESP-IDF terminal:
`idf.py -B build_voice -p COM6 build flash monitor`.

## Touch Wi-Fi setup

1. After recording/playback, tap the Wi-Fi icon at the welcome screen's top right.
2. If credentials were saved previously, the connecting screen tries that network
   for up to **15 seconds**, showing live association/DHCP stages and a countdown.
   Success opens network details. On timeout it automatically opens configuration
   and scans again. Without saved credentials, configuration opens immediately.
   The configuration screen scans visible **2.4 GHz** networks. Use the page
   arrows to see all results; duplicate access points with the same SSID/security
   are grouped. Tap the circular refresh icon to scan again. The selected network
   is highlighted, and long names are shortened with an ellipsis. ESP32-S3 cannot
   see 5 GHz-only networks.
3. Tap a network. For secured networks, enter an 8-63 character password using
   the keyboard: Shift toggles case, #+= switches to symbols, Space and Delete
   edit, and the eye icon toggles masking. Tap Done, then Connect. Open networks do not
   require a password. Connect shows the same progress screen. Wrong passwords or
   unavailable networks return to the form after the 15-second attempt.
4. After DHCP assigns an address, the connected screen displays actual SSID,
   signal strength, IPv4 address, subnet, gateway, primary/backup DNS and elapsed
   connection duration. Refresh updates signal/DNS, Configure Wi-Fi returns to
   scanning, Disconnect ends the connection, and Back returns to the welcome screen.

Connection duration is shown as elapsed time. After DHCP succeeds, the SSID, password and security
mode are committed together in the NVS `wifi_setup` namespace, key `station`.
Failed connection attempts do not replace the last working credentials. Reusing
unchanged credentials avoids rewriting flash. A storage failure is reported on
the connected screen. Passwords are never logged or copied into UI snapshots.
Saved data is loaded after recording/playback, and the device makes one automatic
15-second saved-Wi-Fi connection attempt on every boot. The welcome screen remains
visible; tapping its Wi-Fi logo shows current progress or connected details. If
the boot attempt failed, tapping the logo retries the saved network. Missing or
invalid saved data opens configuration. Disconnect keeps credentials for later use.
The 15-second deadline includes initialization, retries and DHCP; retries do not
restart it. At timeout the form appears while radio cleanup finishes, then a new
scan starts. A late IP event cannot turn a timed-out attempt into a saved success.
Back does not cancel an in-progress scan/connection, and preserves an existing
connection. The screen remains responsive while the worker scans/connects.
Controls show a pressed tint immediately and activate on release. Sliding more
than 18 pixels, sliding out of a control, or changing screens cancels that press.
Release is debounced for 16 ms, preventing repeated actions from a held finger
or a brief touch dropout. The UI polls touch every 12 ms between redraws.
Connect is enabled once a supported network and a valid-length password (when
required) are selected. The dropdown focuses the list; it does not start an
unnecessary rescan. A spinner shows ongoing network work.
Security support: open and WPA/WPA2/WPA3 Personal. Enterprise, WEP and other
unsupported security modes are listed but cannot be joined through this form.
Hidden networks/manual SSID entry and captive portal login are not implemented.
Non-ASCII SSID bytes are preserved for connecting but displayed as `?` by the
compact ASCII font. No passwords or network names from the reference artwork
are used as actual connection credentials.

Touch orientation can be adjusted with `TOUCH_SWAP_XY`, `TOUCH_MIRROR_X` and
`TOUCH_MIRROR_Y` in `main/wifi_ui.c`. These default to the unrotated board.
Regenerate the font with `powershell -File tools/generate_ui_font.ps1`.
Regenerate the curved background with `powershell -File tools/generate_wifi_background.ps1`.
The background is embedded as RGB565 so the display does not recalculate its
curves and gradients during touch events. Cards and controls are drawn in C.
Preview PNG files use sample network data only; firmware uses live scan results.
Run `python tools/test_wifi_ui.py` to test the production hit testing/keyboard/
tap tracking and generate previews. It uses the Windows TinyCC compiler at
`build/ui_host/tcc/tcc.exe`, or the path specified by the `TCC` environment
variable; TinyCC is only a desktop test dependency, not a firmware dependency.
Run `python tools/test_wifi_credentials.py` to check the production NVS record
validation, save/load round trips, unchanged-write avoidance and error handling
against a memory-backed NVS test double. These tests do not verify physical flash.

Board acceptance checks: verify all four touch corners, tap the welcome icon,
scan/page/rescan, connect to an open and WPA2 network, try a wrong password,
retry successfully, verify DHCP details, disconnect, and return to welcome.
After a successful connection, power-cycle: verify automatic use of the saved
network without tapping the screen. Turn that access point off, open the Wi-Fi
progress screen and confirm the form appears at the end of the 15-second attempt.
Try a wrong replacement password, reboot, and confirm the last
working network is still saved. Test unavailable DHCP as well as authentication
failure, and check that Back stays responsive during an attempt.
Confirm the audio recording/playback still works with the new microphone wiring.

## Build and flash

From an ESP-IDF v5.4 environment:

```sh
idf.py -B build_voice build
idf.py -B build_voice -p COM6 flash monitor
```

Full flashing writes the bundled SPIFFS image and replaces device recordings.
After SPIFFS is initialized, use `app-flash` instead of `flash` to update only
the application and retain files. Mount failure does not automatically format
SPIFFS.


