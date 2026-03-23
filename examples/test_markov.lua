-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")
local markov = require("noise_markov")

print("--- Markov Chain Generative Test ---")

-- Seed random
math.randomseed(os.time())

local ctx = noise.new(0.5, os.time())
ctx:set_bit_depth(8)

-- The Corpus: A string containing various behaviors we want to remix
-- Digits (rhythm), '.' (rest), '!' (stutter), '+' (accel), '/' (decel), 'A-Z' (noise)
local source_material = "1..1.. 2..2.. !A!A + 1.1.1.1. / B_B_ C... 9... "

print("Training on corpus: " .. source_material)

-- Order 2 preserves short local structures (like '1..' or '!A')
local gen = markov.new(source_material, 2)

print("Generating infinite evolving sequence...")
print("(Ctrl+C to stop)")

-- Playback loop
-- We generate a chunk, schedule it, play it, and repeat.
local chunk_len = 16
local chunk_duration = 0.15 * chunk_len -- rough estimate

while true do
    local seq = gen:generate(chunk_len)
    io.write("\rCurrent Chunk: " .. seq .. "   ")
    io.flush()
    
    ctx:sequence_string(seq, 0.12, false) -- false = don't loop, play once
    ctx:play(44100, 2, 20) -- Duration is limited by sequence length anyway
    
    -- Note: play() is blocking here, which is perfect for this simple loop.
    -- For async seamless chaining, we'd need double-buffering or the async API with callbacks.
end
