// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp> // for 'map_list_of()'
#include <boost/foreach.hpp>

#include "checkpoints.h"

#include "main.h"
#include "uint256.h"

namespace Checkpoints
{
    typedef std::map<int, uint256> MapCheckpoints;

    // How many times we expect transactions after the last checkpoint to
    // be slower. This number is a compromise, as it can't be accurate for
    // every system. When reindexing from a fast disk with a slow CPU, it
    // can be up to 20, while when downloading from a slow network with a
    // fast multicore CPU, it won't be much higher than 1.
    static const double fSigcheckVerificationFactor = 5.0;

    struct CCheckpointData {
        const MapCheckpoints *mapCheckpoints;
        int64 nTimeLastCheckpoint;
        int64 nTransactionsLastCheckpoint;
        double fTransactionsPerDay;
    };

    // What makes a good checkpoint block?
    // + Is surrounded by blocks with reasonable timestamps
    //   (no blocks before with a timestamp after, none after with
    //    timestamp before)
    // + Contains no strange transactions
    static MapCheckpoints mapCheckpoints =
        boost::assign::map_list_of
	      (  0, uint256("0x00000496d303ae6e6ed9d474639f18b3fdf70166c8d89d1267bbf5fd640e1690"))
              (  1, uint256("0x000008660c7c840e4f2526ae0bb3b186bdd30ed43e1e1d02c3dec7074a14c9fb"))
	      (  100, uint256("0x000000000a01771b25cde2fadacbd46f76543bec076d7b4eadc78f90ce635a2b"))
              (  300, uint256("0x00000000003780fe6d89514e6319e18dbf10192394efe2e68f19303f325b4f51"))
	      (  505, uint256("0x000000000004b5897709f30fbb73ce176c9e1434e79a9bbd53b46960da0ea157"))
              (  1010, uint256("0x00000000002595298a3dc7e9fb0b62c4be15f4ffaa57bc5d06efb20aa5879921"))
              (  1515, uint256("0x00000000000bf5d94285d7ca1fd72b3d29c018b1d0cba4419952788edbe20028"))
              (  1640, uint256("0x000000000023a18f1d1cf8ad0f1ae6a71d2d25fdf1fb6eb122e064449ad16f73"))
	      (  5000, uint256("0x00000000010f3a103a86bdd30cbf35ce6aa6cfa0ce8b624db317f95cc060f12a"))
	      (  5500, uint256("0x000000000f3299960b58ec6a0db59ee67b44359a1f9cea367b6035d4cc75df53"))
	      (  5550, uint256("0x0000000007505cba20fb03ae763fa331fc588dbc75e2f3ad5a6439ce69162a0f"))
	      (  5555, uint256("0x00000000067b4df76cdc8143148bb639cbfa71b77386258e099295ff8b255938"))
		  
        ;
    static const CCheckpointData data = {
        &mapCheckpoints,
	1487788280, // * UNIX timestamp of last checkpoint block 1397260800
//        1397260800, // * UNIX timestamp of last checkpoint block 1397260800
   	6269,    // * total number of transactions between genesis and last checkpoint
                    //   (the tx=... number in the SetBestChain debug.log lines)
        288     // * estimated number of transactions per day after checkpoint
    };
    static MapCheckpoints mapCheckpointsTestnet = 
        boost::assign::map_list_of
        (   0, uint256("0x"))
        ;
    static const CCheckpointData dataTestnet = {
        &mapCheckpointsTestnet,
        1485907131,
        3000,
        30
    };

    const CCheckpointData &Checkpoints() {
        if (fTestNet)
            return dataTestnet;
        else
            return data;
    }

    bool CheckBlock(int Vcoinh, const uint256& hash)
    {
        if (fTestNet) return true; // Testnet has no checkpoints
        if (!GetBoolArg("-checkpoints", true))
            return true;

        const MapCheckpoints& checkpoints = *Checkpoints().mapCheckpoints;

        MapCheckpoints::const_iterator i = checkpoints.find(Vcoinh);
        if (i == checkpoints.end()) return true;
        return hash == i->second;
    }

    // Guess how far we are in the verification process at the given block index
    double GuessVerificationProgress(CBlockIndex *pindex) {
        if (pindex==NULL)
            return 0.0;

        int64 nNow = time(NULL);

        double fWorkBefore = 0.0; // Amount of work done before pindex
        double fWorkAfter = 0.0;  // Amount of work left after pindex (estimated)
        // Work is defined as: 1.0 per transaction before the last checkoint, and
        // fSigcheckVerificationFactor per transaction after.

        const CCheckpointData &data = Checkpoints();

        if (pindex->nChainTx <= data.nTransactionsLastCheckpoint) {
            double nCheapBefore = pindex->nChainTx;
            double nCheapAfter = data.nTransactionsLastCheckpoint - pindex->nChainTx;
            double nExpensiveAfter = (nNow - data.nTimeLastCheckpoint)/86400.0*data.fTransactionsPerDay;
            fWorkBefore = nCheapBefore;
            fWorkAfter = nCheapAfter + nExpensiveAfter*fSigcheckVerificationFactor;
        } else {
            double nCheapBefore = data.nTransactionsLastCheckpoint;
            double nExpensiveBefore = pindex->nChainTx - data.nTransactionsLastCheckpoint;
            double nExpensiveAfter = (nNow - pindex->nTime)/86400.0*data.fTransactionsPerDay;
            fWorkBefore = nCheapBefore + nExpensiveBefore*fSigcheckVerificationFactor;
            fWorkAfter = nExpensiveAfter*fSigcheckVerificationFactor;
        }

        return fWorkBefore / (fWorkBefore + fWorkAfter);
    }

    int GetTotalBlocksEstimate()
    {
        if (fTestNet) return 0; // Testnet has no checkpoints
        if (!GetBoolArg("-checkpoints", true))
            return 0;

        const MapCheckpoints& checkpoints = *Checkpoints().mapCheckpoints;

        return checkpoints.rbegin()->first;
    }

    CBlockIndex* GetLastCheckpoint(const std::map<uint256, CBlockIndex*>& mapBlockIndex)
    {
        if (fTestNet) return NULL; // Testnet has no checkpoints
        if (!GetBoolArg("-checkpoints", true))
            return NULL;

        const MapCheckpoints& checkpoints = *Checkpoints().mapCheckpoints;

        BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, checkpoints)
        {
            const uint256& hash = i.second;
            std::map<uint256, CBlockIndex*>::const_iterator t = mapBlockIndex.find(hash);
            if (t != mapBlockIndex.end())
                return t->second;
        }
        return NULL;
    }

    uint256 GetLastAvailableCheckpoint() {
        const MapCheckpoints& checkpoints = (fTestNet ? mapCheckpointsTestnet : mapCheckpoints);

        BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, checkpoints) {
            const uint256& hash = i.second;
            if(mapBlockIndex.count(hash) && mapBlockIndex[hash]->IsInMainChain())
                return(hash);
        }
        return(hashGenesisBlock);
   }

    uint256 GetLatestHardenedCheckpoint()
    {
        const MapCheckpoints& checkpoints = *Checkpoints().mapCheckpoints;
        return (checkpoints.rbegin()->second);
    }
}
