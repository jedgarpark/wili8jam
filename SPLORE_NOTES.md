# Splore Implementation Notes

WiFi cart browser for wili8jam. Connects to the Lexaloffle BBS via the Fruit Jam's
ESP32-C6 WiFi co-processor, downloads a community cart index, and lets the user
browse, download, and run PICO-8 carts from the SD card.

## Files Added

- `src/esp_nina.c` / `src/esp_nina.h` — NINA-fw SPI driver
- `src/http_get.c` / `src/http_get.h` — HTTPS GET to file
- `src/p8_splore.c` / `src/p8_splore.h` — Splore browser UI
- `CMakeLists.txt` — added all three .c files to the executable

## WiFi Credentials

Create `/wifi.cfg` in the SD card root:
```
YourNetworkSSID
YourPassword
```
Two lines, no quotes, no labels.

## How It Works

1. `splore()` is registered as a Lua global in `main.cpp` via `p8_splore_register(L)`
2. User calls `splore` at the REPL
3. Reads `/wifi.cfg`, connects to WiFi via ESP32-C6
4. Downloads (or uses cached) `/splore_index.txt` from the gameflix community index
5. Displays a scrollable list of 128 carts
6. O button: download + run cart; X button: download only; Start: refresh index; ESC: exit

### Index Source

```
https://raw.githubusercontent.com/WizzardSK/gameflix/main/fantasy/pico8.txt
```

Tab-separated format: `post_id\ttitle\tfilename.p8.png`
First line is a header (`---\tReleases`) — skipped.

### Cart Download URL

```
https://www.lexaloffle.com/bbs/cposts/{first_two_chars_of_filename}/{filename}.p8.png
```

The subdirectory prefix is the **first two characters** of the filename (e.g., `plorks-2`
→ `pl/`). Carts are saved to `/carts/{filename}.p8.png` on SD.

---

## NINA-fw SPI Protocol

### Hardware

- SPI1: MISO=GPIO28, SCK=GPIO30, MOSI=GPIO31, CS=GPIO46
- BUSY pin: GPIO3 (LOW = ESP32 ready)
- GPIO22 = PERIPH_RESET shared between ESP32-C6 and TLV320DAC3100 codec

### Critical Details

**GPIO22 reset order**: `nina_init()` pulses GPIO22 LOW (10ms) then HIGH to boot the
ESP32-C6. This also resets the audio codec, so `nina_init()` must be called **before**
`audio_init()` in `main.cpp`. The codec gets reconfigured by `audio_init()` after.

**REPLY_FLAG = 0x80** — bit 7, not bit 6. Response command byte = cmd | 0x80.

**BUSY pin timing** — after CS HIGH (send phase):
1. Wait for BUSY to go HIGH (up to 2ms) — signals ESP32 received the command
2. Wait for BUSY to go LOW — signals response is ready

A fixed 200µs sleep is not enough. Some commands are so fast BUSY never rises
before it falls again, so the 2ms wait is a "best-effort" that proceeds if it times out.

**START_CLIENT_TCP requires 5 params**: hostname + ip_zero[4] + port(2) + sock(1) + prot(1).
Sending only 4 params causes the connection to fail silently.

**CMD_AVAIL_DATA_TCP (0x43) does not work on TLS sockets** in nina-fw 3.1.0 — it
returns ERROR_CMD. The http_get driver polls `GET_DATABUF_TCP` (0x45) directly
instead. GET_DATABUF_TCP returns a valid 0-length response when no data is available.

**Drain loop must abort on 0x00**: When nina-fw has no response prepared (e.g.
GET_DATABUF_TCP after the TCP connection has closed), it returns 0x00 bytes on MISO.
If the drain loop continues and clocks 256× 0xFF on MOSI, it corrupts nina-fw's SPI
frame parser — every subsequent command returns 0xEF (ERROR_CMD), including
GET_SOCKET. Fix: abort the drain loop immediately on the first 0x00 byte.

**WiFi reconnect retries**: After failed HTTP sessions, the nina-fw WiFi stack can get
confused and not progress past status IDLE. `nina_connect_wpa()` retries 3 times
with a 10-second timeout per attempt.

---

## HTTPS / HTTP Client

`http_get_to_file(url, dest_path, progress_cb)` in `http_get.c`:

