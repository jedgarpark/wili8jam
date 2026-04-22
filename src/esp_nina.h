#ifndef ESP_NINA_H
#define ESP_NINA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// NINA-fw WiFi connection status codes (nina_get_conn_status return values)
#define NINA_STATUS_IDLE            0
#define NINA_STATUS_NO_SSID_AVAIL   1
#define NINA_STATUS_SCAN_COMPLETED  2
#define NINA_STATUS_CONNECTED       3
#define NINA_STATUS_CONNECT_FAILED  4
#define NINA_STATUS_CONNECTION_LOST 5
#define NINA_STATUS_DISCONNECTED    6

// NINA-fw TCP socket state codes (nina_tcp_state return values)
#define NINA_TCP_CLOSED      0
#define NINA_TCP_LISTEN      1
#define NINA_TCP_SYN_SENT    2
#define NINA_TCP_SYN_RCVD    3
#define NINA_TCP_ESTABLISHED 4
#define NINA_TCP_FIN_WAIT_1  5
#define NINA_TCP_FIN_WAIT_2  6
#define NINA_TCP_CLOSE_WAIT  7
#define NINA_TCP_CLOSING     8
#define NINA_TCP_LAST_ACK    9
#define NINA_TCP_TIME_WAIT   10

#define NINA_NO_SOCKET 255

// Initialize NINA-fw SPI driver. Call once after audio_init().
// Does NOT touch GPIO22 (shared ESP32/codec reset — auto-boots from pull-up).
void nina_init(void);

// Retrieve NINA firmware version string. Returns 0 on success, -1 on error.
int nina_get_fw_version(char *buf, size_t len);

// Connect to a WPA/WPA2 network. Blocks until connected or ~15s timeout.
// Returns 0 on success (NINA_STATUS_CONNECTED), -1 on error/timeout.
int nina_connect_wpa(const char *ssid, const char *passphrase);

// Get current WiFi connection status (NINA_STATUS_* code). Returns -1 on error.
int nina_get_conn_status(void);

// Allocate a free socket. Returns NINA_NO_SOCKET on error.
uint8_t nina_get_socket(void);

// Open a TCP (or TLS when tls=true) connection to host:port.
// sock must come from nina_get_socket(). Returns 0 on success, -1 on error.
int nina_tcp_open(uint8_t sock, const char *host, uint16_t port, bool tls);

// Get current socket state (NINA_TCP_* code). Returns -1 on error.
int nina_tcp_state(uint8_t sock);

// Send data over TCP socket. Returns 0 on success, -1 on error.
// Data must be <= 512 bytes per call.
int nina_tcp_send(uint8_t sock, const uint8_t *data, uint16_t len);

// Get number of bytes available to read from socket.
// Returns byte count (>= 0) or -1 on error.
int nina_tcp_avail(uint8_t sock);

// Receive up to len bytes from TCP socket into buf.
// Returns bytes actually read (>= 0) or -1 on error.
int nina_tcp_recv(uint8_t sock, uint8_t *buf, uint16_t len);

// Close TCP socket. Returns 0 on success, -1 on error.
int nina_tcp_close(uint8_t sock);

#ifdef __cplusplus
}
#endif

#endif // ESP_NINA_H
