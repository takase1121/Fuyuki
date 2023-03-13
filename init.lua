-- mod-version:3
local core = require "core"
local common = require "core.common"
local config = require "core.config"
local style = require "core.style"
local Object = require "core.object"
local process = require "process"


---A function called when configuration has changed.
---@type fun(): nil
local on_config_change


---A function to call on_config_change as the function
---is initially not defined.
local function defer_on_config_change(...)
  return on_config_change(...)
end


---Configuration used by the plugin.
---@class ImmersiveTitleConfig
---@field extend_frame boolean
---@field backdrop_type "default" | "none" | "mica" | "acrylic" | "tabbed"
---@field adaptive_theme boolean
---@field adaptive_accent boolean
---@field adaptive_accent_contrast boolean
---@field theme_dark string
---@field theme_light string
---@field monitor_paths string[]
---@field class_name string
---@field min_contrast_ratio number
config.plugins.immersive_title = common.merge(config.plugins.immersive_title, {
  -- extend window frame into client area
  extend_frame = true,
  -- the backdrop type
  backdrop_type = "mica",
  -- changes theme based on Windows settings
  adaptive_theme = true,
  -- detects the accent color from windows and automatically apply it to the theme
  adaptive_accent = false,
  -- automatically uses a tint of the accent that has good contrast
  adaptive_accent_contrast = true,
  -- default dark theme
  theme_dark = "colors.default",
  -- default light theme
  theme_light = "colors.summer",
  -- default path to the monitor
  monitor_paths = {
    USERDIR .. "/plugins/immersive-title/monitor.exe",
    DATADIR .. "/plugins/immersive-title/monitor.exe"
  },
  -- class name of the window
  class_name = "SDL_app",
  -- the minimum contrast ratio for the accent
  min_contrast_ratio = 5.0,

  config_spec = {
    name = "Mica",
    {
      label = "Extend window frame into client area",
      description = "When enabled, the window frame will be extended into the window, "
                    .. "applying Mica/Acrylic onto the window itself.",
      path = "extend_frame",
      type = "toggle",
      default = true,
      on_apply = defer_on_config_change
    },
    {
      label = "Backdrop type",
      description = "The type of backdrop for the window frame. "
                    .. "Only supports Windows 11 build 22621 and above.",
      path = "backdrop_type",
      type = "selection",
      values = {
        {"Default", "default" },
        { "None", "none" },
        { "Mica", "mica" },
        { "Acrylic", "acrylic" },
        { "Mica (Tabbed)", "tabbed" }
      },
      default = "default",
      on_apply = defer_on_config_change
    },
    {
      label = "Adaptive theming",
      description = "Changes the theme according to Windows settings.",
      path = "adaptive_theme",
      type = "toggle",
      default = true,
      on_apply = defer_on_config_change
    },
    {
      label = "Adaptive accent color",
      description = "Changes the accent color of the current theme based on "
                    .. "Windows' accent color.",
      path = "adaptive_accent",
      type = "toggle",
      default = false,
      on_apply = defer_on_config_change
    },
    {
      label = "Adaptive accent contrast",
      description = "If the selected accent color doesn't have sufficient contrast "
                      .. "compared to the background, automatically generate a tint "
                      .. "or a shade that does.",
      path = "adaptive_accent_contrast",
      type = "toggle",
      default = true,
      on_apply = defer_on_config_change
    },
    {
      label = "Light theme",
      description = "The theme to use when Windows is set to use light theme.",
      path = "theme_light",
      type = "string",
      default = "colors.default",
      on_apply = defer_on_config_change
    },
    {
      label = "Dark theme",
      description = "The theme to use when Windows is set to use dark theme.",
      path = "theme_dark",
      type = "string",
      default = "colors.summer",
      on_apply = defer_on_config_change
    }
  }
})

local C = config.plugins.immersive_title

---Theme colors.
---@alias ThemeType
---| "dark" # Dark theme
---| "light" # Light theme

---A color.
---@alias Color {[1]: number, [2]: number, [3]: number}

---Backdrop types mapped to their respective numeric values for the monitor.
---@alias BackdropType
---| "default" # let Windows pick the default backdrop.
---| "none" # Opaque backdrop.
---| "mica" # Mica-style backdrop.
---| "acrylic" # Acrylic-style backdrop.
---| "tabbed" # Mica-style backdrop with tabbed (opaque) title bar.
local BACKDROP_TYPE = {
  default = 0,
  none = 1,
  mica = 2,
  acrylic = 3,
  tabbed = 4,
}

---Monitors theme change and reports various stuffs.
---@class Monitor
local Monitor = Object:extend()


---Gets the first path that is executable.
---@param possible_paths string[] a list of possible paths
---@returns string | nil the path to the executable
local function get_exe_path(possible_paths)
  for _, p in ipairs(possible_paths) do
    local info = system.get_file_info(p)
    if info and info.type == "file" then
      return p
    end
  end
end


