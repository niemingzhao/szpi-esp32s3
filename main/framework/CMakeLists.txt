idf_component_register(
    SRC_DIRS src assets
    INCLUDE_DIRS include
    # 内置示例脚本以真实文件放在 assets/scripts/，构建期嵌进固件。
    # 符号名按 IDF 规则只取文件名（counter.lua → _binary_counter_lua_start）；
    # api-reference.txt 用 ASCII 名（非 ASCII 会变成一串下划线），释放到卡上再改中文名。
    EMBED_FILES
        "assets/scripts/counter.lua"
        "assets/scripts/clock.lua"
        "assets/scripts/snake.lua"
        "assets/scripts/io.lua"
        "assets/scripts/gomoku.lua"
        "assets/scripts/api-reference.txt"
    REQUIRES services peripherals lvgl esp_lvgl_port lua cjson esp_timer
)

# 必须与 Lua 核心的 LUA_32BITS=1 保持一致：espressif/lua 只用 PRIVATE 编译这个宏，
# 头文件注入对 lua.h 的引号包含无效；数值类型不一致时 luaL_checkversion() 会直接 abort。
target_compile_definitions(${COMPONENT_LIB} PUBLIC LUA_32BITS=1)
