#include "p8_splore.h"
#include "esp_nina.h"
#include "http_get.h"
#include "gfx.h"
#include "input.h"
#include "p8_cart.h"
#include "p8_console.h"
#include "fatfs/ff.h"
#include "pico/stdlib.h"
#include "lauxlib.h"
#include <string.h>
#include <stdio.h>

// --- Cart index ---
// WizzardSK/gameflix pico8.txt: tab-separated filename\ttitle\tauthor\tpost_id
#define INDEX_URL  "https://raw.githubusercontent.com/WizzardSK/gameflix/main/fantasy/pico8.txt"
#define INDEX_PATH "/splore_index.txt"

// Download URL pattern: https://www.lexaloffle.com/bbs/cposts/{first_two_chars}/{filename}.p8.png
#define LEXALOFFLE_HOST "www.lexaloffle.com"
#define CART_URL_FMT    "https://www.lexaloffle.com/bbs/cposts/%c%c/%s.p8.png"

// Local cart save path
#define CART_SAVE_DIR  "/carts"
#define CART_SAVE_FMT  "/carts/%s.p8.png"

// --- Static cart list ---
#define SPLORE_MAX_ENTRIES 128

static char splore_files[SPLORE_MAX_ENTRIES][32];  // cart filename (no extension)
static char splore_titles[SPLORE_MAX_ENTRIES][24]; // display title
static int  splore_count = 0;

// --- Screen helpers ---
// 128x128 display: gfx_print uses 8px-wide chars → 16 chars/line, 6px line height → 18 rows

static void splore_draw_status(const char *line1, const char *line2) {
    gfx_cls(1);
    gfx_print("wili8jam splore", 8, 2, 7);
    gfx_rectfill(0, 9, 127, 9, 6);
    if (line1) gfx_print(line1, 0, 30, 7);
    if (line2) gfx_print(line2, 0, 40, 6);
    gfx_flip();
}

static void splore_draw_progress(const char *label, int bytes, int total) {
    gfx_cls(1);
    gfx_print("wili8jam splore", 8, 2, 7);
    gfx_rectfill(0, 9, 127, 9, 6);
    gfx_print(label, 0, 30, 7);

    // Progress bar (100px wide, 4px tall at y=50)
    gfx_rectfill(0, 50, 99, 53, 5);
    if (total > 0 && bytes > 0) {
        int filled = (bytes * 100) / total;
        if (filled > 100) filled = 100;
        gfx_rectfill(0, 50, filled - 1, 53, 11);
    } else {
        // Unknown size: just fill proportionally up to 50%
        static int spinner = 0;
        spinner = (spinner + 2) % 100;
        gfx_rectfill(0, 50, spinner, 53, 11);
    }

    // Byte count
    char buf[32];
    if (total > 0)
        snprintf(buf, sizeof(buf), "%d/%d", bytes, total);
    else
        snprintf(buf, sizeof(buf), "%d bytes", bytes);
    gfx_print(buf, 0, 60, 6);

    gfx_flip();
}

// --- Line reader (f_gets is disabled in this project's ffconf.h) ---

// Read one line from an open FIL into buf (up to buf_max-1 chars + NUL).
// Returns chars written (0 = EOF), strips trailing \r\n.
static int fat_readline(FIL *fil, char *buf, int buf_max) {
    int len = 0;
    while (len < buf_max - 1) {
        char c;
        UINT br;
        if (f_read(fil, &c, 1, &br) != FR_OK || br == 0) break;
        if (c == '\n') break;
        if (c != '\r') buf[len++] = c;
    }
    buf[len] = '\0';
    return len;
}

// --- WiFi credentials ---

static int read_wifi_config(char *ssid, int ssid_max, char *password, int pass_max) {
    static FIL fil;
    if (f_open(&fil, "/wifi.cfg", FA_READ) != FR_OK) return -1;

    char line[128];

    // Line 1: SSID
    if (fat_readline(&fil, line, sizeof(line)) == 0 && line[0] == '\0')
        { f_close(&fil); return -1; }
    strncpy(ssid, line, ssid_max - 1);
    ssid[ssid_max - 1] = '\0';

    // Line 2: password
    if (fat_readline(&fil, line, sizeof(line)) == 0 && line[0] == '\0')
        { f_close(&fil); return -1; }
    strncpy(password, line, pass_max - 1);
    password[pass_max - 1] = '\0';

    f_close(&fil);
    return 0;
}

