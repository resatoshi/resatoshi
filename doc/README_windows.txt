ReSatoshi public alpha
======================

ReSatoshi is an experimental Bitcoin Core-derived cryptocurrency prototype.
The public alpha network may be reset and alpha RST has no monetary value.

Run resatoshi-qt.exe and select the public alpha shortcut, or start
resatoshid.exe with -testnet. ReSatoshi uses its own data directory and must
never be pointed at a Bitcoin wallet or Bitcoin data directory.

Only the P2P port 49595 should be opened for public peer connections. Keep the
RPC port 49594 local and private.

See doc/resatoshi-alpha.md in the source distribution for the complete alpha
warning, build, connection, mining, and reset instructions.
