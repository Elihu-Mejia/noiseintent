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
                set_bit_depth = function() end,
                sequence_string = function() end,
                play_async = function() end,
                set_amplitude = function() end,
                set_tempo = function() end,
                set_auto_glitch = function() end,
                stop = function() end,
            }
        end
    }
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
    local seq_str = "1... 1..."
    local base_dur = 0.15
    local states = {}

    for i = 1, count do
        -- Create context with random seed
        local ctx = noise.new(0.5 / math.sqrt(count), os.time() + i * 999)
        ctx:set_bit_depth(8)
        ctx:sequence_string(seq_str, base_dur, true)
        table.insert(ctxs, ctx)
        table.insert(states, {seq=seq_str, vol=0.5 / math.sqrt(count), tempo=1.0})
    end

    print("--- GUNDAM SOUND SYSTEM ONLINE // UNITS: " .. count .. " ---")
    print("[STATUS] INFINITE CYCLE ENGAGED. AWAITING TACTICAL DATA.")
    if count > 1 then
        print("[INFO] TARGET LOCK: USE 'N:' TO ISOLATE DRIVE (E.G. '1: vol 0.9'). DEFAULT: GLOBAL.")
    end
    print("[LEGEND] [+] BOOST [/] BRAKE [.] IDLE [0-9] IMPACT [A-Z] THRUST")
    print("[OPCODES] 'quit', 'list', 'vol <val>', 'tempo <val>', 'seq <str>', 'markov <data>', 'evolve', 'funnel <cnt>', 'autoglitch <freq>', 'transam <dur>', 'save <file>', 'load <file>', 'minovsky'")
    print("[ACTIVE PATTERN] " .. seq_str)

    -- Start async playback (approx 1000 hours)
    for _, ctx in ipairs(ctxs) do
        if sync then
            ctx:play_async(44100, 2, 3600000, sync)
        else
            ctx:play_async(44100, 2, 3600000)
        end
    end

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

        io.write("> ")
        io.flush()
        local input = io.read()
        if not input then break end -- EOF
        
        input = input:match("^%s*(.-)%s*$") -- Trim
        
        if input == "quit" or input == "exit" then
            break
        end

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
                f_ctx:set_bit_depth(8)
                
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
                        f:write(string.format("  {vol=%f, tempo=%f, seq=%q, corpus=%q},\n", 
                            s.vol, s.tempo, s.seq, s.corpus or ""))
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
            for i, s in ipairs(states) do
                print(string.format("[DRIVE %d] LVL: %.2f | CLK: %.2f | SEQ: %s", i, s.vol, s.tempo, s.seq))
            end
        elseif #cmd_str > 0 then
            -- Update sequence
            apply(function(c, i) c:sequence_string(cmd_str, base_dur, true); states[i].seq = cmd_str; print("[DRIVE "..i.."] SEQUENCE RECONFIGURED.") end)
        end
    end
    
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