// --- Index download & parse ---

static void download_progress(int bytes, int total) {
    splore_draw_progress("downloading index", bytes, total);
}

static int download_index(void) {
    splore_draw_status("downloading index", "please wait...");
    return http_get_to_file(INDEX_URL, INDEX_PATH, download_progress);
}

static int load_index(void) {
    static FIL fil;
    if (f_open(&fil, INDEX_PATH, FA_READ) != FR_OK) return -1;

    splore_count = 0;
    char line[256];
    bool skip_header = true; // first line is "---\tReleases"

    while (splore_count < SPLORE_MAX_ENTRIES) {
        int llen = fat_readline(&fil, line, sizeof(line));
        if (llen == 0 && line[0] == '\0') break; // EOF
        if (llen == 0) continue;                  // empty line

        if (skip_header) { skip_header = false; continue; }

        // Format: post_id\ttitle\tfilename.p8.png
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        // field 1: post_id (ignored)

        char *title = tab1 + 1;
        char *tab2 = strchr(title, '\t');
        if (!tab2) continue;
        *tab2 = '\0';
        // field 2: title

        // field 3: filename.p8.png — strip the extension
        char *filename = tab2 + 1;
        char *ext = strstr(filename, ".p8.png");
        if (ext) *ext = '\0';

        strncpy(splore_files[splore_count],  filename, sizeof(splore_files[0]) - 1);
        splore_files[splore_count][sizeof(splore_files[0]) - 1] = '\0';
        strncpy(splore_titles[splore_count], title,    sizeof(splore_titles[0]) - 1);
        splore_titles[splore_count][sizeof(splore_titles[0]) - 1] = '\0';
        splore_count++;
    }

    f_close(&fil);
    return splore_count;
}

// --- Cart download ---

static void cart_progress(int bytes, int total) {
    splore_draw_progress("downloading cart", bytes, total);
}

// Download cart to /carts/<filename>.p8.png. Returns 0 on success.
static int download_cart(const char *filename) {
    // Ensure /carts directory exists
    f_mkdir(CART_SAVE_DIR);

    char url[256];
    char dest[64];
    snprintf(url,  sizeof(url),  CART_URL_FMT,  filename[0], filename[1], filename);
    snprintf(dest, sizeof(dest), CART_SAVE_FMT, filename);

    printf("[splore] downloading: %s -> %s\n", url, dest);
    return http_get_to_file(url, dest, cart_progress);
}

// --- Main Splore browser UI ---

