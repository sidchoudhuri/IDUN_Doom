-- main.lua  –  Pi-side launcher for IDUN Doom
--
-- The IDUN shell runs this script (via the "l:" Lua device) when the
-- C64 app opens "l:.d/main.lua".
--
-- Responsibilities:
--   1. Create two named pipes that doom-idun uses for I/O.
--   2. Launch the doom-idun binary in the background.
--   3. Relay data between minisock (IDUN TTY ↔ C64) and the pipes.
--
-- Pipe layout:
--   /tmp/idun_doom_out   doom-idun writes frame packets → we read → minisock.write → C64
--   /tmp/idun_doom_in    C64 sends input → minisock.read → we write → doom-idun reads
--
-- The relay loop runs at the pace of incoming data; Lua's minisock.read
-- with timeout=0 is non-blocking so we don't stall on the input side.

local minisock = require('minisock')

local PIPE_OUT = '/tmp/idun_doom_out'   -- doom-idun → C64
local PIPE_IN  = '/tmp/idun_doom_in'    -- C64 → doom-idun
local DOOM_BIN = '/usr/local/bin/doom-idun'
local WAD_PATH = os.getenv('HOME') .. '/doom1.wad'

-- ── create pipes (ignore errors if they already exist) ──────────────────────
os.execute('mkfifo ' .. PIPE_OUT .. ' 2>/dev/null')
os.execute('mkfifo ' .. PIPE_IN  .. ' 2>/dev/null')

-- ── launch doom-idun in background ─────────────────────────────────────────
-- doom-idun opens PIPE_OUT for writing and PIPE_IN for reading,
-- using the paths compiled into doomgeneric_idun.c.
local cmd = DOOM_BIN .. ' -iwad ' .. WAD_PATH ..
            ' > /tmp/idun_doom.log 2>&1 &'
os.execute(cmd)

-- Give doom-idun a moment to open the pipes on its end; we open ours now.
-- Named pipes block open() until both ends are connected, so the order matters:
--   doom-idun opens PIPE_OUT for writing  →  we open PIPE_OUT for reading
--   doom-idun opens PIPE_IN  for reading  →  we open PIPE_IN  for writing
-- Open in this order to avoid deadlock: out first, then in.
local fout = io.open(PIPE_OUT, 'rb')   -- blocks until doom-idun connects
if not fout then
    error('Cannot open ' .. PIPE_OUT)
end
local fin = io.open(PIPE_IN, 'wb')     -- blocks until doom-idun connects
if not fin then
    error('Cannot open ' .. PIPE_IN)
end

-- ── relay loop ──────────────────────────────────────────────────────────────
-- Forward data from doom-idun to C64 in 256-byte chunks.
-- Poll C64 input non-blocking and forward to doom-idun.
local CHUNK = 256

while true do
    -- Pi → C64: read up to CHUNK bytes from doom-idun (blocking read,
    -- fout:read returns nil at EOF meaning doom-idun exited)
    local data = fout:read(CHUNK)
    if data == nil then
        break   -- doom-idun has exited
    end
    if #data > 0 then
        minisock.write(redirect.stdout, data)
    end

    -- C64 → Pi: non-blocking poll for input packets
    local input = minisock.read(redirect.stdin, 0)
    if input and #input > 0 then
        fin:write(input)
        fin:flush()
    end
end

-- clean up
fout:close()
fin:close()
os.execute('rm -f ' .. PIPE_OUT .. ' ' .. PIPE_IN)
