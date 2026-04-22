#ifndef P8_SPLORE_H
#define P8_SPLORE_H

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

// Read WiFi credentials from /wifi.cfg, connect to WiFi, and open the Splore
// cart browser. Displays a scrollable list of carts sourced from the gameflix
// community index. The user can browse, download, and run carts from the BBS.
//
// /wifi.cfg format (SD card root):
//   line 1: WiFi SSID
//   line 2: WiFi password
//
// Called as a Lua global: splore()
int p8_splore(lua_State *L);

// Register the splore() Lua global.
void p8_splore_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // P8_SPLORE_H
