//
// Copyright (c) 2019 The Energi Core developers
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "spork.h"
#include "utilstrencodings.h"
#include "validation.h"
#include "wallet/wallet.h"
#include "pos_kernel.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "miner.h"

#include "test/test_energi.h"

#include <boost/test/unit_test.hpp>

struct PoSTestSetup : TestChain100Setup {
    CWallet wallet;
    int64_t mock_time{0};
    int block_shift{0};

    void UpdateMockTime(int block_count = 1) {
        mock_time += block_count * block_shift;
        SetMockTime(mock_time);
    }

    PoSTestSetup() {
#if 0
        const char* args[] = {
            "",
            "-debug=stake",
        };
        ParseParameters(ARRAYLEN(args), args);
        fDebug = true;
        fPrintToConsole = true;
#endif

        CScript scriptPubKey = CScript() <<  ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

        pwalletMain = &wallet;
        pwalletMain->AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());
        pwalletMain->ScanForWalletTransactions(chainActive.Genesis(), true);
        pwalletMain->ReacceptWalletTransactions();
        pwalletMain->nStakeSplitThreshold = 1;

        CBitcoinAddress spork_address;
        spork_address.Set(coinbaseKey.GetPubKey().GetID());
        BOOST_CHECK(spork_address.IsValid());

        BOOST_CHECK(sporkManager.SetSporkAddress(spork_address.ToString()));
        BOOST_CHECK(sporkManager.SetPrivKey(CBitcoinSecret(coinbaseKey).ToString()));
        BOOST_CHECK(sporkManager.UpdateSpork(SPORK_15_FIRST_POS_BLOCK, 103, *connman));
        BOOST_CHECK_EQUAL(nFirstPoSBlock, 103U);
        //int last_pow_height;

        mock_time = chainActive.Tip()->GetBlockTimeMax() + 5;
        block_shift = pwalletMain->nHashDrift;
        UpdateMockTime(0);

        // PoW mode
        //---
        for (auto i = 2; i > 0; --i) {
            auto blk = CreateAndProcessBlock(CMutableTransactionList(), scriptPubKey);
            BOOST_CHECK(blk.IsProofOfWork());
            UpdateMockTime();
        }

        // PoS mode by spork
        //---
        for (auto i = 30; i > 0; --i) {
            auto blk = CreateAndProcessBlock(CMutableTransactionList(), CScript());
            BOOST_CHECK(blk.IsProofOfStake());
            BOOST_CHECK(blk.HasStake());
            UpdateMockTime();
        }
    }

    ~PoSTestSetup() {
        pwalletMain = nullptr;
        nFirstPoSBlock = 999999;
        BOOST_CHECK(sporkManager.UpdateSpork(SPORK_15_FIRST_POS_BLOCK, 999999ULL, *connman));
    }
};

BOOST_FIXTURE_TEST_SUITE(PoS_tests, PoSTestSetup)

BOOST_AUTO_TEST_CASE(PoS_transition_test)
{
    // Still, it must continue PoS even after Spork change
    //---
    auto value_bak = sporkManager.GetSporkValue(SPORK_15_FIRST_POS_BLOCK);
    BOOST_CHECK(sporkManager.UpdateSpork(SPORK_15_FIRST_POS_BLOCK, 999999ULL, *connman));
    BOOST_CHECK_EQUAL(nFirstPoSBlock, 103U);

    {
        auto blk = CreateAndProcessBlock(CMutableTransactionList(), CScript());
        BOOST_CHECK(blk.IsProofOfStake());
        BOOST_CHECK(blk.HasStake());
        UpdateMockTime();
    }

    BOOST_CHECK(sporkManager.UpdateSpork(SPORK_15_FIRST_POS_BLOCK, value_bak, *connman));
}

BOOST_AUTO_TEST_CASE(PoS_check_signature)
{
    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    CKey key;
    key.MakeNewKey(true);
    BOOST_CHECK(key.SignCompact(blk.GetHash(), blk.posBlockSig));
    BOOST_CHECK(!CheckProofOfStake(state, blk, Params().GetConsensus()));
    state = CValidationState();
    BOOST_CHECK(!TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));
}