--- Creates the new monitor process.
function Monitor:new()
  ---A command to be sent to the monitor.
  ---@class Command
  ---@field cmd string the command
  ---@field cb fun(res: string, err: string): nil the callback to run when a response is received

  ---A queue of items should be sent when the monitor is ready.
  ---@type Command[]
  self.pending = {}
  ---A map of commands that have been sent and is awaiting results, identified by their serial.
  ---@type {string: Command}
  self.sent = {}
  ---A queue of received responses from the monitor waiting to be processed.
  ---@type string[]
  self.received = {}
  ---The last fragment received from the monitor.
  ---@type string
  self.last_fragment = ""
  ---Indicates whether the monitor is ready to receive commands.
  ---@type boolean
  self.ready = false
  ---The serial used to send data to the monitor.
  ---@type number
  self.serial = 0
  ---The monitor process.
  ----@type Process
  self.proc = nil
end


---Starts the monitor process.
function Monitor:start()
  if self.proc then return end
  local exec_path = assert(get_exe_path(C.monitor_paths), "cannot find monitor")
  self.proc = assert(process.start({ exec_path, system.get_process_id(), C.class_name }, {
    stdin = process.REDIRECT_PIPE,
    stdout = process.REDIRECT_PIPE,
    stderr = process.REDIRECT_STDOUT
  }), "cannot start monitor")
end


