#ifndef WIIPAD_H
#define WIIPAD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the Wii controller driver.
// i2c0 must already be initialized — call after audio_init().
// Safe to call even if no controller is plugged in; will retry automatically.
void wiipad_init(void);

// Read current button state. Returns a bitmask of WII_BTN_* flags.
// Returns 0 if no controller is connected.
// Handles connection detection and re-init internally; call once per frame.
uint16_t wiipad_read(void);

// Button bitmask flags returned by wiipad_read()
// Covers Classic Controller, NES Classic, and SNES Classic.
#define WII_BTN_LEFT   (1 << 0)   // D-pad left
#define WII_BTN_RIGHT  (1 << 1)   // D-pad right
#define WII_BTN_UP     (1 << 2)   // D-pad up
#define WII_BTN_DOWN   (1 << 3)   // D-pad down
#define WII_BTN_A      (1 << 4)   // A button  → PICO-8 O (btn 4)
#define WII_BTN_B      (1 << 5)   // B button  → PICO-8 X (btn 5)
#define WII_BTN_PLUS   (1 << 6)   // + (Start) → PICO-8 menu (btn 6)

#ifdef __cplusplus
}
#endif

#endif // WIIPAD_H
