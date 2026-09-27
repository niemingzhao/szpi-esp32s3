-- @name 指针时钟
-- @desc 画布示例：表盘 + 时/分/秒指针，每秒刷新
-- @perm sys,timer

local W, H = 176, 176
local cx, cy = W / 2, H / 2
local R = W / 2             -- 表盘半径：正好画满画布

local page = ui.page()
local cv = ui.canvas(page, W, H)

-- 从圆心朝 angle 画一根指针（0 度 = 12 点）；half 是半边宽，用几条平行线凑粗
local function hand(angle, length, half, color)
    local a = math.rad(angle)
    local dx, dy = math.sin(a), -math.cos(a)
    local x2, y2 = cx + dx * length, cy + dy * length
    for o = -half, half do
        cv:line(cx + dy * o, cy - dx * o, x2 + dy * o, y2 - dx * o, color)
    end
end

local function draw()
    local t = sys.localtime()

    cv:fill(0x121820)
    cv:circle(cx, cy, R, 0x8A93A0)
    cv:circle(cx, cy, R - 1, 0x8A93A0)

    for i = 0, 11 do
        local a = math.rad(i * 30)
        local sx, sy = math.sin(a), -math.cos(a)
        cv:line(cx + sx * (R - 7), cy + sy * (R - 7),
                cx + sx * (R - 2), cy + sy * (R - 2), 0x6E7784)
    end

    hand((t.hour % 12) * 30 + t.min * 0.5, R * 0.50, 1, 0xE8ECF2)
    hand(t.min * 6 + t.sec * 0.1, R * 0.76, 0, 0xE8ECF2)
    hand(t.sec * 6, R * 0.86, 0, 0xE5534B)
end

timer.every(1000, draw)
draw()