BOOST_AUTO_TEST_CASE(PoS_check_stake_tx)
{
    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus()));
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    blk.vtx.erase(blk.vtx.begin() + 1);

    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus())); // Yes, it's TRUE
    BOOST_CHECK(!TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));
}
    
BOOST_AUTO_TEST_CASE(PoS_check_coinbase) {
    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus()));
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    CMutableTransaction cb{*(blk.CoinBase())};
    cb.vout[0].scriptPubKey = cb.vout[1].scriptPubKey;
    blk.CoinBase() = MakeTransactionRef(std::move(cb));

    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus())); // Yes, it's TRUE
    BOOST_CHECK(!TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));
}

BOOST_AUTO_TEST_CASE(PoS_unknown_stake) {
    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus()));
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    blk.posStakeHash = uint256();

    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, Params().GetConsensus()));

        int dos = 0;
        BOOST_CHECK(state_fail.IsInvalid(dos));
        BOOST_CHECK(!state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(dos, 100);
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "bad-unkown-stake");
    }

    blk.hashPrevBlock = uint256();

    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, Params().GetConsensus()));

        int dos = 0;
        BOOST_CHECK(!state_fail.IsInvalid(dos));
        BOOST_CHECK_EQUAL(dos, 0);
        BOOST_CHECK(state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "tmp-bad-unkown-stake");
    }
}

BOOST_AUTO_TEST_CASE(PoS_mempool_stake) {
    auto params = Params();
    auto consensus = params.GetConsensus();

    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus()));
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    CMutableTransaction mempoool_tx;
    blk.posStakeHash = mempoool_tx.GetHash();
    TestMemPoolEntryHelper entry;
    BOOST_CHECK(mempool.addUnchecked(blk.posStakeHash, entry.FromTx(mempoool_tx)));

    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, consensus));

        int dos = 0;
        BOOST_CHECK(state_fail.IsInvalid(dos));
        BOOST_CHECK(!state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(dos, 100);
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "bad-stake-mempool");
    }

    blk.hashPrevBlock = uint256();
    
    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, consensus));

        int dos = 0;
        BOOST_CHECK(!state_fail.IsInvalid(dos));
        BOOST_CHECK_EQUAL(dos, 0);
        BOOST_CHECK(state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "tmp-bad-stake-mempool");
    }
}

BOOST_AUTO_TEST_CASE(PoS_beyond_fork_point) {
    auto params = Params();
    auto consensus = params.GetConsensus();

    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus()));
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    blk.hashPrevBlock = uint256();;

    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, consensus));

        int dos = 0;
        BOOST_CHECK(state_fail.IsInvalid(dos));
        BOOST_CHECK(!state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(dos, 100);
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "bad-prev-header");
    }

    blk.hashPrevBlock = chainActive[0]->GetBlockHash();

    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, consensus));

        int dos = 0;
        BOOST_CHECK(state_fail.IsInvalid(dos));
        BOOST_CHECK(!state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(dos, 100);
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "bad-stake-after-fork");
    }
}

BOOST_AUTO_TEST_CASE(PoS_coinbase_maturity) {
    auto params = Params();
    auto consensus = params.GetConsensus();

    UpdateMockTime();

    auto pblk = BlockAssembler(Params()).CreateNewBlock(CScript(), pwalletMain)->block;
    auto &blk = *pblk;

    CValidationState state;
    BOOST_CHECK(CheckProofOfStake(state, blk, Params().GetConsensus()));
    BOOST_CHECK(TestBlockValidity(state, Params(), blk, chainActive.Tip(), true, false));

    CBlock maturity_edge;
    auto pindex_maturity_edge = chainActive[chainActive.Height() - COINBASE_MATURITY + 1];
    ReadBlockFromDisk(maturity_edge, pindex_maturity_edge, consensus);
    blk.posStakeHash = maturity_edge.vtx[0]->GetHash();

    {
        CValidationState state_fail;
        BOOST_CHECK(!CheckProofOfStake(state_fail, blk, consensus));

        int dos = 0;
        BOOST_CHECK(state_fail.IsInvalid(dos));
        BOOST_CHECK(!state_fail.IsTransientError());
        BOOST_CHECK_EQUAL(dos, 100);
        BOOST_CHECK_EQUAL(state_fail.GetRejectReason(), "bad-stake-coinbase-maturity");
    }
}

BOOST_AUTO_TEST_SUITE_END()
