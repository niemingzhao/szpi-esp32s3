-- @name 五子棋（网页版）
-- @desc 在浏览器里下五子棋：运行后用同一局域网的电脑 / 手机打开屏幕上的地址。15×15，黑先，五连即胜
-- @perm ui,timer,json,web
--
-- 走的是脚本的 web 能力：路由挂在 /s 底下，只在脚本运行期间有效（离开脚本页脚本就停，
-- 网页也就没了）。设备屏幕上同步显示棋盘与当前该谁走，屏幕自动熄屏不影响游戏。

local N = 15
local BLACK, WHITE = 1, 2
local CELL = 10                       -- 设备上棋盘格边长（画布 N*CELL 见方）

local board = {}
local turn = BLACK
local winner = 0
local history = {}
local last = nil

local function reset()
    board = {}
    for y = 1, N do
        board[y] = {}
        for x = 1, N do board[y][x] = 0 end
    end

    turn = BLACK
    winner = 0
    history = {}
    last = nil
end

local function at(x, y)
    if x < 1 or x > N or y < 1 or y > N then return -1 end
    return board[y][x]
end

-- 落子处是否已经五连
local function wins(x, y, p)
    local dirs = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } }

    for _, d in ipairs(dirs) do
        local n = 1
        for s = 1, 4 do
            if at(x + d[1] * s, y + d[2] * s) == p then n = n + 1 else break end
        end
        for s = 1, 4 do
            if at(x - d[1] * s, y - d[2] * s) == p then n = n + 1 else break end
        end
        if n >= 5 then return true end
    end

    return false
end

