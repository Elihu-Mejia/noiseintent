local noise = require("noiseintent")

print("--- Multiple Async Sequences Test (Polyrhythm) ---")

-- Track 1: Heavy Kick (Center)
local kick = noise.new(0.6, 100)
kick:set_bit_depth(12) -- Slight crunch
local kick_pat = {
    loop = true,
    {dur=0.1, amp=0.9, start=50.0, ends=5.0}, -- Impact (Sweep down)
    {dur=0.3, amp=0.0, start=1.0, ends=1.0},  -- Rest
    {dur=0.1, amp=0.6, start=20.0, ends=5.0, prob=0.3}, -- Occasional Ghost kick
    {dur=0.3, amp=0.0, start=1.0, ends=1.0},  -- Rest
}
kick:sequence(kick_pat)

-- Track 2: Glitch Hats (Stereo Panned)
-- Loop length: 0.4s (2x speed of kick)
local hats = noise.new(0.3, 200)
hats:set_bit_depth(6) -- Gritty
local hat_pat = {
    loop = true,
    {dur=0.05, amp=0.7, start=1.0, ends=5.0, pan=-0.6, prob=0.9}, -- Left hit
    {dur=0.05, amp=0.0, start=1.0, ends=1.0},
    {dur=0.05, amp=0.5, start=1.0, ends=10.0, pan=0.6, prob=0.6}, -- Right hit
    {dur=0.05, amp=0.0, start=1.0, ends=1.0},
    {dur=0.05, amp=0.7, start=2.0, ends=2.0, stutter=60.0, pan=0.0}, -- Center roll
    {dur=0.15, amp=0.0, start=1.0, ends=1.0},
}
hats:sequence(hat_pat)

-- Track 3: Drifting Texture (Phasing)
-- Loop length: 0.7s (Will drift against the 0.8s kick pattern)
local texture = noise.new(0.2, 300)
texture:set_fade(2.0, 2.0) -- Smooth entry/exit
local tex_pat = {
    loop = true,
    {dur=0.2, amp=0.5, start=50.0, ends=80.0, pan=0.5}, -- High rising drone
    {dur=0.1, amp=0.0, start=1.0, ends=1.0},
    {dur=0.1, amp=0.6, start=5.0, ends=5.0, stutter=30.0, pan=-0.5, prob=0.5}, -- Stutter burst
    {dur=0.3, amp=0.0, start=1.0, ends=1.0},
}
texture:sequence(tex_pat)

print("Starting 3 independent layers (12 seconds)...")

-- Create a sync barrier for 3 threads
local sync = noise.create_sync(3)
kick:play_async(44100, 2, 12, sync)
hats:play_async(44100, 2, 12, sync)
texture:play_async(44100, 2, 12, sync)

-- Keep main thread alive while background threads play
os.execute("sleep 12")

print("Done.")