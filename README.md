# ReSatoshi

ReSatoshi is an experimental Bitcoin Core–based cryptocurrency prototype built
around one additional lifecycle rule: a UTXO that remains unspent for exactly
5,256,000 blocks expires into a Recycle Pool. The Pool can then return at most
1 RST per block through the coinbase reward.

The project preserves Bitcoin's UTXO validation model where possible and adds:

- zero RST genesis issuance with the message `Flow like water`;
- 10-minute target blocks with per-block ASERT difficulty adjustment;
- exact-height UTXO expiry with reversible reorganization handling;
- persistent Recycle Pool accounting;
- Pool debit based only on Recycle value actually claimed by the miner;
- ReSatoshi-specific network, address, data-directory, and executable names.

## Current status

This repository is an **alpha prototype**, not a safe value-bearing mainnet
release. The public alpha testnet may be reset, its RST has no monetary value,
and no investment or profit is promised.

The alpha network is intentionally isolated from both Bitcoin and the future
ReSatoshi mainnet. It uses `-testnet`, Bech32 prefix `trs`, P2P port `49595`,
RPC port `49594`, and the data directory `~/.resatoshi/alpha` on Linux.

See [Public alpha guide](doc/resatoshi-alpha.md) for build, connection, mining,
and reset instructions. Consensus details and verification status are recorded
in [ReSatoshi design](doc/resatoshi-design.md) and
[development status](RELEASE_STATUS.md).

## Main commands

After a successful build, the main programs are:

- `resatoshid` — full node daemon;
- `resatoshi-cli` — RPC command-line client;
- `resatoshi-qt` — optional graphical client;
- `resatoshi-wallet`, `resatoshi-tx`, and `resatoshi-util` — supporting tools.

## Upstream and license

ReSatoshi is derived from Bitcoin Core. Original Bitcoin Core copyright notices
are retained in source files. This project is distributed under the MIT license;
see [COPYING](COPYING).