local function place(x, y)
    if winner ~= 0 then return false, "已经分出胜负，先重新开始" end
    if x < 1 or x > N or y < 1 or y > N then return false, "落子超出棋盘" end
    if board[y][x] ~= 0 then return false, "这个位置已经有子了" end

    board[y][x] = turn
    history[#history + 1] = { x = x, y = y, p = turn }
    last = { x = x, y = y }

    if wins(x, y, turn) then
        winner = turn
    else
        turn = (turn == BLACK) and WHITE or BLACK
    end

    return true
end

local function undo()
    if #history == 0 then return false end

    local m = table.remove(history)
    board[m.y][m.x] = 0
    winner = 0
    turn = m.p
    last = history[#history]
    return true
end

-- 状态：棋盘压成一串数字（"0" 空 / "1" 黑 / "2" 白），last 是最后一手的格子序号
local function state()
    local t = {}
    for y = 1, N do
        for x = 1, N do t[#t + 1] = tostring(board[y][x]) end
    end

    return {
        size = N,
        turn = turn,
        winner = winner,
        moves = #history,
        last = last and ((last.y - 1) * N + (last.x - 1)) or -1,
        board = table.concat(t),
    }
end

local function turn_text()
    if winner == BLACK then return "黑棋胜" end
    if winner == WHITE then return "白棋胜" end
    return (turn == BLACK) and "黑棋走" or "白棋走"
end

-- ------------------------------ 设备上的这一页 ------------------------------

local page = ui.page()
local cv = ui.canvas(page, N * CELL, N * CELL)
local info = ui.label(page, "")

local function draw()
    cv:fill(0xE4C089)                                   -- 木色底

    for i = 0, N - 1 do
        local p = i * CELL + CELL / 2
        cv:line(p, CELL / 2, p, N * CELL - CELL / 2, 0x9A7644)
        cv:line(CELL / 2, p, N * CELL - CELL / 2, p, 0x9A7644)
    end

    for y = 1, N do
        for x = 1, N do
            local v = board[y][x]
            if v ~= 0 then
                cv:circle(x * CELL - CELL / 2, y * CELL - CELL / 2, CELL / 2 - 1,
                          (v == BLACK) and 0x1C1C1C or 0xF7F7F7)
            end
        end
    end

    if last ~= nil then
        cv:circle(last.x * CELL - CELL / 2, last.y * CELL - CELL / 2, 1.5, 0xE5534B)
    end
end

local function update_info()
    local url = web.url()
    local text = turn_text() .. " · " .. #history .. " 手"
    if url then
        text = text .. " · " .. url
    else
        text = text .. " · 未联网"
    end
    info:set_text(text)
end

-- ------------------------------ 网页 ------------------------------

local PAGE = [==[
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>五子棋</title>
<style>
  :root { --bg:#0e1116; --card:#1a2029; --line:#262d38; --fg:#e6edf3; --dim:#9aa7b4; --accent:#2f81f7; --ok:#3fb950; }
  @media (prefers-color-scheme: light) {
    :root { --bg:#f2f4f7; --card:#fff; --line:#dce2ea; --fg:#1b222c; --dim:#5a6673; }
  }
  * { box-sizing: border-box; }
  body { margin:0; min-height:100vh; display:flex; flex-direction:column; align-items:center;
         gap:16px; padding:20px 14px 36px; background:var(--bg); color:var(--fg);
         font:15px/1.5 -apple-system,"Segoe UI","Microsoft YaHei","Noto Sans SC",Roboto,sans-serif; }
  h1 { margin:0; font-size:19px; }
  .bar { display:flex; align-items:center; gap:14px; background:var(--card); border:1px solid var(--line);
         border-radius:12px; padding:10px 16px; box-shadow:0 1px 2px #0003; }
  .turn { display:flex; align-items:center; gap:8px; font-weight:600; }
  .dot { width:14px; height:14px; border-radius:50%; border:1px solid #0006; background:#111; }
  .dot.w { background:#f7f7f7; }
  .sub { color:var(--dim); font-size:13px; }
  .btns { display:flex; gap:8px; }
  button { appearance:none; font:inherit; padding:7px 14px; border-radius:9px; cursor:pointer;
           background:var(--card); color:var(--fg); border:1px solid var(--line); }
  button:hover { border-color:var(--accent); color:var(--accent); }
  canvas { border-radius:12px; box-shadow:0 6px 24px #0005; touch-action:manipulation;
           max-width:100%; height:auto; cursor:pointer; }
  .msg { min-height:22px; font-size:14px; color:var(--ok); }
</style>
</head>
<body>
  <h1>五子棋</h1>
  <div class="bar">
    <span class="turn"><i class="dot" id="dot"></i><span id="turn">黑棋走</span></span>
    <span class="sub" id="moves">0 手</span>
    <span class="btns">
      <button id="undo">悔棋</button>
      <button id="reset">重新开始</button>
    </span>
  </div>
  <canvas id="board" width="510" height="510"></canvas>
  <div class="msg" id="msg"></div>

<script>
"use strict";
const N = 15, CELL = 34, PAD = CELL / 2;
const cv = document.getElementById("board");
const g = cv.getContext("2d");
let state = null, busy = false;

function draw() {
  /* 木色底 + 网格 */
  g.fillStyle = "#e4c089";
  g.fillRect(0, 0, cv.width, cv.height);
  g.strokeStyle = "#9a7644";
  g.lineWidth = 1;
  for (let i = 0; i < N; i++) {
    const p = PAD + i * CELL;
    g.beginPath(); g.moveTo(PAD, p); g.lineTo(cv.width - PAD, p); g.stroke();
    g.beginPath(); g.moveTo(p, PAD); g.lineTo(p, cv.height - PAD); g.stroke();
  }
  /* 天元与星位 */
  for (const [sx, sy] of [[7,7],[3,3],[11,3],[3,11],[11,11]]) {
    g.beginPath();
    g.arc(PAD + (sx - 1) * CELL, PAD + (sy - 1) * CELL, 3, 0, Math.PI * 2);
    g.fillStyle = "#7d5c30"; g.fill();
  }
  if (!state) return;

  /* 棋子 */
  for (let i = 0; i < state.board.length; i++) {
    const v = state.board[i];
    if (v === "0") continue;
    const x = i % N, y = Math.floor(i / N);
    const cx = PAD + x * CELL, cy = PAD + y * CELL;
    g.beginPath(); g.arc(cx, cy, CELL * 0.44, 0, Math.PI * 2);
    if (v === "1") {
      const grd = g.createRadialGradient(cx - 4, cy - 5, 2, cx, cy, CELL * 0.5);
      grd.addColorStop(0, "#555"); grd.addColorStop(1, "#111");
      g.fillStyle = grd;
    } else {
      const grd = g.createRadialGradient(cx - 4, cy - 5, 2, cx, cy, CELL * 0.5);
      grd.addColorStop(0, "#fff"); grd.addColorStop(1, "#d8d8d8");
      g.fillStyle = grd;
    }
    g.fill();
    g.strokeStyle = "#0004"; g.stroke();
  }

  /* 最后一手 */
  if (state.last >= 0) {
    const x = state.last % N, y = Math.floor(state.last / N);
    g.beginPath(); g.arc(PAD + x * CELL, PAD + y * CELL, 4, 0, Math.PI * 2);
    g.fillStyle = "#e5534b"; g.fill();
  }
}

function render() {
  if (!state) return;
  const black = state.turn === 1;
  document.getElementById("dot").className = "dot" + (black ? "" : " w");
  document.getElementById("turn").textContent =
    state.winner === 1 ? "黑棋胜" : state.winner === 2 ? "白棋胜" : (black ? "黑棋走" : "白棋走");
  document.getElementById("moves").textContent = state.moves + " 手";
  draw();
}

async function api(path, method) {
  const r = await fetch(path, { method: method || "POST" });
  return await r.json();
}

async function refresh() {
  if (busy) return;
  try {
    const j = await fetch("/s/state", { cache: "no-store" });
    state = await j.json();
    render();
  } catch (e) { /* 设备忙或断线：下一轮再试 */ }
}

cv.addEventListener("click", async (ev) => {
  if (!state || busy) return;
  const r = cv.getBoundingClientRect();
  const scale = cv.width / r.width;
  const px = (ev.clientX - r.left) * scale, py = (ev.clientY - r.top) * scale;
  const x = Math.round((px - PAD) / CELL) + 1, y = Math.round((py - PAD) / CELL) + 1;
  if (x < 1 || x > N || y < 1 || y > N) return;

  busy = true;
  try {
    const res = await api("/s/move?x=" + x + "&y=" + y);
    if (res.state) { state = res.state; render(); }
    document.getElementById("msg").textContent = res.ok ? "" : (res.err || "");
  } catch (e) {
    document.getElementById("msg").textContent = "设备没响应，稍后再试";
  }
  busy = false;
});

document.getElementById("reset").onclick = async () => {
  busy = true;
  try { state = await api("/s/reset"); render(); document.getElementById("msg").textContent = ""; }
  catch (e) { document.getElementById("msg").textContent = "设备没响应"; }
  busy = false;
};

document.getElementById("undo").onclick = async () => {
  busy = true;
  try {
    const res = await api("/s/undo");
    if (res.state) { state = res.state; render(); }
  } catch (e) { document.getElementById("msg").textContent = "设备没响应"; }
  busy = false;
};

setInterval(refresh, 700);
refresh();
</script>
</body>
</html>
]==]

web.get("/", function()
    return "text/html; charset=utf-8", PAGE
end)

web.get("/state", function()
    return "application/json; charset=utf-8", json.encode(state())
end)

web.post("/move", function(req)
    local x = tonumber(req.query.x)
    local y = tonumber(req.query.y)
    if x == nil or y == nil then
        return 400, "application/json; charset=utf-8", json.encode({ ok = false, err = "缺少 x / y" })
    end

    local ok, err = place(math.floor(x), math.floor(y))
    return "application/json; charset=utf-8",
           json.encode({ ok = ok and true or false, err = err or "", state = state() })
end)

web.post("/reset", function()
    reset()
    return "application/json; charset=utf-8", json.encode(state())
end)

web.post("/undo", function()
    undo()
    return "application/json; charset=utf-8", json.encode({ ok = true, state = state() })
end)

-- ------------------------------ 跑起来 ------------------------------

reset()
draw()
update_info()

timer.every(500, function()
    draw()
    update_info()
end)

do
    local url = web.url()
    if url then
        print("网页地址：" .. url)
        ui.toast("浏览器打开 " .. url, 5000)
    else
        print("还没连上 Wi-Fi：连上后浏览器打开 http://<设备IP>/s")
        ui.toast("先连上 Wi-Fi 再开网页", 4000)
    end
end
