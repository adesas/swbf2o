/* Spike: compile the decompiled LuaFL module with Emscripten and run a game-style script.
 * If this works, the "swap only the platform layer" thesis holds: the decompiled source
 * is portable C, and the game's scripting layer needs no reimplementation.
 */
#include <stdio.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static int print_binding(lua_State *L) {
  const char *s = lua_tostring(L, 1);
  printf("[game-lua] %s\n", s ? s : "(nil)");
  return 0;
}

int main(void) {
  printf("compiled Lua version: %s\n", LUA_VERSION);

  lua_State *L = lua_open();
  luaopen_base(L);
  luaopen_string(L);
  luaopen_table(L);

  /* stand-in for the engine's Zephyr/LuaFL bindings (Gametoast/SWBF2-Lua-API style) */
  lua_register(L, "print", print_binding);

  const char *script =
    "unit = { name = 'sp_cl_pi', health = 100, faction = 'cl' }\n"
    "function spawn(u) print('spawning ' .. u.name .. ' hp=' .. u.health) end\n"
    "spawn(unit)\n";

  int err = lua_dostring(L, script);
  if (err) {
    printf("lua error: %s\n", lua_tostring(L, -1));
    return 1;
  }
  printf("ok: lua VM ran the mission-style script in wasm\n");
  lua_close(L);
  return 0;
}
