# Plan: I2C Wii Nunchuck Adapter Support

Adds support for the Adafruit Wii Nunchuck Breakout Adapter (#4836) plugged into the
Fruit Jam's STEMMA QT port. Allows use of NES Classic, SNES Classic, Wii Classic, and
other Wii-protocol controllers alongside or instead of a USB gamepad. 
Based on pico-infonesPlus-main https://github.com/fhoedemakers/pico-infonesPlus

## Hardware Context

The Fruit Jam's STEMMA QT connector is on **i2c0, GPIO 20 (SDA) / GPIO 21 (SCL)** —
the same bus already used by the TLV320DAC3100 audio codec (address 0x18). The Wii
adapter lives at address **0x52**. Since the codec only uses I2C during `audio_init()`
at boot (no runtime I2C writes), there is no bus contention during gameplay. The 100 kHz
speed already configured by `audio_init()` is fully compatible with the Wii protocol —
no re-initialization of the bus is needed.

## Files to Change

| File | Change |
|------|--------|
| `src/wiipad.c` | New — self-contained Wii controller driver |
| `src/wiipad.h` | New — public API for the driver |
| `src/input.c` | Add `input_wiipad_update()` call inside `input_update()` |
| `src/input.h` | Expose `input_wiipad_init()` and USB controller count poll setter |
| `src/main.cpp` | Call `input_wiipad_init()` after `audio_init()`; register poll lambda |
| `CMakeLists.txt` | Add `src/wiipad.c` to source list |

No new library dependencies — `hardware_i2c` is already linked.

---

## Step 1 — New file: `src/wiipad.c` + `src/wiipad.h`

A self-contained driver with three responsibilities.

### A. Initialization

Called once after `audio_init()` so i2c0 is already live. Does not call `i2c_init()`
again — just reuses the existing bus.

1. Write `0x55` to register `0xF0` — disables encryption (modern init sequence)
2. Write `0x00` to register `0xFB` — puts the controller in unencrypted data mode
3. Track a `connected` boolean; if `i2c_write_timeout_us()` returns an error, mark not
   connected and retry every ~60 frames

### B. Per-frame read

Called from `input_update()` once per frame.

1. Write `0x00` to request a data burst
2. Short delay (~100 µs)
3. Read 6 bytes
4. Decode the 6-byte Classic Controller packet (standard WiiBrew format) into
   directional + button bits
5. Return decoded button state

### C. Button mapping to PICO-8

| Wii Classic button | PICO-8 button |
|--------------------|---------------|
| D-pad left         | btn 0 (left)  |
| D-pad right        | btn 1 (right) |
| D-pad up           | btn 2 (up)    |
| D-pad down         | btn 3 (down)  |
| A                  | btn 4 (O)     |
| B                  | btn 5 (X)     |
| Plus (+) / Start   | btn 6 (menu)  |

NES Classic: A→O, B→X, Start→menu.
SNES Classic: A→O, B→X, Start→menu. (X/Y/L/R have no PICO-8 equivalent in the
6-button model, but could optionally alias to O/X if desired later.)

---

## Step 2 — Modify `src/input.c`

Add `input_wiipad_update()` inside `input_update()`, after `tuh_task()` and before the
btnp counter loop. It:

1. Calls the wiipad driver to get current button state
2. Determines player assignment:
   - **No USB controller connected** → wiipad = player 1
   - **USB controller connected** → wiipad = player 2
   - Detection via a registered poll function (see step 3)
3. Calls `key_set()` / `key_clear()` on the appropriate virtual keycodes
   (`VKEY_GP1_*` or `VKEY_GP2_*`) in `key_state[]`

This slots into the existing architecture — no new Lua API, no changes to `input_btn()`
or `input_btnp()`. The wiipad just feeds into the same keycode bitfield as USB gamepads
and the keyboard, so btnp auto-repeat works for free.

To avoid a circular dependency (input.c does not know about `obUSBHost`), add a small
**USB controller count poll function pointer** using the same pattern already established
for mouse and modifier polling (`input_set_mouse_poll`, `input_set_modifier_poll`).

---

## Step 3 — Modify `src/input.h`

Expose:

```c
void input_wiipad_init(void);
void input_set_usb_controller_count_poll(int (*fn)(void));
```

---

## Step 4 — Modify `src/main.cpp`

After `audio_init()`:

1. Call `input_wiipad_init()` and log whether a controller was detected
2. Register the USB controller count poll lambda (mirrors the mouse/modifier lambdas
   already present):

```cpp
input_set_usb_controller_count_poll([]() -> int {
    return obUSBHost.m_obHID.getControllerCount()
         + obUSBHost.m_obXInput.getMountedCount();
});
```

---

## Step 5 — Modify `CMakeLists.txt`

Add `src/wiipad.c` to the `add_executable(wili8jam ...)` source list.

---

## Edge Cases

- **No controller at boot**: init fails gracefully, periodic retry every ~60 frames
- **Controller unplugged and replugged**: re-detected on next retry cycle
- **Two Wii controllers**: the STEMMA QT port is a single I2C bus, so only one adapter
  at a time. A second Wii controller would require a separate I2C bus or an I2C
  multiplexer — out of scope.
- **Bus conflict during audio init**: `input_wiipad_init()` is called after
  `audio_init()` completes, so the codec sequence is fully done before touching the bus.
