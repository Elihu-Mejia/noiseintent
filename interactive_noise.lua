-- Ensure modules can be found relative to current directory or macos subdirectory
-- Detect OS and configure paths
local handle = io.popen("uname -s")
local os_name = handle and handle:read("*l") or "Unknown"
if handle then handle:close() end

package.cpath = package.cpath .. ";./?.so"
package.path = package.path .. ";./?.lua"

if os_name == "Darwin" then
    package.cpath = package.cpath .. ";./macos/?.so"
    package.path = package.path .. ";./macos/?.lua"
elseif os_name == "Linux" then
    package.cpath = package.cpath .. ";./linux/?.so"
    package.path = package.path .. ";./linux/?.lua"
end

local markov = require("noise_markov")
local noise

-- Robust loading: Handle missing or corrupt C module by falling back to a mock
local status, lib = pcall(require, "noiseintent")
if status then
    noise = lib
else
    io.stderr:write("[BOOT ERROR] Unable to load 'noiseintent' core: " .. tostring(lib) .. "\n")
    io.stderr:write("[SYSTEM] INITIALIZING DUMMY INTERFACE. AUDIO WILL BE MUTED.\n\n")
    
    noise = {
        create_sync = function() return {} end,
        new = function(amp, seed)
            return {
                sequence_string = function() end,
                play_async = function() end,
                set_amplitude = function() end,
                set_tempo = function() end,
                set_auto_glitch = function() end,
                set_robot = function() end,
                set_delay = function() end,
                stop = function() end,
            }
        end
    }
end

-- Terminal state management
local function set_raw_mode(enable)
    if enable then
        os.execute("stty -icanon -echo min 0")
    else
        os.execute("stty sane")
    end
end

local function clear_screen()
    -- ANSI: Clear screen and home cursor
    io.write("\027[H\027[2J")
end

local M = {}

