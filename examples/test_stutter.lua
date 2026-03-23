-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")

print("--- Stutter Effect Test ---")

local ctx = noise.new(0.4, 333)

-- Add some grit so the repeated buffer has texture
ctx:set_bit_depth(6)

-- Define a rhythmic pattern with varying stutter speeds
-- 'stutter': frequency in Hz (repeats a buffer segment of 1/freq seconds)

local pattern = {
    loop = true,

    -- 1. Base Hit (No stutter)
    {dur=0.2, amp=0.8, start=20.0, ends=1.0, pan=0.0},
    
    -- 2. "Machine Gun" Stutter (50Hz = 20ms loop)
    -- High decimation makes it sound metallic
    {dur=0.2, amp=0.8, start=10.0, ends=10.0, stutter=50.0, pan=-0.5},
    
    -- 3. Slow "Roll" Stutter (15Hz = 66ms loop)
    {dur=0.4, amp=0.8, start=5.0, ends=1.0, stutter=15.0, pan=0.5},
    
    -- 4. Rising Pitch Stutter sequence
    -- By increasing stutter freq, we shorten the loop, raising the perceived pitch
    {dur=0.1, amp=0.6, start=2.0, ends=2.0, stutter=20.0},
    {dur=0.1, amp=0.6, start=2.0, ends=2.0, stutter=30.0},
    {dur=0.1, amp=0.6, start=2.0, ends=2.0, stutter=40.0},
    {dur=0.1, amp=0.6, start=2.0, ends=2.0, stutter=60.0},
    
    {dur=0.2, amp=0.0, start=1.0, ends=1.0}, -- Silence
}

print("Playing stutter sequence... (Ctrl+C to stop)")
ctx:sequence(pattern)
ctx:play(44100, 2, 8)

print("Done.")