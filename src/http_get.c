#include "http_get.h"
#include "esp_nina.h"
#include "fatfs/ff.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>

// Receive chunk size — fits comfortably in RP2350 SRAM
#define RECV_BUF_SIZE 1440

// Receive buffer — static to keep off the call stack
static uint8_t recv_buf[RECV_BUF_SIZE];

// Parse "https://hostname/path" → host and path.
// Returns 0 on success, -1 if URL is malformed.
static int parse_url(const char *url, char *host, int host_max, const char **path_out) {
    if (strncmp(url, "https://", 8) != 0) return -1;
    const char *h = url + 8;
    const char *slash = strchr(h, '/');
    if (!slash) return -1;
    int hlen = (int)(slash - h);
    if (hlen <= 0 || hlen >= host_max) return -1;
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    *path_out = slash; // includes leading '/'
    return 0;
}

int http_get_to_file(const char *url, const char *dest_path,
                     void (*progress_cb)(int bytes_written, int content_length))
{
    // Give nina-fw time to finish releasing any previous TLS socket.
    // The first call has nothing to wait for; subsequent calls need ~500 ms.
    static bool first_call = true;
    if (first_call) { first_call = false; }
    else            { sleep_ms(500); }

    char host[128];
    const char *path;
    if (parse_url(url, host, sizeof(host), &path) != 0) {
        printf("[http] bad url: %s\n", url);
        return -1;
    }

    // --- Open socket and connect ---
    uint8_t sock = nina_get_socket();
    if (sock == NINA_NO_SOCKET) {
        printf("[http] no socket available\n");
        return -1;
    }

    if (nina_tcp_open(sock, host, 443, true) != 0) {
        printf("[http] tcp_open failed\n");
        return -1;
    }

    // Wait for TCP connection established (~10 s timeout)
    bool connected = false;
    uint32_t t0 = time_us_32();
    while (time_us_32() - t0 < 10000000u) {
        int state = nina_tcp_state(sock);
        if (state == NINA_TCP_ESTABLISHED) { connected = true; break; }
        if (state == NINA_TCP_CLOSED || state < 0) break;
        sleep_ms(200);
    }
    if (!connected) {
        printf("[http] connect timeout for %s\n", host);
        nina_tcp_close(sock);
        return -1;
    }
    printf("[http] connected to %s\n", host);

    // --- Send HTTP GET request ---
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: wili8jam/1.0\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (nina_tcp_send(sock, (const uint8_t*)req, (uint16_t)req_len) != 0) {
        printf("[http] send failed\n");
        nina_tcp_close(sock);
        return -1;
    }

    // --- Open output file ---
    static FIL fil;
    if (f_open(&fil, dest_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[http] cannot open dest: %s\n", dest_path);
        nina_tcp_close(sock);
        return -1;
    }

    // --- Read response, strip headers, write body ---
    // State machine for detecting \r\n\r\n header terminator
    int hdr_sep = 0; // 0=idle 1=\r 2=\r\n 3=\r\n\r 4=\r\n\r\n(body)
    bool headers_done = false;
    int content_length = -1;
    int bytes_written = 0;
    bool error = false;

    // Small accumulator for header line parsing (looking for Content-Length / status)
    char hl_buf[256];
    int  hl_pos = 0;
    bool status_checked = false;

    int idle_count = 0;
    const int IDLE_LIMIT = 300; // 30 s

    while (true) {
        // Poll for data directly with GET_DATABUF_TCP.
        // CMD_AVAIL_DATA_TCP returns ERROR_CMD on TLS sockets in nina-fw 3.1.0, so
        // we skip it. GET_DATABUF_TCP returns 0 bytes when nothing is ready (clean
        // "try again"), or actual data when it's available.
        int got = nina_tcp_recv(sock, recv_buf, RECV_BUF_SIZE);

        if (got <= 0) {
            // 0 = nothing ready yet; <0 = recv error (possibly socket closed)
            int state = nina_tcp_state(sock);
            if (state == NINA_TCP_CLOSED    ||
                state == NINA_TCP_FIN_WAIT_2 ||
                state == NINA_TCP_TIME_WAIT  ||
                state == NINA_TCP_CLOSE_WAIT ||
                state == NINA_TCP_LAST_ACK   ||
                state < 0)
            {
                break; // clean EOF
            }
            if (got < 0) {
                // recv error but connection still alive — unusual, limit retries
                if (++idle_count > 10) { error = true; break; }
            } else {
                if (++idle_count > IDLE_LIMIT) { error = true; break; }
            }
            sleep_ms(100);
            continue;
        }
        idle_count = 0;

        if (!headers_done) {
            // Scan for end-of-headers (\r\n\r\n) and parse header lines
            for (int i = 0; i < got; i++) {
                uint8_t b = recv_buf[i];

                // Detect \r\n\r\n
                if      (b == '\r' && (hdr_sep == 0 || hdr_sep == 2)) hdr_sep++;
                else if (b == '\n' && (hdr_sep == 1 || hdr_sep == 3)) hdr_sep++;
                else if (b == '\r') hdr_sep = 1;
                else    hdr_sep = 0;

                // Accumulate header line for parsing
                if (b != '\r') {
                    if (hl_pos < (int)sizeof(hl_buf) - 1)
                        hl_buf[hl_pos++] = (char)b;
                }
                if (b == '\n') {
                    hl_buf[hl_pos] = '\0';

                    // First line: HTTP status
                    if (!status_checked) {
                        status_checked = true;
                        // "HTTP/1.1 200 OK" — extract status code
                        const char *sp = strchr(hl_buf, ' ');
                        if (sp) {
                            int status_code = atoi(sp + 1);
                            if (status_code != 200) {
                                printf("[http] HTTP %d\n", status_code);
                                f_close(&fil);
                                f_unlink(dest_path);
                                nina_tcp_close(sock);
                                return -1;
                            }
                        }
                    }

                    // Content-Length header
                    if (strncasecmp(hl_buf, "content-length:", 15) == 0)
                        content_length = atoi(hl_buf + 15);

                    hl_pos = 0;
                }

                if (hdr_sep == 4) {
                    // End of headers — write rest of this chunk as body
                    headers_done = true;
                    int body_bytes = got - (i + 1);
                    if (body_bytes > 0) {
                        UINT bw;
                        f_write(&fil, recv_buf + i + 1, (UINT)body_bytes, &bw);
                        bytes_written += (int)bw;
                        if (progress_cb) progress_cb(bytes_written, content_length);
                    }
                    break; // remaining recv_buf bytes already handled
                }
            }
        } else {
            // Full chunk is body data
            UINT bw;
            f_write(&fil, recv_buf, (UINT)got, &bw);
            bytes_written += (int)bw;
            if (progress_cb) progress_cb(bytes_written, content_length);
        }

        // Stop early if we received exactly content_length bytes
        if (content_length > 0 && bytes_written >= content_length)
            break;
    }

    f_close(&fil);
    nina_tcp_close(sock);

    if (error || !headers_done || bytes_written == 0) {
        f_unlink(dest_path);
        printf("[http] failed after %d bytes\n", bytes_written);
        return -1;
    }

    printf("[http] done: %d bytes -> %s\n", bytes_written, dest_path);
    return 0;
}
