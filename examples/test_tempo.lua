-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")

print("--- Tempo Acceleration Test ---")

local ctx = noise.new(0.5, 555)
ctx:set_bit_depth(8) -- Add some grit

-- Define a simple rhythmic pattern (Standard Techno-ish beat)
local pattern = {
    loop = true,
    {dur=0.2, amp=0.8, start=30.0, ends=5.0, prob=1.0},  -- Kick
    {dur=0.2, amp=0.0, start=1.0,  ends=1.0},            -- Rest
    {dur=0.2, amp=0.5, start=1.0,  ends=10.0, prob=1.0}, -- Hi-hat / Noise
    {dur=0.2, amp=0.0, start=1.0,  ends=1.0},            -- Rest
}

ctx:sequence(pattern)

-- Start playing asynchronously for 10 seconds
print("Starting playback at Tempo 1.0...")
ctx:play_async(44100, 2, 12) 

-- Accelerate from 1.0x to 4.0x over ~4 seconds
print("Accelerating...")
for i = 1, 40 do
    local t = 1.0 + (3.0 * (i / 40.0))
    ctx:set_tempo(t)
    os.execute("sleep 0.1")
end

-- Decelerate from 4.0x to 0.5x over ~4 seconds
print("Decelerating...")
for i = 1, 40 do
    local t = 4.0 - (3.5 * (i / 40.0))
    ctx:set_tempo(t)
    os.execute("sleep 0.1")
end

print("Done.")