function M.run()
    math.randomseed(os.time())
    io.write("[SYSTEM BOOT] SELECT ACTIVE GN DRIVES (DEFAULT 1): ")
    io.flush()
    local count = tonumber(io.read()) or 1
    if count < 1 then count = 1 end
    
    local ctxs = {}
    local sync
    local funnels = {}
    if count > 1 then
        sync = noise.create_sync(count)
    end
    
    -- Default Sequence
    local seq_str = "1.. 2.. 1! 3.."
    local base_dur = 0.15
    local states = {}

    -- Visualizer helper: maps sequence characters to ASCII blocks for tactical display
    local function get_viz(seq, width)
        if not seq or #seq == 0 then return string.rep(" ", width) end
        local out = ""
        for j = 1, width do
            local c = seq:sub(((j-1) % #seq) + 1, ((j-1) % #seq) + 1)
            if c:match("%d") then out = out .. "█"      -- Heavy Impact (Digits)
            elseif c:match("%u") then out = out .. "▓" -- High Power Thrust (A-Z)
            elseif c:match("%l") then out = out .. "▒" -- Low Power Thrust (a-z)
            elseif c == "!" then out = out .. "⚡"      -- Stutter / Discharge
            elseif c == "." or c == "_" or c == "-" or c == " " then out = out .. " "
            else out = out .. "░" end                  -- Other modulation
        end
        return out
    end

    for i = 1, count do
        -- Create context with random seed
        local ctx = noise.new(0.5 / math.sqrt(count), os.time() + i * 999)
        ctx:sequence_string(seq_str, base_dur, true)
        table.insert(ctxs, ctx)
        table.insert(states, {seq=seq_str, vol=0.5 / math.sqrt(count), tempo=1.0, robot_freq=0.0, robot_depth=0.0, delay_t=0.0, delay_f=0.0, delay_m=0.0})
    end

    print("--- GUNDAM SOUND SYSTEM ONLINE // UNITS: " .. count .. " ---")
    print("[STATUS] INFINITE CYCLE ENGAGED. AWAITING TACTICAL DATA.")
    if count > 1 then
        print("[INFO] TARGET LOCK: USE 'N:' TO ISOLATE DRIVE (E.G. '1: vol 0.9'). DEFAULT: GLOBAL.")
    end
    print("[LEGEND] [+] BOOST [/] BRAKE [.] IDLE [0-9] IMPACT [A-Z] THRUST")
    print("[OPCODES] 'quit', 'list', 'viz', 'monitor <n>', 'vol <val>', 'tempo <val>', 'seq <str>', 'markov <data>', 'evolve', 'funnel <cnt>', 'autoglitch <freq>', 'robot <freq> <depth>', 'delay <time> <fb> <mix>', 'transam <dur>', 'save <file>', 'load <file>', 'minovsky'")
    print("[ACTIVE PATTERN] " .. seq_str)

    local monitoring = false
    local cmd_buffer = ""
    local frame_count = 0

    -- Start async playback (approx 1000 hours)
    for _, ctx in ipairs(ctxs) do
        if sync then
            ctx:play_async(44100, 2, 3600000, sync)
        else
            ctx:play_async(44100, 2, 3600000)
        end
    end

    set_raw_mode(true)

    while true do
        -- Cleanup funnels
        local now = os.time()
        for i = #funnels, 1, -1 do
            if funnels[i].expire < now then
                table.remove(funnels, i)
            end
        end

        for i, s in ipairs(states) do
            if s.trans_am_expire and s.trans_am_expire < now then
                ctxs[i]:set_amplitude(s.vol)
                ctxs[i]:set_tempo(s.tempo)
                s.trans_am_expire = nil
                print("[DRIVE "..i.."] TRANS-AM DISENGAGED. PARTICLE EMISSION NORMAL.")
            end
        end

        -- 1. Handle Non-blocking Input
        local char = io.read(1)
        local input_ready = false
        if char then
            local b = string.byte(char)
            if b == 10 or b == 13 then -- Enter
                input_ready = true
            elseif b == 127 or b == 8 then -- Backspace
                cmd_buffer = cmd_buffer:sub(1, -2)
            elseif b >= 32 and b <= 126 then
                cmd_buffer = cmd_buffer .. char
            end
        end

        -- 2. Render Loop (Only if monitoring is active)
        if monitoring then
            frame_count = frame_count + 1
            clear_screen()
            print("=== GN DRIVE TACTICAL MONITOR [LIVE FEED] ===")
            for i, s in ipairs(states) do
                local offset = math.floor(frame_count * s.tempo) % #s.seq
                local shifted_seq = s.seq:sub(offset + 1) .. s.seq:sub(1, offset)
                print(string.format("DRIVE %02d [%s] LVL:%.2f", i, get_viz(shifted_seq, 60), s.vol))
            end
            print("\n[COMMAND]: " .. cmd_buffer .. "_")
        elseif input_ready or (char and #char > 0) then
            -- Simple feedback if not monitoring
            io.write("\r> " .. cmd_buffer .. "  \b")
        end
        io.flush()
        
        -- 3. Process Command
        local input = input_ready and cmd_buffer or ""
        if input == "quit" or input == "exit" then
            break
        end
        if input_ready then cmd_buffer = "" end -- Clear buffer after processing

        local target_idx = nil
        local cmd_str = input

                    -- Check for target syntax
                    local prefix, rest = input:match("^(%d+):%s*(.*)")
                    if prefix then
                        target_idx = tonumber(prefix)
                        cmd_str = rest
                    end

        local function apply(fn)
            if target_idx then
                if ctxs[target_idx] then
                    fn(ctxs[target_idx], target_idx)
                else
                    print("[ERROR] TARGET DRIVE " .. target_idx .. " NOT FOUND.")
                end
            else
                for i, c in ipairs(ctxs) do
                    fn(c, i)
                end
            end
        end

        if cmd_str:match("^vol") then
            local v = tonumber(cmd_str:match("^vol%s+(.*)"))
            if v then
                apply(function(c, i) c:set_amplitude(v); states[i].vol = v; print("[DRIVE "..i.."] OUTPUT LEVEL: " .. v) end)
            end
        elseif cmd_str:match("^tempo") then
            local t = tonumber(cmd_str:match("^tempo%s+(.*)"))
            if t then 
                apply(function(c, i) 
                    c:set_tempo(t); states[i].tempo = t; print("[DRIVE "..i.."] CLOCK RATE: " .. t) 
                    if t == 3 then
                        print("[DRIVE "..i.."] CHAR MODE ACTIVATED. PERFORMANCE 300%.")
                    end
                end)
            end
        elseif cmd_str:match("^seq") then
            local s = cmd_str:match("^seq%s+(.*)")
            if s then
                apply(function(c, i) c:sequence_string(s, base_dur, true); states[i].seq = s; print("[DRIVE "..i.."] SEQUENCE RECONFIGURED.") end)
            end
        elseif cmd_str:match("^markov") then
            local corpus = cmd_str:match("^markov%s+(.*)")
            if corpus and #corpus > 0 then
                apply(function(c, i)
                    local m = markov.new(corpus, 2)
                    states[i].markov = m
                    states[i].corpus = corpus
                    local s = m:generate(32)
                    c:sequence_string(s, base_dur, true)
                    states[i].seq = s
                    print("[DRIVE "..i.."] NEURAL LINK ESTABLISHED: " .. s)
                end)
            end
        elseif cmd_str == "evolve" then
            apply(function(c, i)
                if states[i].markov then
                    print([[
      \   /
    -- (X) --  [NEWTYPE FLASH]
      /   \]])
                    local s = states[i].markov:generate(32)
                    c:sequence_string(s, base_dur, true)
                    states[i].seq = s
                    print("[DRIVE "..i.."] PATTERN MUTATION: " .. s)
                end
            end)
        elseif cmd_str:match("^autoglitch") then
            local f = tonumber(cmd_str:match("^autoglitch%s+(.*)"))
            if f then
                apply(function(c, i) c:set_auto_glitch(f); print("[DRIVE "..i.."] AUTO-INTERFERENCE RATE: " .. f .. " Hz") end)
            end
        elseif cmd_str:match("^robot") then
            local args = cmd_str:match("^robot%s+(.*)")
            if args then
                local f_str, d_str = args:match("([%d%.]+)%s*([%d%.]*)")
                local f = tonumber(f_str)
                local d = tonumber(d_str) or 1.0
                if f then
                    apply(function(c, i) 
                        c:set_robot(f, d)
                        states[i].robot_freq = f
                        states[i].robot_depth = d
                        print(string.format("[DRIVE %d] ROBOTIC MODULATION: %.1f Hz (Depth: %.2f)", i, f, d))
                    end)
                end
            end
        elseif cmd_str:match("^delay") then
            local args = cmd_str:match("^delay%s+(.*)")
            if args then
                local t_str, f_str, m_str = args:match("([%d%.]+)%s*([%d%.]*)%s*([%d%.]*)")
                local t = tonumber(t_str)
                local f = tonumber(f_str) or 0.4
                local m = tonumber(m_str) or 0.5
                if t then
                    apply(function(c, i) 
                        c:set_delay(t, f, m)
                        states[i].delay_t = t; states[i].delay_f = f; states[i].delay_m = m
                        print(string.format("[DRIVE %d] ECHO SYSTEM: %.2fs Time | %.2f FB | %.2f Mix", i, t, f, m))
                    end)
                end
            end
        elseif cmd_str:match("^transam") then
            local dur = tonumber(cmd_str:match("^transam%s+(.*)")) or 10
            print("[SYSTEM ALERT] TRANS-AM SYSTEM ACTIVATED. MAX OUTPUT FOR " .. dur .. " SECONDS.")
            apply(function(c, i) 
                c:set_amplitude(0.9) 
                c:set_tempo(3.0) 
                states[i].trans_am_expire = os.time() + dur 
            end)
        elseif cmd_str == "minovsky" then
            print("[WARNING] MINOVSKY PARTICLE DISPERSION. RADAR JAMMED.")
            apply(function(c, i) 
                c:set_amplitude(0.0) 
                states[i].vol = 0.0 
            end)
            print("[SYSTEM] VISUAL CONTACT ONLY. DRIVES MUTED.")
            for i=1, 5 do print(" ... ... ... ") os.execute("sleep 0.1") end
            break
        elseif cmd_str:match("^funnel") then
            local cnt = tonumber(cmd_str:match("^funnel%s+(.*)")) or 5
            print("[TACTICAL] DEPLOYING " .. cnt .. " REMOTE FUNNELS...")
            for i = 1, cnt do
                local dur = math.random(3, 8)
                local f_ctx = noise.new(0.25, os.time() + i * 777 + math.random(10000))
                
                -- Generate random sequence
                local chars = "0123456789ABCDEF....+/"
                local seq = ""
                for j = 1, math.random(8, 16) do
                    local r = math.random(#chars)
                    seq = seq .. chars:sub(r, r)
                end
                
                f_ctx:sequence_string(seq, 0.1, true)
                f_ctx:play_async(44100, 2, dur)
                table.insert(funnels, {ctx = f_ctx, expire = os.time() + dur + 1})
            end
        elseif cmd_str:match("^save") then
            local fname = cmd_str:match("^save%s+(.*)")
            if fname then
                local f = io.open(fname, "w")
                if f then
                    f:write("return {\n")
                    for _, s in ipairs(states) do
                        f:write(string.format("  {vol=%f, tempo=%f, seq=%q, corpus=%q, robot_f=%f, robot_d=%f, delay_t=%f, delay_f=%f, delay_m=%f},\n", 
                            s.vol, s.tempo, s.seq, s.corpus or "", s.robot_freq or 0, s.robot_depth or 0, s.delay_t or 0, s.delay_f or 0, s.delay_m or 0))
                    end
                    f:write("}\n")
                    f:close()
                    print("[SYSTEM] TACTICAL DATA DUMPED TO " .. fname)
                else
                    print("[ERROR] UNABLE TO WRITE TO " .. fname)
                end
            end
        elseif cmd_str:match("^load") then
            local fname = cmd_str:match("^load%s+(.*)")
            if fname then
                local chunk, err = loadfile(fname)
                if chunk then
                    local success, loaded_states = pcall(chunk)
                    if success and type(loaded_states) == "table" then
                        for i, s in ipairs(loaded_states) do
                            if ctxs[i] then
                                if s.vol then ctxs[i]:set_amplitude(s.vol); states[i].vol = s.vol end
                                if s.tempo then ctxs[i]:set_tempo(s.tempo); states[i].tempo = s.tempo end
                                if s.seq then ctxs[i]:sequence_string(s.seq, base_dur, true); states[i].seq = s.seq end
                                if s.robot_f then 
                                    ctxs[i]:set_robot(s.robot_f, s.robot_d or 1.0)
                                    states[i].robot_freq = s.robot_f
                                    states[i].robot_depth = s.robot_d or 1.0
                                end
                                if s.delay_t then
                                    ctxs[i]:set_delay(s.delay_t, s.delay_f or 0.4, s.delay_m or 0.5)
                                    states[i].delay_t = s.delay_t
                                    states[i].delay_f = s.delay_f or 0.4
                                    states[i].delay_m = s.delay_m or 0.5
                                end
                                if s.corpus and #s.corpus > 0 then
                                    states[i].corpus = s.corpus
                                    states[i].markov = markov.new(s.corpus, 2)
                                end
                            end
                        end
                        print("[SYSTEM] TACTICAL DATA RELOADED FROM " .. fname)
                    else
                        print("[ERROR] CORRUPT DATA FILE.")
                    end
                else
                    print("[ERROR] LOAD FAILED: " .. err)
                end
            end
        elseif cmd_str == "list" then
            set_raw_mode(false) -- Restore for clean list printing
            print("")
            for i, s in ipairs(states) do
                local viz = get_viz(s.seq, 30)
                print(string.format("[DRIVE %d] LVL: %.2f | CLK: %.2f | [%s] | RAW: %s", i, s.vol, s.tempo, viz, s.seq))
            end
        elseif cmd_str == "viz" then
            print("\n" .. string.rep("=", 60))
            print("  GN-DRIVE TACTICAL STATUS REPORT")
            print(string.rep("=", 60))
            for i, s in ipairs(states) do
                local viz = get_viz(s.seq, 50)
                local power = math.floor(s.vol * 100)
                local bar = string.rep("■", math.floor(power/5)) .. string.rep(" ", 20 - math.floor(power/5))
                print(string.format(" UNIT %02d | POWER [%s] %03d%% | CLK: %.2f", i, bar, power, s.tempo))
                print(string.format(" SEQuence | %s", viz))
                local mods = ""
                if (s.robot_freq or 0) > 0 then mods = mods .. string.format("ROBOT:%.1fHz ", s.robot_freq) end
                if (s.delay_t or 0) > 0 then mods = mods .. string.format("DELAY:%.2fs ", s.delay_t) end
                if s.trans_am_expire then mods = mods .. "TRANS-AM " end
                if #mods > 0 then print(string.format(" MODuLATE | %s", mods)) end
                print(string.rep("-", 60))
            end
            print("")
            set_raw_mode(true)
        elseif cmd_str == "monitor" then
            monitoring = not monitoring
            if not monitoring then clear_screen() end
            print("[SYSTEM] MONITOR MODE: " .. (monitoring and "ENGAGED" or "DISENGAGED"))
        elseif input_ready and #cmd_str > 0 then
            -- Update sequence
            apply(function(c, i) c:sequence_string(cmd_str, base_dur, true); states[i].seq = cmd_str; print("[DRIVE "..i.."] SEQUENCE RECONFIGURED.") end)
        end

        os.execute("sleep 0.05") -- Control refresh rate/CPU usage
    end
    
    set_raw_mode(false)
    for _, ctx in ipairs(ctxs) do
        ctx:stop()
    end
    print("[SYSTEM OFFLINE] MISSION COMPLETE.")
end


-- Auto-run if executed directly
if arg and arg[0] and string.find(arg[0], "interactive_noise.lua") then
    M.run()
end

return M