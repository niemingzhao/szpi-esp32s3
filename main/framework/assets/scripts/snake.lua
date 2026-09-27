-- @name 贪吃蛇
-- @desc 画布示例：点屏幕决定拐弯方向，吃到方块加分
-- @perm timer,input

local COLS, ROWS, CELL = 16, 12, 13
local W, H = COLS * CELL, ROWS * CELL

local page = ui.page()
local cv = ui.canvas(page, W, H)
local info = ui.label(page, "分数 0")

local body, dir, food, alive, grow, score

local function place_food()
    for _ = 1, 200 do
        local x, y = math.random(COLS), math.random(ROWS)
        local hit = false
        for _, s in ipairs(body) do
            if s.x == x and s.y == y then hit = true break end
        end
        if not hit then
            food = { x = x, y = y }
            return
        end
    end
    food = { x = 1, y = 1 }
end

local function cell(x, y, color)
    cv:rect((x - 1) * CELL, (y - 1) * CELL, CELL - 1, CELL - 1, color)
end

local function draw()
    cv:fill(0x0E141C)
    cell(food.x, food.y, 0xE5534B)
    for i, s in ipairs(body) do
        cell(s.x, s.y, (i == 1) and 0x7EE2A8 or 0x3FA46A)
    end
    if alive then
        info:set_text("分数 " .. score)
    else
        info:set_text("撞到了，点屏幕重来（分数 " .. score .. "）")
    end
end

local function reset()
    body = { { x = 8, y = 6 }, { x = 7, y = 6 }, { x = 6, y = 6 } }
    dir = { x = 1, y = 0 }
    score = 0
    grow = 0
    alive = true
    place_food()
    draw()
end

local function turn(nx, ny)
    if not alive then return end
    if dir.x == nx and dir.y == ny then return end      -- 已经朝这边了
    if dir.x == -nx and dir.y == -ny then return end    -- 不能 180 度掉头
    dir = { x = nx, y = ny }
end

local function step()
    if not alive then return end

    local head = { x = body[1].x + dir.x, y = body[1].y + dir.y }

    if head.x < 1 or head.x > COLS or head.y < 1 or head.y > ROWS then
        alive = false
        draw()
        return
    end
    for i = 2, #body do
        if body[i].x == head.x and body[i].y == head.y then
            alive = false
            break
        end
    end
    if not alive then
        draw()
        return
    end

    table.insert(body, 1, head)
    if head.x == food.x and head.y == food.y then
        score = score + 10
        grow = grow + 2
        place_food()
    end
    if grow > 0 then
        grow = grow - 1
    else
        table.remove(body)
    end
    draw()
end

-- 点屏幕：按点相对屏幕中心（320 x 240 -> 160 / 120）的方位拐弯
input.on_click(function(p)
    if not alive then
        reset()
        return
    end
    local dx, dy = p.x - 160, p.y - 120
    if math.abs(dx) > math.abs(dy) then
        turn((dx > 0) and 1 or -1, 0)
    else
        turn(0, (dy > 0) and 1 or -1)
    end
end)

math.randomseed(sys.uptime())
timer.every(400, step)
reset()
