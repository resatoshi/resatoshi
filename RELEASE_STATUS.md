# ReSatoshi public alpha status

Date: 2026-08-05

## Candidate scope

This source is for a resettable public alpha network only. Alpha RST has no
monetary value, the chain may be reset after a defect or incompatible change,
and this checkpoint is not a value-bearing mainnet release.

## Included consensus and identity work

- Zero-value genesis transaction with the message `Flow like water`.
- UTXO expiry at exactly 5,256,000 blocks.
- Expiry-height chainstate buckets, persistent Recycle Pool accounting, atomic
  database updates, block Undo data, and disconnect restoration.
- Recycle payouts capped at 1 RST per block. The Pool is debited only for value
  actually claimed above the ordinary subsidy and transaction fees.
- Per-block integer ASERT with a two-day half-life and a genesis anchor.
- Wallet balance, address-balance, and coin-selection paths that exclude
  expired outputs while preserving wallet history.
- ReSatoshi-specific executable names, data directory, configuration file,
  address namespaces, message bytes, and ports.
- Spendable addresses remain isolated from Bitcoin. Wallet key containers use
  standard WIF and BIP32 `xpub`/`xprv` encodings so upstream key and descriptor
  tooling remains compatible; these strings are not payment destinations.
- An isolated public alpha selected with `-testnet`, using Bech32 prefix `trs`,
  P2P port `49595`, RPC port `49594`, and no inherited Bitcoin DNS seeds or
  chain assumptions.
- A private `regtest` chain kept on Bitcoin Core's historical fixture so the
  upstream unit-test clocks, hashes, and assumeutxo snapshots remain valid.
- Empty `AutoFile` reads and writes return before a zero-length span can pass a
  null data pointer to `fread` or `fwrite`; the regression test covers both
  unobfuscated and obfuscated files.

## Verification record from the completed candidate

Before the final archive was lost, the dependency-complete candidate recorded:

- Generic build and full CTest: **375/375 passed**.
- ASan+UBSan build and full CTest after the `AutoFile` repair:
  **375/375 passed**, with no AddressSanitizer or UndefinedBehaviorSanitizer
  diagnostics.
- The first sanitizer pass found one real defect in `net_tests`: an empty
  captured message reached `AutoFile::write` as a zero-length span. The repair
  and the dedicated `autofile_empty_span` regression test then passed in both
  the normal and sanitizer builds.
- Twelve focused sanitizer groups passed, covering Recycle stress, 600-year
  supply and Undo behavior, address/WIF/extended-key encodings, descriptors,
  BIP32, BIP324, ASERT/PoW, and network separation.
- All 497 object files and 15 static libraries in each of the normal and
  sanitizer build trees passed format and nonzero-size checks.
- Five source linters, whitespace checks, include checks, and circular
  dependency checks passed.

LeakSanitizer was the only sanitizer component not completed in that sandbox.
It could not inspect `/proc/<pid>/task` during process shutdown, so leak
detection was disabled while AddressSanitizer and UndefinedBehaviorSanitizer
remained active. LSan still requires an ordinary Linux host or CI runner.

## Consensus stress and long-horizon results

- One million deterministic Recycle Pool transitions conserved exact
  accounting in optimized and UBSan builds.
- One million deterministic coinbase-underclaim combinations verified that
  only value above ordinary subsidy and fees leaves the Pool.
- A 10,000-UTXO expiry/reorg stress test covered 5,000 pre-expiry spends, bulk
  expiry, malformed Undo rejection, and exact restoration.
- The recurring-expiry simulation processed 31,536,001 blocks through year 600
  and ended with `30,600,000 RST` expired, `26,280,001 RST` paid, and
  `4,319,999 RST` in the Pool.
- The whole-supply simulation maintained
  `issued subsidy = liquid UTXOs + queued UTXOs + Recycle Pool` after every
  block and rejected deliberate one-satoshi inflation and loss. At year 600:
  `20,999,949.97690000 = 16,754,350.97690000 + 0 + 4,245,599.00000000 RST`.
- The adversarial-underclaim simulation mixed full, partial, and zero Recycle
  claims through year 600 while preserving exact Pool accounting and Undo.

## Archive reconstruction checks

The final Git objects were not present in the retained `dd03168` archive. The
two intended deltas were reconstructed on 2026-08-05: the verified regtest
fixture and the empty-span safety repair with its regression test.

The reconstructed tree passed the checks available in the recovery workspace:

- syntax-only compilation of `src/kernel/chainparams.cpp`, `src/streams.cpp`,
  and `src/test/streams_tests.cpp`;
- all six standalone Recycle harnesses in optimized and UBSan builds, including
  the three 600-year simulations;
- `lint-files`, `lint-include-guards`, `lint-includes`,
  `lint-circular-dependencies`, `lint-tests`, and Git whitespace checks.

That workspace did not contain CMake, libevent, SQLite, or the full supported
Boost development package. Therefore the **current reconstructed commit must
repeat the complete CTest run on the target Ubuntu machine before P2P port
49595 is opened**. The earlier 375/375 results are retained above as the
verification record of the completed candidate, not represented as a fresh
run of the reconstructed archive.

## Target Ubuntu rerun and compatibility repair

The reconstructed `c15aa61` archive was configured on the target Ubuntu host
with GUI, IPC, ZMQ, and benchmarks disabled. This configuration registered 370
tests. The first complete run passed 355 and failed 15. The failures were not
in the Recycle Pool tests; they were upstream fixtures that still assumed
Bitcoin mainnet address prefixes, network magic, port 8333, BIP32 containers,
or historical chain parameters.

This repair candidate:

- keeps ReSatoshi payment-address prefixes, `rs` Bech32 HRP, message magic,
  ports, genesis blocks, ASERT, expiry, and Recycle Pool consensus unchanged;
- restores standard WIF and BIP32 wallet-key containers;
- re-encodes Bitcoin address vectors to the selected ReSatoshi network while
  preserving their script payloads;
- runs historical mining fixtures on the restored private regtest chain;
- makes fixed BIP324 vectors explicitly use Bitcoin's vector magic while
  production traffic continues to use ReSatoshi's selected-network magic;
- removes hard-coded port 8333 expectations; and
- handles the zero-minimum-chain-work edge case in the IBD unit test.

The changed production files and the new address-vector helper passed
syntax-only compilation in the recovery workspace. All available source
linters passed. Full CTest verification of this repair candidate remains
pending on the target Ubuntu host.

The first target-host rerun of this candidate passed 368 of 370 tests. The two
remaining failures were fixture-only mismatches: two inferred descriptors
still expected Bitcoin payment addresses, and the historical miner fixture
ran with CSV active even though the test intentionally exercises its
pre-activation behavior. The follow-up repair updates those two expected
addresses and delays CSV only inside that private miner-test regtest fixture;
public ReSatoshi CSV activation and all production consensus parameters remain
unchanged. A complete target-host rerun is still required.

## Remaining release debt

- Repeat the normal 370-test build/CTest for this repair candidate, then repeat
  ASan+UBSan CTest; run LSan on an unrestricted Linux host.
- Test GUI, IPC, ZMQ, fuzz targets, and migration from previous alpha data.
- Run multi-machine partition, competing-chain, long-reorganization, and soak
  tests.
- Obtain independent consensus and security review.
- Establish public seed infrastructure and signed reproducible binaries before
  treating the network as more than an experimental alpha.

## Readiness assessment

The source is a candidate for a resettable, valueless public alpha. It is not a
safe value-bearing mainnet release. Complete the target-host 370-test run for
this exact repair commit and confirm the archive hash before starting the first
public node.
