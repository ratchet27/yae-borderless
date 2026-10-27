-- Usage: lua check_layout.lua <patched ui_ini.lua>
-- Loads the layout like the game (placeholders filled in) and prints the
-- Full Screen / Display Mode rows.
local f = assert(io.open(arg[1], "rb"))
local src = f:read("a"):gsub("##WINDOW_WIDTH", "2560"):gsub("##WINDOW_HEIGHT", "1440")
f:close()
assert(load(src))()

local function walk(t, fn)
  for _, v in ipairs(t) do
    if type(v) == "table" then fn(v, t); walk(v, fn) end
  end
end
walk(ui_windows_ini, function(v)
  if v.name == "Full Screen" or v.name == "Display Mode" or v.name == "Max FPS" then
    print(v.class_name, v.name, v.text or "", v.pos and (v.pos[1] .. "," .. v.pos[2]) or "")
    for _, child in ipairs(v) do
      if child.items then
        for k, item in pairs(child.items) do print("", k, item.column_0) end
      end
    end
  end
end)
