#!/usr/bin/env python3
# Copyright (c) 2025-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test ReSatoshi mainnet mining and per-block ASERT adjustment."""

from test_framework.blocktools import (
    create_coinbase,
    nbits_str,
    target_str,
)
from test_framework.messages import (
    CBlock,
    uint256_from_compact,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


COINBASE_SCRIPT_PUBKEY = "76a914eadbac7f36c37e39361168b7aaee3cb24a25312d88ac"
TARGET_SPACING = 10 * 60
ASERT_HALF_LIFE = 2 * 24 * 60 * 60
ASERT_RADIX = 1 << 16


def trunc_div(numerator, denominator):
    """Integer division truncated toward zero, matching C++."""
    if numerator < 0:
        return -((-numerator) // denominator)
    return numerator // denominator


def compact_from_uint256(value):
    size = (value.bit_length() + 7) // 8
    if size <= 3:
        compact = value << (8 * (3 - size))
    else:
        compact = value >> (8 * (size - 3))
    if compact & 0x00800000:
        compact >>= 8
        size += 1
    return compact | (size << 24)


def calculate_asert(anchor_target, time_delta, height_delta, pow_limit):
    """Mirror CalculateASERT in src/pow.cpp for an independent RPC check."""
    schedule_delta = time_delta - TARGET_SPACING * (height_delta + 1)
    exponent = trunc_div(schedule_delta * ASERT_RADIX, ASERT_HALF_LIFE)
    shifts = exponent >> 16
    frac = exponent - shifts * ASERT_RADIX
    factor = ASERT_RADIX + ((
        195766423245049 * frac
        + 971821376 * frac * frac
        + 5127 * frac * frac * frac
        + (1 << 47)
    ) >> 48)

    target = anchor_target * factor
    if shifts < 0:
        target >>= -shifts
    else:
        target <<= shifts
    target >>= 16
    target = min(max(target, 1), pow_limit)
    return compact_from_uint256(target)


class MiningMainnetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.chain = ""  # main

    def expected_bits(self, height, block_time):
        anchor_parent_time = self.anchor_time - TARGET_SPACING
        return calculate_asert(
            anchor_target=self.anchor_target,
            time_delta=block_time - anchor_parent_time,
            height_delta=height,
            pow_limit=self.pow_limit,
        )

    def check_next_work(self, height, block_time):
        # setmocktime RPC is intentionally regtest-only. Restarting with the
        # hidden -mocktime option lets this mainnet test make UpdateTime and
        # getmininginfo deterministic without changing production consensus.
        self.restart_node(0, extra_args=[f"-mocktime={block_time}"])
        mining_info = self.nodes[0].getmininginfo()
        expected_bits = self.expected_bits(height, block_time)
        expected_target = uint256_from_compact(expected_bits)
        assert_equal(mining_info['next']['height'], height)
        assert_equal(mining_info['next']['bits'], nbits_str(expected_bits))
        assert_equal(mining_info['next']['target'], target_str(expected_target))
        return expected_bits

    def mine(self, height, block_time, expected_bits):
        node = self.nodes[0]
        block = CBlock()
        block.nVersion = 0x20000000
        block.hashPrevBlock = int(node.getbestblockhash(), 16)
        block.nTime = block_time
        block.nBits = expected_bits
        block.vtx = [create_coinbase(
            height=height,
            script_pubkey=bytes.fromhex(COINBASE_SCRIPT_PUBKEY),
            halving_period=210000,
        )]
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()

        assert_equal(node.submitblock(block.serialize(with_witness=False).hex()), None)
        assert_equal(node.getbestblockhash(), block.hash_hex)
        assert_equal(node.getblockcount(), height)

        mining_info = node.getmininginfo()
        assert_equal(mining_info['bits'], nbits_str(expected_bits))
        assert_equal(mining_info['target'], target_str(uint256_from_compact(expected_bits)))

    def run_test(self):
        node = self.nodes[0]
        genesis_header = node.getblockheader(node.getbestblockhash())
        self.anchor_time = genesis_header['time']
        anchor_bits = int(genesis_header['bits'], 16)
        self.anchor_target = uint256_from_compact(anchor_bits)
        self.pow_limit = int(node.getmininginfo()['target'], 16)
        assert_equal(self.anchor_target, self.pow_limit)

        self.log.info("Mine an on-schedule block at the genesis target")
        scheduled_time = self.anchor_time + TARGET_SPACING
        scheduled_bits = self.check_next_work(height=1, block_time=scheduled_time)
        assert_equal(scheduled_bits, anchor_bits)
        self.mine(height=1, block_time=scheduled_time, expected_bits=scheduled_bits)

        self.log.info("Mine a fast block and verify ASERT raises difficulty")
        fast_time = scheduled_time + 1
        fast_bits = self.check_next_work(height=2, block_time=fast_time)
        assert uint256_from_compact(fast_bits) < self.anchor_target
        self.mine(height=2, block_time=fast_time, expected_bits=fast_bits)

        self.log.info("Return to the ideal schedule and verify ASERT recovery")
        recovered_time = self.anchor_time + 3 * TARGET_SPACING
        recovered_bits = self.check_next_work(height=3, block_time=recovered_time)
        assert_equal(recovered_bits, anchor_bits)
        self.mine(height=3, block_time=recovered_time, expected_bits=recovered_bits)

        self.log.info("getblock RPC reports the historical target for the fast block")
        fast_block = node.getblock(node.getblockhash(2))
        assert_equal(fast_block['bits'], nbits_str(fast_bits))
        assert_equal(fast_block['target'], target_str(uint256_from_compact(fast_bits)))


if __name__ == '__main__':
    MiningMainnetTest(__file__).main()
