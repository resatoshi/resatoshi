#!/usr/bin/env python3
"""Cross-check the deterministic genesis miner against the C++ constants."""

import pathlib
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ReSatoshiGenesisTest(unittest.TestCase):
    def test_zero_issuance_mainnet_genesis(self):
        result = subprocess.run(
            ["python3", str(ROOT / "genesis_miner.py")],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("MerkleRoot = b19621383a511e65cd4ae8b7d6257c5abfe0e967e389f8728901c51e0e025b48", result.stdout)
        self.assertIn("GenesisHash= 0000abbb2646c7f6dd1930d4c9214f8670f18fe67399c0ea13c4ed7a94ba7800", result.stdout)
        self.assertIn("1, 0 * COIN);", result.stdout)

        chainparams = (ROOT / "src/kernel/chainparams.cpp").read_text()
        self.assertIn("CreateGenesisBlock(1785594544, 4997, 0x1f00ffff, 1, 0 * COIN)", chainparams)
        self.assertIn("0000abbb2646c7f6dd1930d4c9214f8670f18fe67399c0ea13c4ed7a94ba7800", chainparams)
        self.assertIn("b19621383a511e65cd4ae8b7d6257c5abfe0e967e389f8728901c51e0e025b48", chainparams)


if __name__ == "__main__":
    unittest.main()
