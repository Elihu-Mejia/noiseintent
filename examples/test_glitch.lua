-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")

print("--- Glitch Function Test ---")

local ctx = noise.new(0.2, 4242)

-- Base settings: 8-bit depth adds some grit to make the decimation more obvious
ctx:set_bit_depth(8) 

print("1. Glitch Sweep Up: 1.0 -> 50.0 (Decimation) over 2 seconds")
-- Starts clear, gets very crushed/robotic as sample rate drops effectively
ctx:glitch(2.0, 1.0, 50.0)
ctx:play(44100, 2, 2)

os.execute("sleep 0.5")

print("2. Glitch Sweep Down: 50.0 -> 1.0 (Decimation) over 2 seconds")
-- Starts crushed, resolves back to clear white noise
ctx:glitch(2.0, 50.0, 1.0)
ctx:play(44100, 2, 2)

print("3. Fast Glitch ('Laser' effect): 100.0 -> 1.0 over 0.5 seconds")
ctx:glitch(0.5, 100.0, 1.0)
ctx:play(44100, 2, 1)

print("Done.")