int p8_splore(lua_State *L) {
    // --- Read WiFi credentials ---
    char ssid[64], password[64];
    if (read_wifi_config(ssid, sizeof(ssid), password, sizeof(password)) != 0) {
        splore_draw_status("no wifi.cfg found", "add /wifi.cfg to SD");
        sleep_ms(3000);
        p8_console_print("splore: no /wifi.cfg\n");
        p8_console_print("format: line1=ssid line2=pw\n");
        p8_console_draw(); gfx_flip();
        return 0;
    }

    // --- Connect to WiFi ---
    printf("[splore] ssid='%s'\n", ssid);
    // (password not printed for security)

    // Check NINA hardware is responding before attempting connect
    char fw_ver[16] = {0};
    if (nina_get_fw_version(fw_ver, sizeof(fw_ver)) != 0) {
        printf("[splore] nina-fw not responding (SPI issue?)\n");
        splore_draw_status("wifi hw not found", "serial: nina fw err");
        sleep_ms(3000);
        p8_console_print("splore: nina not responding\n");
        p8_console_draw(); gfx_flip();
        return 0;
    }
    printf("[splore] nina-fw v%s\n", fw_ver);

    splore_draw_status("connecting to wifi", ssid);

    // Check if already connected
    int conn_status = nina_get_conn_status();
    printf("[splore] initial status: %d\n", conn_status);
    if (conn_status != NINA_STATUS_CONNECTED) {
        if (nina_connect_wpa(ssid, password) != 0) {
            splore_draw_status("wifi failed", "check ssid/pw in /wifi.cfg");
            sleep_ms(3000);
            p8_console_print("splore: wifi failed\n");
            p8_console_printf("splore: ssid=%s\n", ssid);
            p8_console_draw(); gfx_flip();
            return 0;
        }
    }
    printf("[splore] wifi connected\n");

    // --- Get/refresh cart index ---
    // Download if not cached on SD, or if the cached file is empty (failed prev download)
    FILINFO fno;
    bool need_dl = (f_stat(INDEX_PATH, &fno) != FR_OK) || (fno.fsize == 0);
    if (need_dl) {
        if (download_index() != 0) {
            splore_draw_status("index download", "failed");
            sleep_ms(3000);
            return 0;
        }
    }

    if (load_index() <= 0) {
        splore_draw_status("index parse failed", NULL);
        sleep_ms(2000);
        return 0;
    }

    printf("[splore] loaded %d carts\n", splore_count);

    // --- Browse UI ---
    int selected = 0;
    int scroll = 0;
    const int visible = 17; // lines available between header and footer

    while (true) {
        input_update();

        // Navigation
        if (input_btnp(2, 0)) { // up
            selected--;
            if (selected < 0) selected = splore_count - 1;
        }
        if (input_btnp(3, 0)) { // down
            selected++;
            if (selected >= splore_count) selected = 0;
        }

        // Refresh index — download first, then reload (FA_CREATE_ALWAYS overwrites)
        if (input_btnp(6, 0)) { // start/menu button — refresh index
            if (download_index() == 0) {
                load_index();
                selected = 0;
                scroll = 0;
            }
        }

        bool btn_o = input_btnp(4, 0);
        bool btn_x = input_btnp(5, 0);
        if (btn_o || btn_x) {
            // O = download + run, X = download only
            bool do_run = btn_o;
            const char *filename = splore_files[selected];
            const char *title    = splore_titles[selected];

            char dest[64];
            snprintf(dest, sizeof(dest), CART_SAVE_FMT, filename);

            // Download if not already on SD
            if (f_stat(dest, &fno) != FR_OK) {
                if (download_cart(filename) != 0) {
                    // If nina-fw got stuck (all commands returning EF), recover by
                    // resetting and reconnecting before showing the error to the user.
                    char fw_ver[16] = {0};
                    if (nina_get_fw_version(fw_ver, sizeof(fw_ver)) != 0) {
                        printf("[splore] nina-fw stuck, resetting...\n");
                        splore_draw_status("wifi reset...", "reconnecting");
                        nina_init();
                        nina_connect_wpa(ssid, password);
                    }
                    splore_draw_status("download failed", title);
                    sleep_ms(2000);
                    continue;
                }
            } else {
                printf("[splore] already on SD: %s\n", dest);
            }

            if (do_run) {
                printf("[splore] running %s\n", dest);
                p8_cart_run(L, dest);
                // After cart exits, reconnect display and redraw splore
                // (palette/framebuffer may have been touched by cart)
                selected = 0;
                scroll   = 0;
            } else {
                splore_draw_status("saved!", dest);
                sleep_ms(1000);
            }
            continue;
        }

        if (input_key(0x29)) { // ESC — exit splore
            return 0;
        }

        // Scroll to keep selected visible
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;

        // --- Draw browser UI ---
        gfx_cls(1); // dark blue background

        // Header
        gfx_print("wili8jam splore", 8, 2, 7);
        gfx_rectfill(0, 9, 127, 9, 6);

        // Cart list
        for (int i = 0; i < visible && scroll + i < splore_count; i++) {
            int idx = scroll + i;
            int y = 12 + i * 6;

            // Truncate title to 15 chars (leaves room for ">" prefix)
            char display[17];
            strncpy(display, splore_titles[idx], 15);
            display[15] = '\0';

            if (idx == selected) {
                gfx_rectfill(0, y, 127, y + 5, 12); // blue highlight
                gfx_print(">", 0, y, 7);
                gfx_print(display, 8, y, 7);
            } else {
                gfx_print(display, 8, y, 6);
            }
        }

        // Footer: button hints (16 chars * 8px = 128px — exact fit)
        gfx_rectfill(0, 119, 127, 119, 5);
        gfx_print("o:run x:save esc", 0, 121, 5);

        gfx_flip();
        sleep_ms(33); // ~30 fps
    }
}

void p8_splore_register(lua_State *L) {
    lua_register(L, "splore", p8_splore);
}
