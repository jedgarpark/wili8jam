#include "wiipad.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"
#include <stdio.h>

// Wii controller I2C address (Classic, NES Classic, SNES Classic)
#define WII_ADDR         0x52

// How many frames to wait between re-init attempts when not connected
#define WII_RETRY_FRAMES 60

static bool wii_connected    = false;
static int  wii_retry_counter = 0;

// Write a single register on the controller.
// Returns true on success.
static bool wii_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return i2c_write_timeout_us(i2c0, WII_ADDR, buf, 2, false, 3000) == 2;
}

// Send the "new" initialization sequence that disables encryption.
// Returns true if the controller acknowledged both writes.
static bool wii_try_init(void) {
    if (!wii_write_reg(0xF0, 0x55)) return false;
    sleep_us(100);
    if (!wii_write_reg(0xFB, 0x00)) return false;
    sleep_us(100);
    return true;
}

void wiipad_init(void) {
    // i2c0 is already initialized at 100 kHz on GPIO 20/21 by audio_init()
    wii_connected     = wii_try_init();
    wii_retry_counter = 0;
    if (wii_connected)
        printf("Wii controller: connected\n");
    else
        printf("Wii controller: not found (will retry each %d frames)\n", WII_RETRY_FRAMES);
}

uint16_t wiipad_read(void) {
    // If not connected, periodically retry initialization
    if (!wii_connected) {
        if (++wii_retry_counter >= WII_RETRY_FRAMES) {
            wii_retry_counter = 0;
            wii_connected = wii_try_init();
            if (wii_connected)
                printf("Wii controller: connected\n");
        }
        return 0;
    }

    // Request a data burst by writing register 0x00
    uint8_t req = 0x00;
    if (i2c_write_timeout_us(i2c0, WII_ADDR, &req, 1, false, 3000) != 1) {
        wii_connected = false;
        printf("Wii controller: disconnected\n");
        return 0;
    }

    // Allow the controller time to prepare data (~200 µs is sufficient at 100 kHz)
    sleep_us(200);

    // Read 6 bytes of controller state
    uint8_t data[6];
    if (i2c_read_timeout_us(i2c0, WII_ADDR, data, 6, false, 3000) != 6) {
        wii_connected = false;
        printf("Wii controller: disconnected\n");
        return 0;
    }

    // Decode digital buttons from bytes 4 and 5.
    // All button bits are active LOW (0 = pressed, 1 = released).
    //
    // Byte 4:  BDR(7) BDD(6) BLT(5) B-(4) BH(3) B+(2) BRT(1) 1(0)
    // Byte 5:  BZL(7) BB(6)  BY(5)  BA(4) BX(3) BZR(2) BDL(1) BDU(0)
    //
    // BDR/BDL/BDU/BDD = right/left/up/down on D-pad
    // BA = A button, BB = B button, B+ = Plus/Start
    uint8_t b4 = data[4];
    uint8_t b5 = data[5];

    uint16_t btns = 0;
    if (!(b5 & 0x02)) btns |= WII_BTN_LEFT;   // BDL  byte5 bit1
    if (!(b4 & 0x80)) btns |= WII_BTN_RIGHT;  // BDR  byte4 bit7
    if (!(b5 & 0x01)) btns |= WII_BTN_UP;     // BDU  byte5 bit0
    if (!(b4 & 0x40)) btns |= WII_BTN_DOWN;   // BDD  byte4 bit6
    if (!(b5 & 0x10)) btns |= WII_BTN_A;      // BA   byte5 bit4
    if (!(b5 & 0x40)) btns |= WII_BTN_B;      // BB   byte5 bit6
    if (!(b4 & 0x04)) btns |= WII_BTN_PLUS;   // B+   byte4 bit2

    return btns;
}
