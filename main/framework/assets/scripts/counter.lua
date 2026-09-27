-- @name 计数
-- @desc 无界面示例：每秒打印计数与时间戳，10 秒后自己退出
-- @perm sys,timer

local function stamp()
    local t = sys.localtime()
    return string.format("%02d:%02d:%02d", t.hour, t.min, t.sec)
end

local n = 0
print("开始  " .. stamp())

timer.every(1000, function()
    n = n + 1
    print(string.format("#%d  %s", n, stamp()))
    if n >= 10 then
        print("结束，退出")
        sys.exit()
    end
end)
