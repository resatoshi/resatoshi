import hashlib
import struct
import argparse

MESSAGE = b"Flow like water"
BITS = 0x1f00ffff
VERSION = 1
DEFAULT_TIME = 1785594544
REWARD = 0

PUBKEY = bytes.fromhex(
    "04678afdb0fe5548271967f1a67130b7105cd6a828e03909"
    "a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112"
    "de5c384df7ba0b8d578a4c702b6bf11d5f"
)
# CScript() << PUBKEY << OP_CHECKSIG serializes the 65-byte public key as a
# pushed byte vector. The push-length opcode (0x41) is consensus-critical.
OUTPUT_SCRIPT = bytes([len(PUBKEY)]) + PUBKEY + b"\xac"


def sha256d(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def varint(number: int) -> bytes:
    if number < 0xfd:
        return bytes([number])
    if number <= 0xffff:
        return b"\xfd" + struct.pack("<H", number)
    if number <= 0xffffffff:
        return b"\xfe" + struct.pack("<I", number)
    return b"\xff" + struct.pack("<Q", number)


def compact_target(bits: int) -> int:
    exponent = bits >> 24
    mantissa = bits & 0x007fffff
    if exponent <= 3:
        return mantissa >> (8 * (3 - exponent))
    return mantissa << (8 * (exponent - 3))


script_sig = (
    bytes.fromhex("04ffff001d")
    + bytes.fromhex("0104")
    + varint(len(MESSAGE))
    + MESSAGE
)

transaction = (
    struct.pack("<I", 1)
    + varint(1)
    + bytes(32)
    + struct.pack("<I", 0xffffffff)
    + varint(len(script_sig))
    + script_sig
    + struct.pack("<I", 0xffffffff)
    + varint(1)
    + struct.pack("<Q", REWARD)
    + varint(len(OUTPUT_SCRIPT))
    + OUTPUT_SCRIPT
    + struct.pack("<I", 0)
)

merkle_raw = sha256d(transaction)
merkle_display = merkle_raw[::-1].hex()

parser = argparse.ArgumentParser(description="Mine and verify the ReSatoshi genesis block")
parser.add_argument("--time", type=int, default=DEFAULT_TIME, help="initial Unix timestamp")
args = parser.parse_args()

genesis_time = args.time
target = compact_target(BITS)
nonce = 0

print("Mining ReSatoshi genesis block...")
print("Message:", MESSAGE.decode())
print("Time:", genesis_time)
print("Bits:", hex(BITS))

while True:
    header = (
        struct.pack("<I", VERSION)
        + bytes(32)
        + merkle_raw
        + struct.pack("<I", genesis_time)
        + struct.pack("<I", BITS)
        + struct.pack("<I", nonce)
    )

    block_hash_raw = sha256d(header)

    if int.from_bytes(block_hash_raw, "little") <= target:
        block_hash_display = block_hash_raw[::-1].hex()
        break

    nonce += 1

    if nonce > 0xffffffff:
        nonce = 0
        genesis_time += 1

print()
print("FOUND!")
print("nTime      =", genesis_time)
print("nNonce     =", nonce)
print("nBits      =", hex(BITS))
print("MerkleRoot =", merkle_display)
print("GenesisHash=", block_hash_display)
print()
print("CreateGenesisBlock line:")
print(
    f"genesis = CreateGenesisBlock({genesis_time}, {nonce}, "
    f"0x{BITS:08x}, 1, 0 * COIN);"
)
