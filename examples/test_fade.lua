local noise = require("noiseintent")

-- A simple sleep function for demonstration
local function sleep(s)
    os.execute("sleep " .. tonumber(s))
end

print("--- Testing Asynchronous Fade Effects ---")

-- Context 1: 5 seconds with a 2-second fade-in
print("Starting a 5-second sound with a 2-second fade-in...")
local ctx1 = noise.new(0.2, 12345)
ctx1:set_fade(2.0, 0.0) -- 2s fade-in, 0s fade-out
ctx1:play_async(44100, 2, 5)

-- Context 2: 5 seconds with a 2-second fade-out, slightly different sound
print("Starting a 5-second sound with a 2-second fade-out (muffled)...")
local ctx2 = noise.new(0.2, 54321)
ctx2:set_lpf_alpha(0.1) -- Make it sound different
ctx2:set_fade(0.0, 2.0) -- 0s fade-in, 2s fade-out
ctx2:play_async(44100, 2, 5)

print("\nBoth sounds are playing asynchronously. Waiting for 5 seconds...")
sleep(5)

print("--- Test Complete ---")
