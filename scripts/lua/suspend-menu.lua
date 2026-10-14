local suspend = {}

local sys = dofile(os.getenv("FWM_HOME") .. "/scripts/lua/system-lib.lua")

--------------------------------------------------------------------------------
--
-- Local functions.
--
--------------------------------------------------------------------------------

local suspend_script = [[
#!/bin/sh

if ! pidof xscreensaver >/dev/null 2>&1; then
	xscreensaver -no-splash &
fi

for i in 0 1 2 3 4 5 6 7 8 9; do
	if pidof xscreensaver >/dev/null 2>&1; then
		break
	fi
	sleep 1
done

xscreensaver-command --lock
sleep 3 # give it some time before screen gets blank
sudo pm-suspend >$FWM_HOME/tmp/suspend.log 2>&1
]]

local function show_menu(menu)
	local menu_items = {
		"\a\t\n",
		"\n",
		"\a\tsuspend system\n",
		"\a\tcancel\n",
	}

	local text = menu.run(sys.config, menu_items)
	if not text then
		return
	end

	local fields = {}
	for field in text:gmatch("[^\t\n]+") do
		fields[#fields + 1] = field
	end

	if not fields then
		return
	elseif fields[2] ~= "suspend system" then
		return
	end

	local path = os.getenv("FWM_HOME") .. "/tmp/suspend.sh"
	local file, err = io.open(path, "w")
	if not file then
		error("Could not open file '" .. path .. "': " .. err)
	end

	file:write(suspend_script)
	file:close()

	sys.run("/bin/sh " .. path)
end

--------------------------------------------------------------------------------
--
-- Public API.
--
--------------------------------------------------------------------------------

-- As a submenu.
function suspend.start_menu(menu)
	show_menu(menu)
end

-- As a standalone menu.
function start()
	show_menu(sys.load_library("menulib-lua"))
end

return suspend
