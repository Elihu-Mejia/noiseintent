local M = {}

-- Creates a Markov chain generator
-- corpus: The source string to learn from
-- order: The state size (n-gram size). Higher = more structured, Lower = more random. Default 2.
function M.new(corpus, order)
    local self = {
        chain = {},
        order = order or 2,
        current = nil
    }

    -- Training
    -- We treat the string as circular to ensure no dead ends
    for i = 1, #corpus do
        local gram = ""
        for j = 0, self.order - 1 do
            local idx = ((i + j - 1) % #corpus) + 1
            gram = gram .. corpus:sub(idx, idx)
        end

        local next_idx = ((i + self.order - 1) % #corpus) + 1
        local next_char = corpus:sub(next_idx, next_idx)

        if not self.chain[gram] then self.chain[gram] = {} end
        table.insert(self.chain[gram], next_char)
    end

    -- Set initial state
    self.current = corpus:sub(1, self.order)

    -- Generator method
    function self:generate(length)
        local result = ""
        for i = 1, length do
            local options = self.chain[self.current]
            if not options or #options == 0 then
                -- Fallback (shouldn't happen with circular training, but safe)
                self.current = corpus:sub(1, self.order)
                options = self.chain[self.current]
            end

            local char = options[math.random(#options)]
            result = result .. char
            self.current = self.current:sub(2) .. char
        end
        return result
    end

    return self
end

return M