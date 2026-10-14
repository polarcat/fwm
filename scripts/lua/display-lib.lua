--------------------------------------------------------------------------------
--
-- Display helpers.
--
--------------------------------------------------------------------------------

local display = {}

local lfs = require("lfs")

local function get_display_mode(card)
	local path = "/sys/class/drm/" .. card .. "/modes"
	local file = io.open(path, "r")
	if not file then
		print("Failed to open " .. path)
		return false
	end

	local mode = file:read("*l")
	file:close()

	-- Trim '\n' to ensure the comparison works.
	return mode:gsub("%s+", "")
end

local function display_connected(card)
	local path = "/sys/class/drm/" .. card .. "/status"
	local file = io.open(path, "r")
	if not file then
		print("Failed to open " .. path)
		return false
	end

	local status = file:read("*all")
	file:close()

	-- Trim '\n' to ensure the comparison works.
	status = status:gsub("%s+", "")
	if status == "connected" then
		return true
	elseif status == "disconnected" then
		return false
	else
		print("Unknown status '" .. status .. "'")
		return false
	end
end

function display.get_connected_displays()
	local displays = {}
	for card in lfs.dir("/sys/class/drm") do
		if card:match("^card%d+-") then
			if display_connected(card) then
				local display = card:match("^card%d+%-(.+)$")
				if display then
					local mode = get_display_mode(card)
					table.insert(displays, display .. " " .. mode)
				end
			end
		end
	end

	return displays
end

function display.get_all_displays()
	local displays = {}
	for card in lfs.dir("/sys/class/drm") do
		if card:match("^card%d+-") then
			local display = card:match("^card%d+%-(.+)$")
			if display then
				table.insert(displays, display)
			end
		end
	end

	return displays
end

return display
