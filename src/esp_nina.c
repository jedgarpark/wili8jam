#include "esp_nina.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdio.h>

// --- Fruit Jam hardware pin assignments ---
#define NINA_SPI      spi1
#define NINA_MISO_PIN 28   // SPI1 RX
#define NINA_SCK_PIN  30   // SPI1 SCK
#define NINA_MOSI_PIN 31   // SPI1 TX
#define NINA_CS_PIN   46   // manual CS (active LOW)
#define NINA_BUSY_PIN 3    // BUSY/ACK: LOW = ESP32 ready to transact
#define NINA_IRQ_PIN  23   // IRQ (not used by this driver)
// GPIO22 = shared codec/ESP32 RESET — DO NOT TOUCH (ESP32 auto-boots)

// --- NINA-fw SPI protocol constants ---
#define START_CMD    0xE0u
#define END_CMD      0xEEu
#define REPLY_FLAG   0x80u

// Command bytes
#define CMD_GET_FW_VERSION    0x37u
#define CMD_SET_PASSPHRASE    0x11u
#define CMD_GET_CONN_STATUS   0x20u
#define CMD_GET_SOCKET        0x3Fu
#define CMD_START_CLIENT_TCP  0x2Du
#define CMD_GET_CLIENT_STATE  0x2Fu
#define CMD_SEND_DATA_TCP     0x44u
#define CMD_AVAIL_DATA_TCP    0x43u
#define CMD_GET_DATABUF_TCP   0x45u
#define CMD_STOP_CLIENT       0x2Eu

// Protocol type for TLS (bearssl)
#define PROT_TLS 2

// Timeouts
#define BUSY_TIMEOUT_MS    5000u
#define CONNECT_TIMEOUT_MS 15000u

// -----------------------------------------------------------------------
// Low-level SPI / GPIO helpers
// -----------------------------------------------------------------------

static inline void cs_low(void)  { gpio_put(NINA_CS_PIN, 0); }
static inline void cs_high(void) { gpio_put(NINA_CS_PIN, 1); }

// Wait for BUSY pin to reach desired level. Returns true on success.
static bool wait_busy(int desired_level, uint32_t timeout_ms) {
    uint32_t start = time_us_32();
    while (gpio_get(NINA_BUSY_PIN) != desired_level) {
        if (time_us_32() - start > timeout_ms * 1000u)
            return false;
        tight_loop_contents();
    }
    return true;
}

static void spi_write_byte(uint8_t b) {
    spi_write_blocking(NINA_SPI, &b, 1);
}

static uint8_t spi_read_byte(void) {
    uint8_t b;
    spi_read_blocking(NINA_SPI, 0xFF, &b, 1);
    return b;
}

// -----------------------------------------------------------------------
// Packet builder (commands sent to ESP32)
// -----------------------------------------------------------------------

// Build a command packet into buf (caller allocates).
// Returns packet length. Adds END_CMD and 4-byte alignment padding.
// Uses 8-bit param length prefix for all params (standard commands).
// Usage: pkt_begin → one or more pkt_param8 → pkt_finish → send buf[0..ret-1]

typedef struct {
    uint8_t *buf;
    int capacity;
    int len;
} pkt_t;

static void pkt_begin(pkt_t *p, uint8_t cmd, uint8_t n_params) {
    p->len = 0;
    p->buf[p->len++] = START_CMD;
    p->buf[p->len++] = cmd & ~REPLY_FLAG;
    p->buf[p->len++] = n_params;
}

static void pkt_param8(pkt_t *p, const uint8_t *data, uint8_t len) {
    p->buf[p->len++] = len;
    memcpy(&p->buf[p->len], data, len);
    p->len += len;
}

// Add a param with 16-bit big-endian length prefix (SEND_DATA_TCP / GET_DATABUF_TCP)
static void pkt_param16(pkt_t *p, const uint8_t *data, uint16_t len) {
    p->buf[p->len++] = (uint8_t)(len >> 8);
    p->buf[p->len++] = (uint8_t)(len & 0xFF);
    if (data) memcpy(&p->buf[p->len], data, len);
    p->len += len;
}

static void pkt_finish(pkt_t *p) {
    p->buf[p->len++] = END_CMD;
    while (p->len & 3) p->buf[p->len++] = 0xFF; // 4-byte alignment
}

