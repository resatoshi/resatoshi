#include <consensus/recycle_pool.h>
#include <consensus/recycle_state.h>

#include <cassert>

int main()
{
    Consensus::RecycleState state;
    const COutPoint outpoint{};
    Coin coin;
    coin.out.nValue = 8 * COIN;
    coin.nHeight = 100;
    const int expiry_height{100 + Consensus::UTXO_EXPIRY_AGE};

    assert(state.Queue(outpoint, coin));
    assert(!state.Queue(outpoint, coin));
    assert(state.BucketSize(expiry_height) == 1);
    const auto undo{state.ExpireAndPay(expiry_height, 3 * COIN)};
    assert(undo && undo->expired.size() == 1);
    assert(state.PoolBalance() == 5 * COIN);
    assert(state.Undo(expiry_height, *undo));
    assert(state.PoolBalance() == 0);
    assert(state.BucketSize(expiry_height) == 1);
    assert(state.Unqueue(outpoint, coin));
}
