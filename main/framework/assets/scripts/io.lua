-- @name 外扩接口
-- @desc 状态页：I2C 扫描、GPIO10/11 电平，并把外扩口在 GPIO / PWM / UART / CAN 之间切换
-- @perm io,timer

local page = ui.page()
local info = ui.label(page, "")

local mode = "GPIO"     -- GPIO / PWM / UART / CAN
local level = 0          -- GPIO10 输出电平
local found = "未扫描"   -- I2C 扫描结果

local function refresh()
    local l10, l11 = "-", "-"
    if mode == "GPIO" then
        l10 = tostring(io.gpio_read(10))
        l11 = tostring(io.gpio_read(11))
    end
    info:set_text(string.format("I2C 器件: %s\nGPIO10=%s  GPIO11=%s\n外扩口复用: %s",
                                found, l10, l11, mode))
end

-- 一路 I2C：GPIO1/2 上的总线，逐个地址试读一次（8..119，最多列 4 个）
local function scan_i2c()
    local hits = {}
    for addr = 8, 119 do
        if io.i2c_read(addr, 1) ~= nil then
            hits[#hits + 1] = string.format("%02X", addr)
            if #hits >= 4 then break end
        end
    end
    found = (#hits > 0) and table.concat(hits, " ") or "无"
    ui.toast("I2C: " .. found)
    refresh()
end

-- 一路外扩口：GPIO10/11。同一时刻只能一种复用，按顺序循环切换
local function cycle_mode()
    if mode == "GPIO" then
        mode = "PWM"
        io.pwm_set(10, 1000, 50)            -- GPIO10 输出 1 kHz / 50%
    elseif mode == "PWM" then
        io.pwm_stop(10)
        mode = "UART"
        io.uart_config(115200, 8, 0, 1)     -- 115200 8N1（占用 GPIO10/11）
    elseif mode == "UART" then
        mode = "CAN"
        io.can_config(500000, true)         -- 500 kbit/s，只听模式：没接收发器也不报错
    else
        io.can_stop()
        mode = "GPIO"
    end
    ui.toast("外扩口: " .. mode)
    refresh()
end

local function toggle_gpio()
    if mode ~= "GPIO" then
        ui.toast("先切回 GPIO 模式")
        return
    end
    level = 1 - level
    io.gpio_write(10, level)
    refresh()
end

ui.row(page, "扫", "扫描 I2C 总线", scan_i2c)
ui.row(page, "翻", "翻转 GPIO10 电平", toggle_gpio)
ui.row(page, "换", "切换外扩口复用", cycle_mode)

timer.every(2000, refresh)
refresh()
