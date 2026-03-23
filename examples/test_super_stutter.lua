-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")

print("--- Super Stutter Effect Test ---")

local ctx = noise.new(0.5, 999)
ctx:set_bit_depth(8)

-- Define a simple pattern
local pattern = {
    loop = true,
    {dur=0.2, amp=0.8, start=10.0, ends=2.0, pan=0.0},
    {dur=0.2, amp=0.0, start=1.0, ends=1.0},
    {dur=0.2, amp=0.6, start=1.0, ends=5.0, pan=0.5},
    {dur=0.2, amp=0.0, start=1.0, ends=1.0},
}
ctx:sequence(pattern)

print("Playing sequence...")
ctx:play_async(44100, 2, 8)

-- Wait a bit for the sequence to establish
os.execute("sleep 2")

print("Triggering Super Stutter (2 second loop of 20Hz)...")
-- 20Hz = 50ms loop. Lasts for 2 seconds.
-- Captures the current sound and freezes it into a loop
ctx:super_stutter(2.0, 20.0)

os.execute("sleep 3")

print("Triggering Fast Super Stutter (1 second loop of 60Hz)...")
ctx:super_stutter(1.0, 60.0)

os.execute("sleep 3")
print("Done.")