local noise = require("noiseintent")

-- Create a new noise context
-- Amplitude: 0.2 (low volume to be safe), Seed: 12345
local ctx = noise.new(0.2, 12345)

print("Test 1: Pure White Noise (Alpha = 1.0) - 2 Seconds")
ctx:set_lpf_alpha(1.0)
if not ctx:play(44100, 2, 2) then
    print("Error: Could not play audio.")
end

print("Test 2: Moderate Low-Pass Filter (Alpha = 0.2) - 2 Seconds")
-- Lower alpha means the previous sample has more weight -> smoother signal
ctx:set_lpf_alpha(0.2)
ctx:play(44100, 2, 2)

print("Test 3: Heavy Low-Pass Filter (Alpha = 0.05) - 2 Seconds")
-- Very low alpha results in a bass-heavy, muffled sound
ctx:set_lpf_alpha(0.05)
ctx:play(44100, 2, 2)

print("Done.")