---Sends a command to the monitor and returns the response
---@param cmd string the command
---@param cb fun(response: string): nil the callback to call after a response had been received
function Monitor:send(cmd, cb)
  local cmd = { cmd = cmd, cb = cb }
  if self.ready then
    self:_send(cmd)
  else
    self.pending[#self.pending+1] = cmd
  end
end


---A function that does nothing.
local function noop() end


---Configures the monitor.
---@param extend_frame boolean if true, the window frame will be
---extended into the application, giving a frosted glass look.
---@param backdrop_type BackdropType program backdrop type.
function Monitor:configure(extend_frame, backdrop_type)
  self:send(string.format("config %d%d", extend_frame and 1 or 0, BACKDROP_TYPE[backdrop_type]), noop)
end


---Gets the current Windows App theme.
---@param cb fun(theme: ThemeType): nil the result callback.
function Monitor:get_theme(cb)
  self:send("theme ", function(res) cb(res == "1" and "dark" or "light") end)
end


---Parses the color returned from the monitor.
---The color returned from the monitor is a 32-bit unsigned integer
---in the format 0xRRGGBBAA.
---@param str string the color string
---@returns Color the parsed color
local function parse_color(str)
  local v = tonumber(str, 10)
  return { (v & 0xFF000000) >> 24, (v & 0xFF0000) >> 16, (v & 0xFF00) >> 8, v & 0xFF }
end


---Gets the current accent color.
---@param cb fun(color: Color, opaque: boolean): nil the result callback
function Monitor:get_accent_color(cb)
  self:send("accent ", function(res)
    local opaque, color = res:match("^(%d) (%d+)$")
    cb(parse_color(color), opaque == "1")
  end)
end


---Sends a command to the monitor.
---@param cmd Command the command to send.
function Monitor:_send(cmd)
  self.proc:write(string.format("%d %s\n", self.serial, cmd.cmd))
  self.sent[tostring(self.serial)] = cmd
  self.serial = self.serial + 1
end


---Sets the current theme based on Windows' theme
---if adaptive theming is enabled.
---@param type ThemeType
local function set_theme(type)
  -- if adaptive theming is used, change the theme
  if C.adaptive_theme then
    local theme = type == "dark" and C.theme_dark or C.theme_light
    core.reload_module(theme)
  end
end


---Calculates the relative luminance of a color.
---@param color Color the color
---@returns number
local function relative_luminance(color)
  local r, g, b = table.unpack(color)
  r = r / 255
  g = g / 255
  b = b / 255
  if r <= 0.03928 then
    r = r / 12.92
  else
    r = ((r + 0.055) / 1.055) ^ 2.4
  end
  if g <= 0.03928 then
    g = g / 12.92
  else
    g = ((g + 0.055) / 1.055) ^ 2.4
  end
  if b <= 0.03928 then
    b = b / 12.92
  else
    b = ((b + 0.055) / 1.055) ^ 2.4
  end
  return 0.2126 * r + 0.7152 * g + 0.0722 * b
end


---Calculates the contrast ratio between two colors.
---@param a Color the first color
---@param b Color the second color
---@returns number
local function contrast_ratio(a, b)
  local L1, L2 = relative_luminance(a), relative_luminance(b)
  L1, L2 = math.max(L1, L2), math.min(L1, L2)
  return (L1 + 0.05) / (L2 + 0.05)
end


---Gets a tint or shade of a color based on the percentage.
---@param color Color the color
---@param percentage number the percentage from 0 to 100
---@retuns Color a color that is percentage brighter/darker than the input
local function color_shade(color, percentage)
  percentage = 1 + percentage / 100
  return {
    math.min(255, math.floor(color[1] * percentage)),
    math.min(255, math.floor(color[2] * percentage)),
    math.min(255, math.floor(color[3] * percentage)),
    color[4]
  }
end


---Gets the best accent color that has sufficient contrast.
---@param color Color the initial accent color
---@param background Color the background
local function get_best_accent_color(color, background)
  local low, high = -100, 100
  local result = nil
  while low <= high do
    local mid = math.floor((low + high) / 2)
    local c = color_shade(color, mid)
    if contrast_ratio(c, background) >= C.min_contrast_ratio then
      result = c
      high = mid - 1
    else
      low = mid + 1
    end
  end
  return result
end


---Sets the accent color if adaptive accent is enabled.
---@param color Color the new accent color
---@param opaque boolean true if the color is an opaque blend
local function set_accent_color(color, opaque)
  -- if the color is opaque, we should set the alpha to 0xFF
  if opaque then
    color[4] = 0xFF
  end

  if C.adaptive_accent then
    if C.adaptive_accent_contrast then
      -- FIXME: using only style.background is unreliable
      color = get_best_accent_color(color, style.background)
    end
    style.caret = color
  end
end


---A function called when the monitor is ready for commands and events.
function Monitor:on_ready()
  -- Send configuration to the monitor
  self:configure(C.extend_frame, C.backdrop_type)
  self:get_theme(set_theme)
  self:get_accent_color(set_accent_color)

  -- send every pending message
  for _, payload in ipairs(self.pending) do
    self:_send(payload)
  end
  self.pending = {}
end


---A function called when the current theme changed.
---@param type ThemeType the current theme
function Monitor:on_theme_change(type)
  set_theme(type)
end


---A function called when the accent color changed.
---@param color Color the current accent color
---@param opaque boolean if true, the alpha value of the color should be ignored
function Monitor:on_accent_change(color, opaque)
  set_accent_color(color, opaque)
end


---A function called when the monitor receives an error.
---@param err string the error message
function Monitor:on_error(err)
  core.error("monitor error: %s", err)
end


---Parses the response from the monitor.
---@param resp string the response from monitor
---@returns string string, string the serial, type and actual response
local function parse_message(resp)
  return resp:match("^([^ ]+) ([^ ]+) (.*)$")
end


---A function called when the monitor receives a response.
function Monitor:on_recv()
  -- drain the queue
  while #self.received > 0 do
    local cmd = table.remove(self.received, 1)
    local serial, type, content = parse_message(cmd)

    if serial == "-1" then
      if type == "ready" then
        self.ready = true
        self:on_ready()
      elseif type == "themechange" then
        self:on_theme_change(content == "1" and "dark" or "light")
      elseif type == "accentchange" then
        local opaque, color = content:match("^(%d) (%d+)$")
        self:on_accent_change(parse_color(color), opaque == "1")
      elseif type == "error" then
        self:on_error(content)
      else
        self:on_error(string.format("unknown broadcast: %q", content))
      end
    else
      -- remove message from sent queue
      local sent_message = self.sent[serial]
      self.sent[serial] = nil
      if sent_message then
        if type == "ok" then
          sent_message.cb(content)
        elseif type == "error" then
          sent_message.cb(nil, content)
        else
          self:on_error(string.format("unknown response type: %q", type))
        end
      else
        self:on_error(string.format("unknown serial: %q", serial))
      end
    end
  end
end


---Stops the monitor.
function Monitor:stop()
  self:configure(false, "default")
  self.proc:terminate()
  self.proc:wait(50)
  self.proc:kill()
  self.proc = nil
  self.ready = false
end


---Polls the monitor for more messages.
function Monitor:poll()
  local buf, err = self.proc:read_stdout()
  if not buf then
    self.ready = false
    return self:on_error(err)
  end

  buf = self.last_fragment .. buf
  if buf == "" then
    return
  end

  local last_pos
  local n = #self.received
  for cmd, pos in buf:gmatch("([^\r\n]+)\r?\n()") do
    self.received[#self.received + 1] = cmd
    last_pos = pos + 1
  end

  -- if there is something left after parsing, we'll save it
  if last_pos then
    self.last_fragment = buf:sub(last_pos)
  else
    self.last_fragment = buf
  end

  -- if there's something in the queue, process it
  if #self.received ~= n then
    self:on_recv()
  end
end


local default_accent = style.caret
local monitor = Monitor()


function on_config_change()
  if not monitor then return end
  monitor:configure(C.extend_frame, C.backdrop_type)
  if C.adaptive_accent then
    monitor:get_accent_color(set_accent_color)
  elseif default_accent then
    style.caret = default_accent
  end
end


local core_quit = core.quit
function core.quit(force)
  monitor:stop()
  return core_quit(force)
end


local core_restart = core.restart
function core.restart()
  monitor:stop()
  return core_restart()
end


core.add_thread(function()
  -- save the color once again just in case someone tried to change the color
  default_accent = style.caret
  monitor:start()
  while true do
    monitor:poll()
    coroutine.yield(0)
  end
end)


return monitor