// -----------------------------------------------------------------------
// Core transaction
// -----------------------------------------------------------------------
// Send a command packet, then read the response first parameter into resp_buf.
// param_len_16: if true, response uses 16-bit length prefixes (GET_DATABUF_TCP).
// Returns bytes read into resp_buf, or -1 on protocol error.
static int nina_transact(
    const uint8_t *send_buf, int send_len,
    uint8_t cmd,
    uint8_t *resp_buf, int resp_max,
    bool param_len_16
) {
    // --- Send phase ---
    if (!wait_busy(0, BUSY_TIMEOUT_MS)) { printf("[nina] busy timeout (send)\n"); return -1; }
    cs_low();
    spi_write_blocking(NINA_SPI, send_buf, (size_t)send_len);
    cs_high();

    // Wait for ESP32 to raise BUSY HIGH (signals it received the command).
    // Short window: some commands are so fast BUSY never goes HIGH — if it doesn't
    // rise within 2 ms we assume it already did and proceed to wait for LOW.
    {
        uint32_t t0 = time_us_32();
        while (gpio_get(NINA_BUSY_PIN) == 0) {
            if (time_us_32() - t0 > 2000u) break;
            tight_loop_contents();
        }
    }

    // --- Wait for response ready (BUSY goes LOW) ---
    if (!wait_busy(0, BUSY_TIMEOUT_MS)) { printf("[nina] busy timeout (resp)\n"); return -1; }

    // --- Read phase ---
    cs_low();

    // Drain until START_CMD.
    // IMPORTANT: abort immediately on 0x00 — that means nina-fw has no response
    // prepared (e.g. GET_DATABUF_TCP when the connection has just closed).
    // Clocking further 0xFF bytes past a 0x00 feeds garbage into nina-fw's SPI
    // frame parser and corrupts ALL subsequent transactions (symptoms: every cmd
    // returns 0xEF, nina_get_socket fails). One byte of 0xFF is harmless; 256 is not.
    uint8_t b = 0;
    uint8_t diag[8];
    int diag_n = 0;
    bool found_start = false;
    for (int i = 0; i < 32; i++) {
        b = spi_read_byte();
        if (diag_n < (int)sizeof(diag)) diag[diag_n++] = b;
        if (b == START_CMD) { found_start = true; break; }
        if (b == 0x00) break; // nina has no response — stop now, don't clock more garbage in
    }
    if (!found_start) {
        cs_high();
        printf("[nina] no START_CMD for cmd %02X, got:", cmd);
        for (int i = 0; i < diag_n; i++) printf(" %02X", diag[i]);
        printf("\n");
        return -1;
    }

    // Validate response command
    uint8_t resp_cmd = spi_read_byte();
    if (resp_cmd != (cmd | REPLY_FLAG)) {
        cs_high();
        printf("[nina] cmd mismatch: expected %02X got %02X\n", cmd | REPLY_FLAG, resp_cmd);
        return -1;
    }

    // Number of response params
    uint8_t n_params = spi_read_byte();

    int bytes_read = 0;
    for (int p = 0; p < (int)n_params; p++) {
        uint16_t plen;
        if (param_len_16) {
            uint8_t hi = spi_read_byte();
            uint8_t lo = spi_read_byte();
            plen = ((uint16_t)hi << 8) | lo;
        } else {
            plen = spi_read_byte();
        }
        for (uint16_t i = 0; i < plen; i++) {
            uint8_t rb = spi_read_byte();
            // Only store first param bytes into resp_buf
            if (p == 0 && (int)i < resp_max)
                resp_buf[i] = rb;
        }
        if (p == 0)
            bytes_read = (int)(plen < (uint16_t)resp_max ? plen : (uint16_t)resp_max);
    }

    spi_read_byte(); // END_CMD (ignore)
    cs_high();
    return bytes_read;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

void nina_init(void) {
    // SPI1 at 8 MHz, mode 0 (CPOL=0, CPHA=0), MSB-first
    spi_init(NINA_SPI, 8000000);
    spi_set_format(NINA_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(NINA_MISO_PIN, GPIO_FUNC_SPI);
    gpio_set_function(NINA_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(NINA_MOSI_PIN, GPIO_FUNC_SPI);

    // CS: output, active-low, default high
    gpio_init(NINA_CS_PIN);
    gpio_set_dir(NINA_CS_PIN, GPIO_OUT);
    gpio_put(NINA_CS_PIN, 1);

    // BUSY: input (pull-up so it reads HIGH = busy when floating)
    gpio_init(NINA_BUSY_PIN);
    gpio_set_dir(NINA_BUSY_PIN, GPIO_IN);
    gpio_pull_up(NINA_BUSY_PIN);

    // IRQ: input (not used, just configure for safety)
    gpio_init(NINA_IRQ_PIN);
    gpio_set_dir(NINA_IRQ_PIN, GPIO_IN);
    gpio_pull_up(NINA_IRQ_PIN);

    // GPIO22 = PERIPH_RESET: shared between ESP32-C6 and TLV320 audio codec.
    // We MUST assert RESET here so nina-fw boots cleanly.
    // nina_init() is called BEFORE audio_init() in main.cpp so the codec
    // comes out of this same reset and gets reconfigured by audio_init().
    #define NINA_RESET_PIN 22
    gpio_init(NINA_RESET_PIN);
    gpio_set_dir(NINA_RESET_PIN, GPIO_OUT);
    gpio_put(NINA_RESET_PIN, 0);  // assert RESET LOW
    sleep_ms(10);                 // hold for 10 ms
    gpio_put(NINA_RESET_PIN, 1);  // release RESET HIGH

    printf("[nina] SPI1 init at 8MHz (MISO=%d SCK=%d MOSI=%d CS=%d BUSY=%d)\n",
        NINA_MISO_PIN, NINA_SCK_PIN, NINA_MOSI_PIN, NINA_CS_PIN, NINA_BUSY_PIN);
    printf("[nina] reset ESP32-C6, waiting for nina-fw boot...\n");

    // ESP32-C6 nina-fw takes ~750 ms to boot after reset (per Adafruit spec)
    sleep_ms(750);
    printf("[nina] BUSY pin: %d (want 0)\n", gpio_get(NINA_BUSY_PIN));

    // Verify ESP32 is alive and running nina-fw
    char ver[16] = {0};
    bool alive = false;
    for (int retry = 0; retry < 3 && !alive; retry++) {
        if (nina_get_fw_version(ver, sizeof(ver)) == 0 && ver[0] != '\0') {
            alive = true;
        } else {
            printf("[nina] fw version attempt %d failed, retrying...\n", retry + 1);
            sleep_ms(500);
        }
    }
    if (alive) {
        printf("[nina] ESP32-C6 nina-fw version: %s\n", ver);
    } else {
        printf("[nina] WARNING: ESP32-C6 not responding to nina-fw commands\n");
        printf("[nina] Check: is nina-fw flashed? Are SPI pins correct?\n");
    }
}

int nina_get_fw_version(char *buf, size_t len) {
    uint8_t pkt_buf[16];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    pkt_begin(&p, CMD_GET_FW_VERSION, 0);
    pkt_finish(&p);

    uint8_t resp[16] = {0};
    int n = nina_transact(p.buf, p.len, CMD_GET_FW_VERSION, resp, (int)sizeof(resp)-1, false);
    if (n < 0) return -1;
    resp[n] = '\0';
    strncpy(buf, (char*)resp, len - 1);
    buf[len - 1] = '\0';
    return 0;
}

int nina_connect_wpa(const char *ssid, const char *passphrase) {
    // Retry loop: nina-fw WiFi sometimes needs a re-send of SET_PASSPHRASE
    // (e.g. after previous failed HTTP sessions confused the WiFi stack).
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            printf("[nina] retrying SET_PASSPHRASE (attempt %d)...\n", attempt + 1);
            sleep_ms(1000);
        }

        uint8_t pkt_buf[128];
        pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
        pkt_begin(&p, CMD_SET_PASSPHRASE, 2);
        pkt_param8(&p, (const uint8_t*)ssid,       (uint8_t)strlen(ssid));
        pkt_param8(&p, (const uint8_t*)passphrase, (uint8_t)strlen(passphrase));
        pkt_finish(&p);

        uint8_t resp[4];
        printf("[nina] sending SET_PASSPHRASE for ssid='%s'\n", ssid);
        if (nina_transact(p.buf, p.len, CMD_SET_PASSPHRASE, resp, sizeof(resp), false) < 0) {
            printf("[nina] SET_PASSPHRASE SPI transaction failed\n");
            continue;
        }
        printf("[nina] SET_PASSPHRASE sent, polling...\n");

        // Poll for up to 10 s per attempt; print only status changes
        int last_status = -1;
        uint32_t start = time_us_32();
        while (time_us_32() - start < 10000000u) {
            int status = nina_get_conn_status();
            if (status != last_status) {
                printf("[nina] conn status: %d\n", status);
                last_status = status;
            }
            if (status == NINA_STATUS_CONNECTED) {
                printf("[nina] WiFi connected!\n");
                return 0;
            }
            if (status == NINA_STATUS_CONNECT_FAILED) {
                printf("[nina] connect failed (wrong credentials?)\n");
                break; // retry
            }
            sleep_ms(500);
        }
    }
    printf("[nina] connect failed after 3 attempts\n");
    return -1;
}

int nina_get_conn_status(void) {
    uint8_t pkt_buf[16];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    pkt_begin(&p, CMD_GET_CONN_STATUS, 0);
    pkt_finish(&p);

    uint8_t resp[4];
    if (nina_transact(p.buf, p.len, CMD_GET_CONN_STATUS, resp, sizeof(resp), false) < 0)
        return -1;
    return (int)resp[0];
}

uint8_t nina_get_socket(void) {
    uint8_t pkt_buf[16];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    pkt_begin(&p, CMD_GET_SOCKET, 0);
    pkt_finish(&p);

    uint8_t resp[4];
    if (nina_transact(p.buf, p.len, CMD_GET_SOCKET, resp, sizeof(resp), false) < 0)
        return NINA_NO_SOCKET;
    return resp[0];
}

int nina_tcp_open(uint8_t sock, const char *host, uint16_t port, bool tls) {
    uint8_t pkt_buf[300];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    // 5-param form: hostname, ip(4 bytes=0 for DNS), port(2), sock(1), prot(1)
    // nina-fw uses the hostname for TLS SNI; ip=0 means "resolve from hostname"
    pkt_begin(&p, CMD_START_CLIENT_TCP, 5);

    uint8_t host_len = (uint8_t)strlen(host);
    pkt_param8(&p, (const uint8_t*)host, host_len);

    // IP address: 4 bytes, 0.0.0.0 → nina-fw will DNS-resolve from hostname
    uint8_t ip_zero[4] = {0, 0, 0, 0};
    pkt_param8(&p, ip_zero, 4);

    // Port as 2-byte big-endian
    uint8_t port_bytes[2] = { (uint8_t)(port >> 8), (uint8_t)(port & 0xFF) };
    pkt_param8(&p, port_bytes, 2);

    pkt_param8(&p, &sock, 1);

    uint8_t prot = tls ? PROT_TLS : 0;
    pkt_param8(&p, &prot, 1);

    pkt_finish(&p);

    uint8_t resp[4];
    if (nina_transact(p.buf, p.len, CMD_START_CLIENT_TCP, resp, sizeof(resp), false) < 0)
        return -1;
    return 0;
}

int nina_tcp_state(uint8_t sock) {
    uint8_t pkt_buf[16];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    pkt_begin(&p, CMD_GET_CLIENT_STATE, 1);
    pkt_param8(&p, &sock, 1);
    pkt_finish(&p);

    uint8_t resp[4];
    if (nina_transact(p.buf, p.len, CMD_GET_CLIENT_STATE, resp, sizeof(resp), false) < 0)
        return -1;
    return (int)resp[0];
}

int nina_tcp_send(uint8_t sock, const uint8_t *data, uint16_t len) {
    // SEND_DATA_TCP uses 16-bit length prefixes for ALL params
    // Packet: START | 0x44 | 2 | [0x00,0x01,sock] | [len_hi,len_lo,data...] | END | pad
    // Build header separately, then send data in-line during the CS transaction
    uint8_t hdr[16];
    int hi = 0;
    hdr[hi++] = START_CMD;
    hdr[hi++] = CMD_SEND_DATA_TCP & ~REPLY_FLAG;
    hdr[hi++] = 2; // 2 params
    // param 1: socket (16-bit length = 1)
    hdr[hi++] = 0x00; hdr[hi++] = 0x01;
    hdr[hi++] = sock;
    // param 2: data (16-bit length)
    hdr[hi++] = (uint8_t)(len >> 8);
    hdr[hi++] = (uint8_t)(len & 0xFF);
    // (data follows)

    // Trailer: END + padding to 4-byte alignment
    int total_before_pad = hi + len + 1; // +1 for END_CMD
    uint8_t trailer[5];
    int ti = 0;
    trailer[ti++] = END_CMD;
    while ((total_before_pad + ti) & 3) trailer[ti++] = 0xFF;

    if (!wait_busy(0, BUSY_TIMEOUT_MS)) return -1;
    cs_low();
    spi_write_blocking(NINA_SPI, hdr, hi);
    spi_write_blocking(NINA_SPI, data, len);
    spi_write_blocking(NINA_SPI, trailer, ti);
    cs_high();

    // Wait for BUSY HIGH (processing started), then LOW (response ready)
    { uint32_t t0 = time_us_32(); while (gpio_get(NINA_BUSY_PIN) == 0 && (time_us_32() - t0 < 2000u)) tight_loop_contents(); }
    if (!wait_busy(0, BUSY_TIMEOUT_MS)) return -1;

    // Read response — uses 16-bit length prefix
    cs_low();
    uint8_t b;
    for (int i = 0; i < 32; i++) {
        b = spi_read_byte();
        if (b == START_CMD) break;
    }
    if (b != START_CMD) { cs_high(); return -1; }
    uint8_t resp_cmd = spi_read_byte();
    if (resp_cmd != (CMD_SEND_DATA_TCP | REPLY_FLAG)) { cs_high(); return -1; }
    uint8_t n_params = spi_read_byte();
    for (int p = 0; p < n_params; p++) {
        uint8_t hi_b = spi_read_byte();
        uint8_t lo_b = spi_read_byte();
        uint16_t plen = ((uint16_t)hi_b << 8) | lo_b;
        for (uint16_t i = 0; i < plen; i++) spi_read_byte(); // discard
    }
    spi_read_byte(); // END_CMD
    cs_high();
    return 0;
}

int nina_tcp_avail(uint8_t sock) {
    uint8_t pkt_buf[16];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    pkt_begin(&p, CMD_AVAIL_DATA_TCP, 1);
    pkt_param8(&p, &sock, 1);
    pkt_finish(&p);

    // Response: 8-bit length prefix = 2, data is 2-byte big-endian count
    uint8_t resp[4] = {0};
    if (nina_transact(p.buf, p.len, CMD_AVAIL_DATA_TCP, resp, sizeof(resp), false) < 0)
        return -1;
    return (int)(((uint16_t)resp[0] << 8) | resp[1]);
}

int nina_tcp_recv(uint8_t sock, uint8_t *buf, uint16_t len) {
    // GET_DATABUF_TCP uses 16-bit length prefixes for ALL params
    uint8_t pkt_buf[20];
    int pi = 0;
    pkt_buf[pi++] = START_CMD;
    pkt_buf[pi++] = CMD_GET_DATABUF_TCP & ~REPLY_FLAG;
    pkt_buf[pi++] = 2; // 2 params
    // param 1: socket (16-bit length = 1)
    pkt_buf[pi++] = 0x00; pkt_buf[pi++] = 0x01;
    pkt_buf[pi++] = sock;
    // param 2: requested length (16-bit length = 2)
    pkt_buf[pi++] = 0x00; pkt_buf[pi++] = 0x02;
    pkt_buf[pi++] = (uint8_t)(len >> 8);
    pkt_buf[pi++] = (uint8_t)(len & 0xFF);
    pkt_buf[pi++] = END_CMD;
    while (pi & 3) pkt_buf[pi++] = 0xFF;

    // nina_transact will stream response data directly into buf
    int got = nina_transact(pkt_buf, pi, CMD_GET_DATABUF_TCP, buf, (int)len, true);
    return got;
}

int nina_tcp_close(uint8_t sock) {
    uint8_t pkt_buf[16];
    pkt_t p = { pkt_buf, (int)sizeof(pkt_buf), 0 };
    pkt_begin(&p, CMD_STOP_CLIENT, 1);
    pkt_param8(&p, &sock, 1);
    pkt_finish(&p);

    uint8_t resp[4];
    if (nina_transact(p.buf, p.len, CMD_STOP_CLIENT, resp, sizeof(resp), false) < 0)
        return -1;
    return 0;
}
