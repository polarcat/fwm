/* fwm-lua.c: lua script launcher for fwm.
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "misc.h"

int main(int argc, const char *argv[])
{
	if (argc < 2 || argv[1] == NULL) {
		printf("\nUsage: %s <script>\n\n", argv[0]);
		return 1;
	}

	const char *script = argv[1];
	lua_State *lua;
	int rc = 1;

	logd("Run Lua script '%s'", script);

	if (!(lua = luaL_newstate())) {
		loge("failed to create Lua state");
		return 0;
	}
	luaL_openlibs(lua);

	if (luaL_dofile(lua, script) != LUA_OK) {
		loge("Lua script error: %s", lua_tostring(lua, -1));
		goto out;
	}

	lua_getglobal(lua, "start");
	if (!lua_isfunction(lua, -1)) {
		loge("Lua start() function not found");
		goto out;
	}

	if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
		loge("Lua start() function failed: %s", lua_tostring(lua, -1));
		goto out;
	}
	rc = 0;

out:
	lua_close(lua);
	return rc;
}
