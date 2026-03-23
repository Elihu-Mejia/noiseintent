local noise = require("noiseintent")

print("--- String Abstraction Test (C Implementation) ---")

local ctx = noise.new(0.5, 9999)
ctx:set_bit_depth(8)

-- Define a sequence using abstract symbols
-- Digits=Beats, Uppercase=Noise, Symbols=Pan/Stutter
local score = "0.0. 1.1. <<>> !!!! A_B_C_ *-*-* ????"

print("Score: " .. score)

-- ctx:sequence_string(str, base_dur, loop)
ctx:sequence_string(score, 0.12, true)

print("Playing...")
ctx:play(44100, 2, 8)

print("Done.")