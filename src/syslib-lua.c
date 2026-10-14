/* syslib-lua.c: some system utilities library for Lua.
 *
 * Copyright (c) 2026, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

#define LOG_TAG "syslib: "

#include "misc.h"

static int sentinel_gc(lua_State *L)
{
	(void)L;
	return 0;
}

//------------------------------------------------------------------------------
//
// Lua API
//
static int getenv2(lua_State *L)
{
	const char *val = getenv(luaL_checkstring(L, 1));
	if (val == NULL) {
		lua_pushnil(L);
	} else {
		lua_pushlstring(L, val, strlen(val));
	}

	return 1;
}

static int setenv2(lua_State *L)
{
	const char *var = luaL_checkstring(L, 1);
	const char *val = luaL_checkstring(L, 2);

	if (var == NULL) {
		return 0;
	} else if (var == NULL || *var == '\0') {
		unsetenv(var);
	} else if (setenv(var, val, 1) != 0) {
		return luaL_error(L, "Failed to set environment '%s=%s'",
			var, val);
	}

	return 0;
}

static int unsetenv2(lua_State *L)
{
	const char *var = luaL_checkstring(L, 1);
	if (var == NULL) {
		return 0;
	}
	unsetenv(var);
	return 0;
}

static int puts2(lua_State *L)
{
	int fd = luaL_checkinteger(L, 1);
	const char *str = luaL_checkstring(L, 2);
	if (fd < 0 || str == NULL) {
		return 0;
	}

	size_t len = strlen(str) + 1;
	if (write(fd, str, len) != len) {
		loge("Failed to write '%s' to %d", str, fd);
	}

	return 0;
}

static int setprop(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	const char *str = luaL_checkstring(L, 2);

	errno = 0;
	for (int i = 0; i < 2; i++) {
		int fd = open(path, O_WRONLY | O_NONBLOCK);
		if (fd < 0) {
			if (errno == ENOENT) {
				// Special case for FIFOs which might be
				// re-created by the owner.
				sleep(1);
				continue;
			} else if (errno != ENXIO) {
				loge("Failed to open '%s'", path);
				break;
			}
		} else {
			size_t len = strlen(str) + 1;
			if (write(fd, str, len) != len) {
				loge("Failed to write '%s' to '%s'", str, path);
			}
			close(fd);
			break;
		}
		break;
	}

	return 0;
}

static int sleep2(lua_State *L)
{
	sleep(luaL_checkinteger(L, 1));
	return 0;
}

static int msleep2(lua_State *L)
{
	struct timespec need;
	struct timespec left;

	lua_Integer ms = luaL_checkinteger(L, 1);
	if (ms < 0) {
		return 0;
	}

	need.tv_sec  = (time_t)(ms / 1000);
	need.tv_nsec = (long)((ms % 1000) * 1000000);
	while (nanosleep(&need, &left) == -1) {
		if (errno != EINTR) {
			return 0;
		}
		need = left;
	}

	return 0;
}

int luaopen_lua(lua_State *L)
{
	lua_newuserdatauv(L, 0, 0);
	if (luaL_newmetatable(L, "syslib.sentinel")) {
		lua_pushcfunction(L, sentinel_gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);
	lua_setfield(L, LUA_REGISTRYINDEX, "syslib.sentinel");

	luaL_Reg funcs[] = {
		{"getenv", getenv2},
		{"setenv", setenv2},
		{"unsetenv", unsetenv2},
		{"setprop", setprop},
		{"puts", puts2},
		{"sleep", sleep2},
		{"msleep", msleep2},
		{NULL, NULL}
	};
	luaL_newlib(L, funcs);
	return 1;
}
//
// Lua API
//
//------------------------------------------------------------------------------
