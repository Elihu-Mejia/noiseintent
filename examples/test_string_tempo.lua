-- Add current directory to cpath to find noiseintent.so
package.cpath = package.cpath .. ";./?.so"

local noise = require("noiseintent")

print("--- String Sequence Tempo Change Test ---")

local ctx = noise.new(0.5, 12345)
ctx:set_bit_depth(8)

-- Pattern breakdown:
-- "1.1." : Normal speed
-- "+"    : Double speed (duration /= 2)
-- "1.1." : Fast
-- "+"    : Quadruple speed
-- "1.1.1.1." : Very Fast
-- "/"    : Back to Double speed
-- "/"    : Back to Normal speed
-- "/"    : Half speed (Slow)
-- "1....." : Slow hits

local score = "1.1. + 1.1. + 1.1.1.1. / / / 1....."

print("Score: " .. score)
print("Legend: '+' = Accelerate (2x), '/' = Decelerate (0.5x)")

-- ctx:sequence_string(str, base_dur, loop)
ctx:sequence_string(score, 0.4, true)

print("Playing for 10 seconds...")
ctx:play(44100, 2, 20)

print("Done.")