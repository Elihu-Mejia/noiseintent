-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")

print("--- Lo-Fi Noise Generator Test ---")

-- Create context with 0.2 amplitude
local ctx = noise.new(0.2, 999)

print("1. Reference: Clean White Noise (2s)")
-- Ensure defaults
ctx:set_bit_depth(0)      -- 0 disables bit reduction
ctx:set_decimation(1.0)   -- 1.0 is normal rate
if not ctx:play(44100, 2, 2) then
    print("Error: Audio playback failed.")
end

print("2. Lo-Fi: 8-bit, 4x Decimation (2s)")
-- 8-bit audio adds a noise floor and grain
-- 4x decimation at 44.1kHz results in ~11kHz effective rate
ctx:set_bit_depth(8)
ctx:set_decimation(4.0)
ctx:play(44100, 2, 2)

print("3. Crushed: 4-bit, 16x Decimation (2s)")
-- 4-bit is extremely quantized
-- 16x decimation drops effective rate to ~2.7kHz (heavy aliasing)
ctx:set_bit_depth(4)
ctx:set_decimation(16.0)
ctx:play(44100, 2, 2)

print("4. Dynamic Sweep: Decimating from 1x to 50x (4s)")
ctx:set_bit_depth(8) -- Keep 8-bit base

-- Start the glitch sweep (4 seconds, from 1.0 to 50.0)
ctx:glitch(4.0, 1.0, 50.0)

ctx:play_async(44100, 2, 4)
os.execute("sleep 4") -- Wait for playback

print("Done.")