- Parses `https://host/path` URL
- Opens TCP+TLS socket (port 443), waits up to 10s for ESTABLISHED
- Sends HTTP/1.1 GET with Host/User-Agent/Connection:close
- Polls `nina_tcp_recv` directly — no AVAIL_DATA_TCP (broken on TLS)
- Strips HTTP headers, writes body to FatFS file
- Treats `bytes_written == 0` as failure (deletes partial file)
- 500ms sleep between calls to let nina-fw release the previous TLS socket

EOF detection: when nina-fw returns all-zeros for GET_DATABUF_TCP, the state
check (`nina_tcp_state`) returns CLOSED/TIME_WAIT/etc. and the loop exits cleanly.

---

## FatFS Notes

`FF_USE_STRFUNC=0` in `fatfs/ffconf.h` disables `f_gets`. All line reading uses a
custom `fat_readline()` helper that does byte-by-byte `f_read`.

---

## PXA Decompressor

PICO-8 PNG carts (`.p8.png`) embed cart data steganographically in pixel LSBs
(2 bits per channel, ARGB order). The code section starts at cart offset 0x4300
and is compressed in one of three formats:

- **PXA** (modern, PICO-8 0.2.2+): header `\x00pxa`
- **:c:** (old): header `\x3a\x63\x3a\x00`
- **Plaintext**: null-terminated

### PXA Format — Correct Back-Reference Decoding

The initial implementation had completely wrong back-reference decoding. The correct
algorithm (verified against Lexaloffle's `pxa_compress_snippets.c`, zepto8, and
shrinko8):

**Offset** — 1-2 prefix bits choose the field width:
```
bit=0          → read 15 bits
bit=1, bit=0   → read 10 bits  (raw_val==0 is raw-literal-block sentinel)
bit=1, bit=1   → read  5 bits
offset = read_bits(nbits) + 1
```

**Raw literal block sentinel**: `nbits==10 && raw_val==0` → read 8-bit bytes
until 0x00 terminator (uncompressed passthrough block).

**Length** — chained 3-bit groups:
```
len = 3   (PXA_MIN_BLOCK_LEN)
do:
    n = read_bits(3)
    len += n
while n == 7
```
Minimum length = 3. Groups of 7 extend the count arbitrarily.

**Literal** (header bit = 1) — MTF with unary index encoding. This was already
correct in the original implementation:
```
unary = count leading 1-bits
index = read_bits(4 + unary) + ((2^unary - 1) << 4)
ch = mtf[index]; move-to-front
```

The bit reader is LSB-first within each byte throughout.

---

## What Was Wrong and Why

| Bug | Symptom | Fix |
|-----|---------|-----|
| REPLY_FLAG = 0x40 | `[nina] cmd mismatch: expected 77 got B7` | Changed to 0x80 |
| GPIO22 not pulsed LOW | nina-fw not responding at boot | Reset pulse in nina_init() before audio_init() |
| BUSY timing (200µs sleep) | `[nina] no START_CMD` for tcp_open | Wait for BUSY HIGH then LOW |
| START_CLIENT_TCP 4 params | tcp_open fails silently | Added ip_zero[4] param |
| AVAIL_DATA_TCP on TLS | Returns ERROR_CMD | Removed; poll GET_DATABUF_TCP directly |
| Index URL wrong branch/path | HTTP 404 | `main` branch, `fantasy/pico8.txt` |
| Index parser wrong field order | Cart titles show as `---` | Fixed: `post_id\ttitle\tfilename` |
| Cart URL 1-char prefix | HTTP 404 | Changed to 2-char prefix (`pl/plorks-2`) |
| 0-byte index file on SD | Empty file after failed download | Treat bytes_written==0 as failure; check fsize==0 in cache |
| Drain loop clocking 256 bytes | nina-fw stuck after download | Abort on first 0x00 byte |
| PXA back-reference wrong | `unexpected symbol near '-'` at line 19 | Corrected offset/length encoding (see above) |
| Editor auto-load reads binary PNG | Garbled editor after loading .p8.png via Splore | `p8_editor_enter()`: skip auto-load if cart path ends in `.p8.png` |
| Editor buffer empty for .p8.png carts | Editor shows `[new]` with no code | `p8_cart_run()`: call `p8_editor_load_buf(lua_code, lua_len)` before freeing decompressed code |
| Compound assignment RHS includes `;` | `')' expected near ';'` (e.g. `a+=1; b=2` → `a = a + (1;)`) | `find_rhs_end()` in `p8_preprocess.c`: stop at `;` at depth 0 |
