# ReSatoshi public alpha guide

## Safety and purpose

The alpha network exists to test node synchronization, mining, transactions,
wallet behavior, reorganizations, UTXO expiry, and Recycle Pool accounting.
Alpha RST has no monetary value. The chain may be reset after a bug or an
incompatible alpha release.

Never reuse a Bitcoin wallet, seed phrase, private key, data directory, or
configuration file with ReSatoshi.

## Network identity

| Item | Public alpha value |
| --- | --- |
| Chain option | `-testnet` |
| Data directory | `~/.resatoshi/alpha` |
| Configuration | `~/.resatoshi/resatoshi.conf` |
| P2P message start | `18 9a 6c 98` |
| P2P port | `49595` |
| Local RPC port | `49594` |
| Bech32 address prefix | `trs` |
| Genesis hash | `00003238910a7bd34d9175b5b9929aeb491641d722b5c1c1eaa8aafafc05c55a` |

The alpha network has no built-in DNS seed. The first node starts alone and
later nodes receive its address explicitly with `-addnode=IP:49595` or an
`addnode=` line in `resatoshi.conf`.

Only `-testnet` is the supported public ReSatoshi alpha chain. The inherited
`-testnet4` and `-signet` modes remain in the source for upstream test
compatibility and must not be advertised as ReSatoshi public networks.

## Build on Ubuntu

Install the build dependencies described in `doc/build-unix.md`, then run:

```bash
cmake -B build -DBUILD_GUI=OFF -DBUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Do not publish binaries unless they were built from the tagged alpha commit and
their SHA-256 hashes were published with the source commit.

## Start the first alpha node

Create the default configuration directory and copy the sample configuration:

```bash
mkdir -p ~/.resatoshi
cp share/examples/resatoshi.conf ~/.resatoshi/resatoshi.conf
build/bin/resatoshid -daemon
build/bin/resatoshi-cli -testnet getblockchaininfo
```

The configuration already selects `testnet=1`, so adding `-testnet` to every
command is optional. Keeping it in important commands is a useful safety check.

## Mine the first alpha reward block

```bash
build/bin/resatoshi-cli -testnet createwallet miner
build/bin/resatoshi-cli -testnet -rpcwallet=miner getnewaddress
build/bin/resatoshi-cli -testnet -rpcwallet=miner generatetoaddress 1 YOUR_TRS_ADDRESS
build/bin/resatoshi-cli -testnet getblockcount
```

Replace `YOUR_TRS_ADDRESS` with the address returned by `getnewaddress`. A
height of `1` means the first post-genesis alpha block was accepted. Coinbase
rewards require 100 additional confirmations before spending.

## Connect a second node

On the second machine, start with the first node's public IP:

```bash
build/bin/resatoshid -testnet -daemon -addnode=FIRST_NODE_IP:49595
build/bin/resatoshi-cli -testnet getconnectioncount
build/bin/resatoshi-cli -testnet getblockcount
```

Only TCP port `49595` should be forwarded through the router or firewall for
public P2P connectivity. Keep RPC port `49594` bound to localhost and never
publish the cookie file or wallet private keys.

## Stop or reset

Stop cleanly:

```bash
build/bin/resatoshi-cli -testnet stop
```

For an announced alpha reset, stop the node and move the `alpha` directory to a
backup location before restarting. Do not delete a data directory unless its
exact path has been checked.
