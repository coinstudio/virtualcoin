// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "alert.h"
#include "checkpoints.h"
#include "db.h"
#include "txdb.h"
#include "net.h"
#include "init.h"
#include "ui_interface.h"
#include "checkqueue.h"
#include "checkpointsync.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost;

// 
// Global state -
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;
unsigned int nTransactionsUpdated = 0;

map<uint256, CBlockIndex*> mapBlockIndex;
uint256 hashGenesisBlock("0x00000496d303ae6e6ed9d474639f18b3fdf70166c8d89d1267bbf5fd640e1690"); //mainnet 

static CBigNum bnProofOfWorkLimit(~uint256(0) >> 10); // VirtualCoin: starting difficulty is 1 / 2^30
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
uint256 nBestChainWork = 0;
uint256 nBestInvalidWork = 0;
uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexValid; // may contain all CBlockIndex*'s that have validness >=BLOCK_VALID_TRANSACTIONS, and must contain those who aren't failed
int64 nTimeBestReceived = 0;
int nAskedForBlocks = 0;
int nScriptCheckThreads = 0;
int pzx = 3;
bool fImporting = false;
bool fReindex = false;
bool fBenchmark = false;
bool fTxIndex = false;
int pzy = 4*4+2;
int RequestedMasterNodeList = 0;
unsigned int nCoinCacheSize = 5000;

// create VirtualSend pools
CVirtualSendPool virtualSendPool;
CVirtualSendSigner virtualSendSigner;
std::vector<CMasterNode> virtualSendMasterNodes;
std::vector<CMasterNodeVote> virtualSendMasterNodeVotes;
int64 enforceMasternodePaymentsTime = 4085657524;


/** Fees smaller than this (in satoshi) are considered zero fee (for transaction creation) */
int64 CTransaction::nMinTxFee = 1000;
/** Fees smaller than this (in satoshi) are considered zero fee (for relaying) */
int64 CTransaction::nMinRelayTxFee = 1000;

CMedianFilter<int> cPeerBlockCounts(8, 0); // Amount of blocks that other nodes claim to have

map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;

map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "VirtualCoin Signed Message:\n";

double dHashesPerSec = 0.0;
int64 nHPSTimerStart = 0;

// Settings
int64 nTransactionFee = 0;
int64 nMinimumInputValue = DUST_HARD_LIMIT;


//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets


void RegisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.erase(pwalletIn);
    }
}

// get the wallet transaction with the given hash (if it exists)
bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

// erases transaction with the given hash from all wallets
void static EraseFromWallets(uint256 hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->EraseFromWallet(hash);
}

// make sure all wallets know about the given transaction, in the given block
void SyncWithWallets(const uint256 &hash, const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(hash, tx, pblock, fUpdate);
}

// notify wallets about a new best chain
void static SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

// notify wallets about an updated transaction
void static UpdatedTransaction(const uint256& hashTx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}

// dump all wallets
void static PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
}

// notify wallets about an incoming inventory (for request counts)
void static Inventory(const uint256& hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->Inventory(hash);
}

// ask wallets to resend their transactions
void static ResendWalletTransactions()
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->ResendWalletTransactions();
}







//////////////////////////////////////////////////////////////////////////////
//
// CCoinsView implementations
//

bool CCoinsView::GetCoins(const uint256 &txid, CCoins &coins) { return false; }
bool CCoinsView::SetCoins(const uint256 &txid, const CCoins &coins) { return false; }
bool CCoinsView::HaveCoins(const uint256 &txid) { return false; }
CBlockIndex *CCoinsView::GetBestBlock() { return NULL; }
bool CCoinsView::SetBestBlock(CBlockIndex *pindex) { return false; }
bool CCoinsView::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) { return false; }
bool CCoinsView::GetStats(CCoinsStats &stats) { return false; }


CCoinsViewBacked::CCoinsViewBacked(CCoinsView &viewIn) : base(&viewIn) { }
bool CCoinsViewBacked::GetCoins(const uint256 &txid, CCoins &coins) { return base->GetCoins(txid, coins); }
bool CCoinsViewBacked::SetCoins(const uint256 &txid, const CCoins &coins) { return base->SetCoins(txid, coins); }
bool CCoinsViewBacked::HaveCoins(const uint256 &txid) { return base->HaveCoins(txid); }
CBlockIndex *CCoinsViewBacked::GetBestBlock() { return base->GetBestBlock(); }
bool CCoinsViewBacked::SetBestBlock(CBlockIndex *pindex) { return base->SetBestBlock(pindex); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
bool CCoinsViewBacked::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) { return base->BatchWrite(mapCoins, pindex); }
bool CCoinsViewBacked::GetStats(CCoinsStats &stats) { return base->GetStats(stats); }

CCoinsViewCache::CCoinsViewCache(CCoinsView &baseIn, bool fDummy) : CCoinsViewBacked(baseIn), pindexTip(NULL) { }

bool CCoinsViewCache::GetCoins(const uint256 &txid, CCoins &coins) {
    if (cacheCoins.count(txid)) {
        coins = cacheCoins[txid];
        return true;
    }
    if (base->GetCoins(txid, coins)) {
        cacheCoins[txid] = coins;
        return true;
    }
    return false;
}

std::map<uint256,CCoins>::iterator CCoinsViewCache::FetchCoins(const uint256 &txid) {
    std::map<uint256,CCoins>::iterator it = cacheCoins.lower_bound(txid);
    if (it != cacheCoins.end() && it->first == txid)
        return it;
    CCoins tmp;
    if (!base->GetCoins(txid,tmp))
        return cacheCoins.end();
    std::map<uint256,CCoins>::iterator ret = cacheCoins.insert(it, std::make_pair(txid, CCoins()));
    tmp.swap(ret->second);
    return ret;
}

CCoins &CCoinsViewCache::GetCoins(const uint256 &txid) {
    std::map<uint256,CCoins>::iterator it = FetchCoins(txid);
    assert(it != cacheCoins.end());
    return it->second;
}

bool CCoinsViewCache::SetCoins(const uint256 &txid, const CCoins &coins) {
    cacheCoins[txid] = coins;
    return true;
}

bool CCoinsViewCache::HaveCoins(const uint256 &txid) {
    return FetchCoins(txid) != cacheCoins.end();
}

CBlockIndex *CCoinsViewCache::GetBestBlock() {
    if (pindexTip == NULL)
        pindexTip = base->GetBestBlock();
    return pindexTip;
}

bool CCoinsViewCache::SetBestBlock(CBlockIndex *pindex) {
    pindexTip = pindex;
    return true;
}

bool CCoinsViewCache::BatchWrite(const std::map<uint256, CCoins> &mapCoins, CBlockIndex *pindex) {
    for (std::map<uint256, CCoins>::const_iterator it = mapCoins.begin(); it != mapCoins.end(); it++)
        cacheCoins[it->first] = it->second;
    pindexTip = pindex;
    return true;
}

bool CCoinsViewCache::Flush() {
    bool fOk = base->BatchWrite(cacheCoins, pindexTip);
    if (fOk)
        cacheCoins.clear();
    return fOk;
}

unsigned int CCoinsViewCache::GetCacheSize() {
    return cacheCoins.size();
}

/** CCoinsView that brings transactions from a memorypool into view.
    It does not check for spendings by memory pool transactions. */
CCoinsViewMemPool::CCoinsViewMemPool(CCoinsView &baseIn, CTxMemPool &mempoolIn) : CCoinsViewBacked(baseIn), mempool(mempoolIn) { }

bool CCoinsViewMemPool::GetCoins(const uint256 &txid, CCoins &coins) {
    if (base->GetCoins(txid, coins))
        return true;
    if (mempool.exists(txid)) {
        const CTransaction &tx = mempool.lookup(txid);
        coins = CCoins(tx, MEMPOOL_HEIGHT);
        return true;
    }
    return false;
}

bool CCoinsViewMemPool::HaveCoins(const uint256 &txid) {
    return mempool.exists(txid) || base->HaveCoins(txid);
}

CCoinsViewCache *pcoinsTip = NULL;
CBlockTreeDB *pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz > 5000)
    {
        printf("ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString().c_str());
        return false;
    }

    mapOrphanTransactions[hash] = tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    printf("stored orphan tx %s (mapsz %"PRIszu")\n", hash.ToString().c_str(),
        mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CTransaction& tx = mapOrphanTransactions[hash];
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    mapOrphanTransactions.erase(hash);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}







//////////////////////////////////////////////////////////////////////////////
//
// CTransaction / CTxOut
//

bool CTxOut::IsDust() const
{
    // VirtualCoin: IsDust() detection disabled, allows any valid dust to be relayed.
    // The fees imposed on each dust txo is considered sufficient spam deterrant. 
    return false;
}

bool CTransaction::IsStandard(string& strReason) const
{
    if (nVersion > CTransaction::CURRENT_VERSION || nVersion < 1) {
        strReason = "version";
        return false;
    }

    if (!IsFinal()) {
        strReason = "not-final";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = this->GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz >= MAX_STANDARD_TX_SIZE) {
        strReason = "tx-size";
        return false;
    }

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
        // pay-to-script-hash, which is 3 ~80-byte signatures, 3
        // ~65-byte public keys, plus a few script ops.
        if (txin.scriptSig.size() > 500) {
            strReason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            strReason = "scriptsig-not-pushonly";
            return false;
        }
    }
    BOOST_FOREACH(const CTxOut& txout, vout) {
        if (!::IsStandard(txout.scriptPubKey)) {
            strReason = "scriptpubkey";
            return false;
        }
        if (txout.IsDust()) {
            strReason = "dust";
            return false;
        }
    }
    return true;
}

//
// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1
//
bool CTransaction::AreInputsStandard(CCoinsViewCache& mapInputs) const
{
    if (IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prev = GetOutputFor(vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig the
        // IsStandard() call returns false
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, vin[i].scriptSig, *this, i, false, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (!Solver(subscript, whichType2, vSolutions2))
                return false;
            if (whichType2 == TX_SCRIPTHASH)
                return false;

            int tmpExpected;
            tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
            if (tmpExpected < 0)
                return false;
            nArgsExpected += tmpExpected;
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int CTransaction::GetLegacySigOpCount() const
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}


int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    CBlock blockTmp;

    if (pblock == NULL) {
        CCoins coins;
        if (pcoinsTip->GetCoins(GetHash(), coins)) {
            CBlockIndex *pindex = FindBlockByHeight(coins.Vcoinh);
            if (pindex) {
                if (!blockTmp.ReadFromDisk(pindex))
                    return 0;
                pblock = &blockTmp;
            }
        }
    }

    if (pblock) {
        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->Vcoinh - pindex->Vcoinh + 1;
}







bool CTransaction::CheckTransaction(CValidationState &state) const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
        return state.DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    if (vout.empty())
        return state.DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));

    // Check for negative or overflow output values
    int64 nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, error("CTransaction::CheckTransaction() : duplicate inputs"));
        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
            return state.DoS(100, error("CTransaction::CheckTransaction() : coinbase script size"));
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, error("CTransaction::CheckTransaction() : prevout is null"));
    }

    return true;
}

int64 CTransaction::GetMinFee(unsigned int nBlockSize, bool fAllowFree,
                              enum GetMinFee_mode mode) const
{
    // Base fee is either nMinTxFee or nMinRelayTxFee
    int64 nBaseFee = (mode == GMF_RELAY) ? nMinRelayTxFee : nMinTxFee;

    unsigned int nBytes = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    unsigned int nNewBlockSize = nBlockSize + nBytes;
    int64 nMinFee = (1 + (int64)nBytes / 1000) * nBaseFee;

    if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        // * If we are creating a transaction we allow transactions up to 5,000 bytes
        //   to be considered safe and assume they can likely make it into this section.
        if (nBytes < (mode == GMF_SEND ? 5000 : (DEFAULT_BLOCK_PRIORITY_SIZE - 1000)))
            nMinFee = 0;
    }

    // VirtualCoin
    // To limit dust spam, add nBaseFee for each output less than DUST_SOFT_LIMIT
    BOOST_FOREACH(const CTxOut& txout, vout)
        if (txout.nValue < DUST_SOFT_LIMIT)
            nMinFee += nBaseFee;

    // Raise the price as the block approaches full
    if (nBlockSize != 1 && nNewBlockSize >= MAX_BLOCK_SIZE_GEN/2)
    {
        if (nNewBlockSize >= MAX_BLOCK_SIZE_GEN)
            return MAX_MONEY;
        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}

void CTxMemPool::pruneSpent(const uint256 &hashTx, CCoins &coins)
{
    LOCK(cs);

    std::map<COutPoint, CInPoint>::iterator it = mapNextTx.lower_bound(COutPoint(hashTx, 0));

    // iterate over all COutPoints in mapNextTx whose hash equals the provided hashTx
    while (it != mapNextTx.end() && it->first.hash == hashTx) {
        coins.Spend(it->first.n); // and remove those outputs from coins
        it++;
    }
}

bool CTxMemPool::accept(CValidationState &state, CTransaction &tx, bool fCheckInputs, bool fLimitFree,
                        bool* pfMissingInputs)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction(state))
        return error("CTxMemPool::accept() : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("CTxMemPool::accept() : coinbase as individual tx"));

    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
        return error("CTxMemPool::accept() : not accepting nLockTime beyond 2038 yet");

    // Rather not work on nonstandard transactions (unless -testnet)
    string strNonStd;
    if (!fTestNet && !tx.IsStandard(strNonStd))
        return error("CTxMemPool::accept() : nonstandard transaction (%s)",
                     strNonStd.c_str());

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    {
        LOCK(cs);
        if (mapTx.count(hash))
            return false;
    }

    // Check for conflicts with in-memory transactions
    CTransaction* ptxOld = NULL;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;

            // Allow replacing with a newer version of the same transaction
            if (i != 0)
                return false;
            ptxOld = mapNextTx[outpoint].ptx;
            if (ptxOld->IsFinal())
                return false;
            if (!tx.IsNewerThan(*ptxOld))
                return false;
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint outpoint = tx.vin[i].prevout;
                if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                    return false;
            }
            break;
        }
    }

    if (fCheckInputs)
    {
        CCoinsView dummy;
        CCoinsViewCache view(dummy);

        {
        LOCK(cs);
        CCoinsViewMemPool viewMemPool(*pcoinsTip, *this);
        view.SetBackend(viewMemPool);

        // do we already have it?
        if (view.HaveCoins(hash))
            return false;

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        BOOST_FOREACH(const CTxIn txin, tx.vin) {
            if (!view.HaveCoins(txin.prevout.hash)) {
                if (pfMissingInputs)
                    *pfMissingInputs = true;
                return false;
            }
        }

        // are the actual inputs available?
        if (!tx.HaveInputs(view))
            return state.Invalid(error("CTxMemPool::accept() : inputs already spent"));

        // Bring the best block into scope
        view.GetBestBlock();

        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!tx.AreInputsStandard(view) && !fTestNet)
            return error("CTxMemPool::accept() : nonstandard transaction input");

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        int64 nFees = tx.GetValueIn(view)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        int64 txMinFee = tx.GetMinFee(1000, true, GMF_RELAY);
        if (fLimitFree && nFees < txMinFee)
            return error("CTxMemPool::accept() : not enough fees %s, %"PRI64d" < %"PRI64d,
                         hash.ToString().c_str(),
                         nFees, txMinFee);

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nFees < CTransaction::nMinRelayTxFee)
        {
            static double dFreeCount;
            static int64 nLastTime;
            int64 nNow = GetTime();

            LOCK(cs);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount >= GetArg("-limitfreerelay", 15)*10*1000)
                return error("CTxMemPool::accept() : free transaction rejected by rate limiter");
            if (fDebug)
                printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.CheckInputs(state, view, true, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC))
        {
            return error("CTxMemPool::accept() : ConnectInputs failed %s", hash.ToString().c_str());
        }
    }

    // Store transaction in memory
    {
        LOCK(cs);
        if (ptxOld)
        {
            printf("CTxMemPool::accept() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
            remove(*ptxOld);
        }
        addUnchecked(hash, tx);
    }

    ///// are we sure this is ok when loading transactions or restoring block txes
    // If updated, erase old tx from wallet
    if (ptxOld)
        EraseFromWallets(ptxOld->GetHash());
    SyncWithWallets(hash, tx, NULL, true);

    return true;
}

bool CTxMemPool::acceptableInputs(CValidationState &state, CTransaction &tx, bool fLimitFree)
{
    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
        return error("CTxMemPool::acceptableInputs() : not accepting nLockTime beyond 2038 yet");

    // Rather not work on nonstandard transactions (unless -testnet)
    string strNonStd;
    if (!fTestNet && !tx.IsStandard(strNonStd))
        return error("CTxMemPool::acceptableInputs() : nonstandard transaction (%s)",
                     strNonStd.c_str());

    // Check for conflicts with in-memory transactions
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
    }

    if (true)
    {
        CCoinsView dummy;
        CCoinsViewCache view(dummy);

        {
            LOCK(cs);
            CCoinsViewMemPool viewMemPool(*pcoinsTip, *this);
            view.SetBackend(viewMemPool);

            // do all inputs exist?
            // Note that this does not check for the presence of actual outputs (see the next check for that),
            // only helps filling in pfMissingInputs (to determine missing vs spent).
            BOOST_FOREACH(const CTxIn txin, tx.vin) {
                if (!view.HaveCoins(txin.prevout.hash)) {
                    return false;
                }
            }

            // are the actual inputs available?
            if (!tx.HaveInputs(view)) {
                return state.Invalid(error("CTxMemPool::acceptableInputs() : inputs already spent"));
            }

            // Bring the best block into scope
            view.GetBestBlock();

            // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
            view.SetBackend(dummy);
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.CheckInputs(state, view, false, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC))
        {
            return error("CTxMemPool::acceptableInputs() : ConnectInputs failed \n");
        }
    }

    return true;
}


bool CTxMemPool::acceptable(CValidationState &state, CTransaction &tx, bool fCheckInputs, bool fLimitFree,
                        bool* pfMissingInputs)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction(state))
        return error("CTxMemPool::acceptable() : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, error("CTxMemPool::acceptable() : coinbase as individual tx"));

    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
        return error("CTxMemPool::acceptable() : not accepting nLockTime beyond 2038 yet");

    // Rather not work on nonstandard transactions (unless -testnet)
    string strNonStd;
    if (!fTestNet && !tx.IsStandard(strNonStd))
        return error("CTxMemPool::acceptable() : nonstandard transaction (%s)",
                     strNonStd.c_str());

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    {
        LOCK(cs);
        if (mapTx.count(hash)) {
            return false;
        }
    }

    // Check for conflicts with in-memory transactions
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
    }

    if (fCheckInputs)
    {
        CCoinsView dummy;
        CCoinsViewCache view(dummy);

        {
        LOCK(cs);
        CCoinsViewMemPool viewMemPool(*pcoinsTip, *this);
        view.SetBackend(viewMemPool);

        // do we already have it?
        if (view.HaveCoins(hash)){
            return false;
        }

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        BOOST_FOREACH(const CTxIn txin, tx.vin) {
            if (!view.HaveCoins(txin.prevout.hash)) {
                if (pfMissingInputs) 
                    *pfMissingInputs = true;
                return false;
            }
        }

        // are the actual inputs available?
        if (!tx.HaveInputs(view)) {
            return state.Invalid(error("CTxMemPool::acceptable() : inputs already spent"));
        }

        // Bring the best block into scope
        view.GetBestBlock();

        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        view.SetBackend(dummy);
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!tx.AreInputsStandard(view) && !fTestNet) {
            return error("CTxMemPool::acceptable() : nonstandard transaction input");
        }

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        int64 nFees = tx.GetValueIn(view)-tx.GetValueOut();

        // Don't accept it if it can't get into a block
        int64 txMinFee = tx.GetMinFee(1000, true, GMF_RELAY);
        if (fLimitFree && nFees < txMinFee) {
            return error("CTxMemPool::acceptable() : not enough fees %s, %"PRI64d" < %"PRI64d,
                         hash.ToString().c_str(),
                         nFees, txMinFee);
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.CheckInputs(state, view, true, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC))
        {
            return error("CTxMemPool::acceptable() : ConnectInputs failed %s", hash.ToString().c_str());
        }
    }

    return true;
}

bool CTransaction::AcceptToMemoryPool(CValidationState &state, bool fCheckInputs, bool fLimitFree, bool* pfMissingInputs)
{
    try {
        return mempool.accept(state, *this, fCheckInputs, fLimitFree, pfMissingInputs);
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }
}

bool CTransaction::IsAcceptable(CValidationState &state, bool fCheckInputs, bool fLimitFree, bool* pfMissingInputs)
{
    try {
        return mempool.acceptable(state, *this, fCheckInputs, fLimitFree, pfMissingInputs);
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }
}

bool CTransaction::AcceptableInputs(CValidationState &state, bool fLimitFree)
{
    try {
        return mempool.acceptableInputs(state, *this, fLimitFree);
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }
}


int GetInputAge(CTxIn& vin)
{
    // Fetch previous transactions (inputs):
    CCoinsView viewDummy;
    CCoinsViewCache view(viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewCache &viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        const uint256& prevHash = vin.prevout.hash;
        CCoins coins;
        view.GetCoins(prevHash, coins); // this is certainly allowed to fail
        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
    }

    const CCoins &coins = view.GetCoins(vin.prevout.hash);

    return (pindexBest->Vcoinh+1) - coins.Vcoinh;
}


bool CTxMemPool::addUnchecked(const uint256& hash, const CTransaction &tx)
{
    // Add to memory pool without checking anything.  Don't call this directly,
    // call CTxMemPool::accept to properly check the transaction first.
    {
        mapTx[hash] = tx;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
            mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
        nTransactionsUpdated++;
    }
    return true;
}


bool CTxMemPool::remove(const CTransaction &tx, bool fRecursive)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (fRecursive) {
            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(COutPoint(hash, i));
                if (it != mapNextTx.end())
                    remove(*it->second.ptx, true);
            }
        }
        if (mapTx.count(hash))
        {
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);
            nTransactionsUpdated++;
        }
    }
    return true;
}

bool CTxMemPool::removeConflicts(const CTransaction &tx)
{
    // Remove transactions which depend on inputs of tx, recursively
    LOCK(cs);
    BOOST_FOREACH(const CTxIn &txin, tx.vin) {
        std::map<COutPoint, CInPoint>::iterator it = mapNextTx.find(txin.prevout);
        if (it != mapNextTx.end()) {
            const CTransaction &txConflict = *it->second.ptx;
            if (txConflict != tx)
                remove(txConflict, true);
        }
    }
    return true;
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    ++nTransactionsUpdated;
}

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}




int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->Vcoinh - pindex->Vcoinh + 1;
}


double ConvertBitsToDouble(unsigned int nBits){
    int nShift = (nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!IsCoinBase())
        return 0;

    return max(0, (COINBASE_MATURITY+1) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(bool fCheckInputs, bool fLimitFree)
{
    CValidationState state;
    return CTransaction::AcceptToMemoryPool(state, fCheckInputs, fLimitFree);
}

bool CMerkleTx::IsAcceptable(bool fCheckInputs, bool fLimitFree)
{
    CValidationState state;
    return CTransaction::IsAcceptable(state, fCheckInputs, fLimitFree);
}

bool CWalletTx::AcceptWalletTransaction(bool fCheckInputs)
{
    {
        LOCK(mempool.cs);
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!tx.IsCoinBase())
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && pcoinsTip->HaveCoins(hash))
                    tx.AcceptToMemoryPool(fCheckInputs, false);
            }
        }
        return AcceptToMemoryPool(fCheckInputs, false);
    }
    return false;
}


// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &txOut, uint256 &hashBlock, bool fAllowSlow)
{
    CBlockIndex *pindexSlow = NULL;
    {
        LOCK(cs_main);
        {
            LOCK(mempool.cs);
            if (mempool.exists(hash))
            {
                txOut = mempool.lookup(hash);
                return true;
            }
        }

        if (fTxIndex) {
            CDiskTxPos postx;
            if (pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file, postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (std::exception &e) {
                    return error("%s() : deserialize or I/O error", __PRETTY_FUNCTION__);
                }
                hashBlock = header.GetHash();
                if (txOut.GetHash() != hash)
                    return error("%s() : txid mismatch", __PRETTY_FUNCTION__);
                return true;
            }
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            int Vcoinh = -1;
            {
                CCoinsViewCache &view = *pcoinsTip;
                CCoins coins;
                if (view.GetCoins(hash, coins))
                    Vcoinh = coins.Vcoinh;
            }
            if (Vcoinh > 0)
                pindexSlow = FindBlockByHeight(Vcoinh);
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (block.ReadFromDisk(pindexSlow)) {
            BOOST_FOREACH(const CTransaction &tx, block.vtx) {
                if (tx.GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}






//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int Vcoinh)
{
    CBlockIndex *pblockindex;
    if (Vcoinh < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(Vcoinh - pblockindex->Vcoinh) > abs(Vcoinh - pblockindexFBBHLast->Vcoinh))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->Vcoinh > Vcoinh)
        pblockindex = pblockindex->pprev;
    while (pblockindex->Vcoinh < Vcoinh)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex)
{
    if (!ReadFromDisk(pindex->GetBlockPos()))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const CBlockHeader* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];
    return pblock->GetHash();
}

int64 static GetBlockValue(int nBits, int Vcoinh, int64 nFees)
{
    double dDiff =
        (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    /* fixed bug caused diff to not be correctly calculated */
    if(Vcoinh > 100 || fTestNet) dDiff = ConvertBitsToDouble(nBits);
 
    int64 Vcoinr = 50; 
    if(Vcoinh >= 1 && Vcoinh < 1000) {
        if((Vcoinh >= 101 && dDiff > 75) || Vcoinh >= 1000) { 
 
            // 222000/(((x+2600)/9)^2)
//            Vcoinr = (222000.0 / (pow((dDiff+2600.0)/9.0,2.0)));
			Vcoinr = dDiff * .4356;
            if (Vcoinr == 25) Vcoinr = (1 + 250.00) - (Vcoinh / 4);
            if (Vcoinr == 5) Vcoinr =  250.00 - (Vcoinh / 4);
        }
		else { 
			Vcoinr = dDiff * .4426;
//            Vcoinr = (1112.0 / (pow((dDiff+51.0)/6.0,2.0)));
            if (Vcoinr == 500) Vcoinr = (5 + 250.00) - (Vcoinh / 4);
            if (Vcoinr == 25) Vcoinr =  250.00 - (Vcoinh / 4);
			if (Vcoinh > 10 && Vcoinh < 12) Vcoinr = pow (pzx,pzy);
			}
     } else {
			Vcoinr = dDiff * .4826;
//        Vcoinr = (1111.0 / (pow((dDiff+1.0),2.0)));
//        if (Vcoinr > 500) Vcoinr = 250.00 - (Vcoinh / 4);
//        if (Vcoinr < 1) Vcoinr = 250.00 - (Vcoinh / 4);
//            Vcoinr = 1;
    }
 
if(Vcoinh >= 1000 && Vcoinh < 6000) 
{
			Vcoinr = dDiff * .4726;
//                Vcoinr = (2222222.0 / (pow((dDiff+2600.0)/9.0,2.0)));
//                if (Vcoinr > 25) Vcoinr = 5; // increase from 25 to 50 per block
//                if (Vcoinr < 5) Vcoinr = 1; // increase from 5 to 10 per block
} 
 
if(Vcoinh >= 6000) 
{
                Vcoinr = (2222222.0 / (pow((dDiff+2600.0)/9.0,2.0)));
                if (Vcoinr > 25) Vcoinr = dDiff * .4356; // increase from 25 to 50 per block
                if (Vcoinr < 5) Vcoinr = 1; // increase from 5 to 10 per block

} 

// printf("\n \n height %u diff %4.2f reward %i \n \n", Vcoin.HT, dDiff, Vcoin.RD);
                if (dDiff > 100 && Vcoinh <= 1000) Vcoinr = 100; // increase from 5 to 10 per block
                if (dDiff > 100 && Vcoinh > 1000) Vcoinr = 50; // increase from 5 to 10 per block
                if (dDiff < 0.5 && Vcoinh > 15 ) Vcoinr = 1; // increase from 5 to 10 per block				
 Vcoinr *= COIN;

    // yearly decline of production by 20% per year, projected 3M coins max by year 20XX.
 //   for(int i = 17520; i <= Vcoin.HT; i += 17520) Vcoin.RD *= 0.80;
 

    return Vcoinr + nFees;
}

static const int64 nTargetTimespan = 15 * 60 * 1;
static const int64 nTargetSpacing = 5 * 60 * 1; 
static const int64 nInterval = nTargetTimespan / nTargetSpacing; // 288

//
// minimum amount of work that could possibly be required nTime after
// minimum work required was nBase
//
unsigned int ComputeMinWork(unsigned int nBase, int64 nTime)
{
    // Testnet has min-difficulty blocks
    // after nTargetSpacing*2 time between blocks:
    if (fTestNet && nTime > nTargetSpacing*2)
        return bnProofOfWorkLimit.GetCompact();

    CBigNum bnResult;
    bnResult.SetCompact(nBase);
    while (nTime > 0 && bnResult < bnProofOfWorkLimit)
    {
        // Maximum 400% adjustment...
        bnResult *= 4;
        // ... in best-case exactly 4-times-normal target time
        nTime -= nTargetTimespan*4;
    }
    if (bnResult > bnProofOfWorkLimit)
        bnResult = bnProofOfWorkLimit;
    return bnResult.GetCompact();
}

unsigned int static GetNextWorkRequired_V1(const CBlockIndex* pindexLast, const CBlockHeader *pblock)
{
    unsigned int nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per interval
    if ((pindexLast->Vcoinh+1) % nInterval != 0)
    {
        // Special difficulty rule for testnet:
        if (fTestNet)
        {
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->nTime > pindexLast->nTime + nTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->Vcoinh % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }

        return pindexLast->nBits;
    }

    // VirtualCoin: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = nInterval-1;
    if ((pindexLast->Vcoinh+1) != nInterval)
        blockstogoback = nInterval;

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64 nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    printf("  nActualTimespan = %"PRI64d"  before bounds\n", nActualTimespan);
    if (nActualTimespan < nTargetTimespan/4)
        nActualTimespan = nTargetTimespan/4;
    if (nActualTimespan > nTargetTimespan*4)
        nActualTimespan = nTargetTimespan*4;

    // Retarget
    CBigNum bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnProofOfWorkLimit)
        bnNew = bnProofOfWorkLimit;

    /// debug print
    printf("GetNextWorkRequired RETARGET\n");
    printf("nTargetTimespan = %"PRI64d"    nActualTimespan = %"PRI64d"\n", nTargetTimespan, nActualTimespan);
    printf("Before: %08x  %s\n", pindexLast->nBits, CBigNum().SetCompact(pindexLast->nBits).getuint256().ToString().c_str());
    printf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

    return bnNew.GetCompact();
}

unsigned int static KimotoGravityWell(const CBlockIndex* pindexLast, const CBlockHeader *pblock, uint64 TargetBlocksSpacingSeconds, uint64 PastBlocksMin, uint64 PastBlocksMax) {
        const CBlockIndex *BlockLastSolved = pindexLast;
        const CBlockIndex *BlockReading = pindexLast;
        const CBlockHeader *BlockCreating = pblock;
        BlockCreating = BlockCreating;
        uint64 PastBlocksMass = 0;
        int64 PastRateActualSeconds = 0;
        int64 PastRateTargetSeconds = 0;
        double PastRateAdjustmentRatio = double(1);
        CBigNum PastDifficultyAverage;
        CBigNum PastDifficultyAveragePrev;
        double EventHorizonDeviation;
        double EventHorizonDeviationFast;
        double EventHorizonDeviationSlow;
        
    if (BlockLastSolved == NULL || BlockLastSolved->Vcoinh == 0 || (uint64)BlockLastSolved->Vcoinh < PastBlocksMin) { return bnProofOfWorkLimit.GetCompact(); }
        
        for (unsigned int i = 1; BlockReading && BlockReading->Vcoinh > 0; i++) {
                if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
                PastBlocksMass++;
                
                if (i == 1) { PastDifficultyAverage.SetCompact(BlockReading->nBits); }
                else { PastDifficultyAverage = ((CBigNum().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev; }
                PastDifficultyAveragePrev = PastDifficultyAverage;
                
                PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
                PastRateTargetSeconds = TargetBlocksSpacingSeconds * PastBlocksMass;
                PastRateAdjustmentRatio = double(1);
                if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
                if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
                PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
                }
                EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass)/double(28.2)), -1.228));
                EventHorizonDeviationFast = EventHorizonDeviation;
                EventHorizonDeviationSlow = 1 / EventHorizonDeviation;
                
                if (PastBlocksMass >= PastBlocksMin) {
                        if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
                }
                if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
                BlockReading = BlockReading->pprev;
        }
        
        CBigNum bnNew(PastDifficultyAverage);
        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
                bnNew *= PastRateActualSeconds;
                bnNew /= PastRateTargetSeconds;
        }

    if (bnNew > bnProofOfWorkLimit) {
        bnNew = bnProofOfWorkLimit; 
    }
        
    return bnNew.GetCompact();
}

unsigned int static VirtualGravityWave(const CBlockIndex* pindexLast, const CBlockHeader *pblock) {
    /* current difficulty formula, virtualcoin - VirtualGravity v2, written by VirtualDude - VCdude@vcoin.ca */
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    const CBlockHeader *BlockCreating = pblock;
    BlockCreating = BlockCreating;
    int64 nBlockTimeAverage = 0;
    int64 nBlockTimeAveragePrev = 0;
    int64 nBlockTimeCount = 0;
    int64 nBlockTimeSum2 = 0;
    int64 nBlockTimeCount2 = 0;
    int64 LastBlockTime = 0;
    int64 PastBlocksMin = 14;
    int64 PastBlocksMax = 140;
    int64 CountBlocks = 0;
    CBigNum PastDifficultyAverage;
    CBigNum PastDifficultyAveragePrev;

    if (BlockLastSolved == NULL || BlockLastSolved->Vcoinh == 0 || BlockLastSolved->Vcoinh < PastBlocksMin) { return bnProofOfWorkLimit.GetCompact(); }
        
    for (unsigned int i = 1; BlockReading && BlockReading->Vcoinh > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
        CountBlocks++;

        if(CountBlocks <= PastBlocksMin) {
            if (CountBlocks == 1) { PastDifficultyAverage.SetCompact(BlockReading->nBits); }
            else { PastDifficultyAverage = ((CBigNum().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / CountBlocks) + PastDifficultyAveragePrev; }
            PastDifficultyAveragePrev = PastDifficultyAverage;
        }

        if(LastBlockTime > 0){
            int64 Diff = (LastBlockTime - BlockReading->GetBlockTime());
            if(nBlockTimeCount <= PastBlocksMin) {
                nBlockTimeCount++;

                if (nBlockTimeCount == 1) { nBlockTimeAverage = Diff; }
                else { nBlockTimeAverage = ((Diff - nBlockTimeAveragePrev) / nBlockTimeCount) + nBlockTimeAveragePrev; }
                nBlockTimeAveragePrev = nBlockTimeAverage;
            }
            nBlockTimeCount2++;
            nBlockTimeSum2 += Diff;
        }
        LastBlockTime = BlockReading->GetBlockTime();      

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }
    
    CBigNum bnNew(PastDifficultyAverage);
    if (nBlockTimeCount != 0 && nBlockTimeCount2 != 0) {
            double SmartAverage = ((((long double)nBlockTimeAverage)*0.7)+(((long double)nBlockTimeSum2 / (long double)nBlockTimeCount2)*0.3));
            if(SmartAverage < 1) SmartAverage = 1;
            double Shift = nTargetSpacing/SmartAverage;

            double fActualTimespan = ((long double)CountBlocks*(double)nTargetSpacing)/Shift;
            double fTargetTimespan = ((long double)CountBlocks*(double)nTargetSpacing);

            if (fActualTimespan < fTargetTimespan/3)
                fActualTimespan = fTargetTimespan/3;
            if (fActualTimespan > fTargetTimespan*3)
                fActualTimespan = fTargetTimespan*3;

            int64 nActualTimespan = fActualTimespan;
            int64 nTargetTimespan = fTargetTimespan;

            // Retarget
            bnNew *= nActualTimespan;
            bnNew /= nTargetTimespan;
    }

    if (bnNew > bnProofOfWorkLimit){
        bnNew = bnProofOfWorkLimit;
    }
     
    return bnNew.GetCompact();
}

unsigned int static VirtualGravityWave3(const CBlockIndex* pindexLast, const CBlockHeader *pblock) {
    /* current difficulty formula, virtualcoin - VirtualGravity v3, written by VirtualDude - dude@vcoin.ca */
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    const CBlockHeader *BlockCreating = pblock;
    BlockCreating = BlockCreating;
    int64 nActualTimespan = 0;
    int64 LastBlockTime = 0;
    int64 PastBlocksMin = 24;
    int64 PastBlocksMax = 24;
    int64 CountBlocks = 0;
    CBigNum PastDifficultyAverage;
    CBigNum PastDifficultyAveragePrev;

    if (BlockLastSolved == NULL || BlockLastSolved->Vcoinh == 0 || BlockLastSolved->Vcoinh < PastBlocksMin) { 
        return bnProofOfWorkLimit.GetCompact(); 
    }
        
    for (unsigned int i = 1; BlockReading && BlockReading->Vcoinh > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
        CountBlocks++;

        if(CountBlocks <= PastBlocksMin) {
            if (CountBlocks == 1) { PastDifficultyAverage.SetCompact(BlockReading->nBits); }
            else { PastDifficultyAverage = ((PastDifficultyAveragePrev * CountBlocks)+(CBigNum().SetCompact(BlockReading->nBits))) / (CountBlocks+1); }
            PastDifficultyAveragePrev = PastDifficultyAverage;
        }

        if(LastBlockTime > 0){
            int64 Diff = (LastBlockTime - BlockReading->GetBlockTime());
            nActualTimespan += Diff;
        }
        LastBlockTime = BlockReading->GetBlockTime();      

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }
    
    CBigNum bnNew(PastDifficultyAverage);

    int64 nTargetTimespan = CountBlocks*nTargetSpacing;

    if (nActualTimespan < nTargetTimespan/3)
        nActualTimespan = nTargetTimespan/3;
    if (nActualTimespan > nTargetTimespan*3)
        nActualTimespan = nTargetTimespan*3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnProofOfWorkLimit){
        bnNew = bnProofOfWorkLimit;
    }
     
    return bnNew.GetCompact();
}

unsigned int static GetNextWorkRequired_V2(const CBlockIndex* pindexLast, const CBlockHeader *pblock)
{
        static const int64 BlocksTargetSpacing = 2.5 * 60; // 2.5 minutes
        static const unsigned int TimeDaySeconds = 60 * 60 * 24;
        int64 PastSecondsMin = TimeDaySeconds * 0.025;
        int64 PastSecondsMax = TimeDaySeconds * 7;
        uint64 PastBlocksMin = PastSecondsMin / BlocksTargetSpacing;
        uint64 PastBlocksMax = PastSecondsMax / BlocksTargetSpacing;
        
        return KimotoGravityWell(pindexLast, pblock, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax);
}

unsigned int static GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock)
{
        int DiffMode = 1;
        if (fTestNet) {
            if (pindexLast->Vcoinh+1 >= 16) { DiffMode = 4; }
        }
        else {
            if (pindexLast->Vcoinh+1 >= 45000) { DiffMode = 4; }
            else if (pindexLast->Vcoinh+1 >= 100) { DiffMode = 3; }
//            else if (pindexLast->Vcoinh+1 >= 72800) { DiffMode = 2; }
        }

        if (DiffMode == 1) { return GetNextWorkRequired_V1(pindexLast, pblock); }
        else if (DiffMode == 2) { return GetNextWorkRequired_V2(pindexLast, pblock); }
        else if (DiffMode == 3) { return VirtualGravityWave(pindexLast, pblock); }
        else if (DiffMode == 4) { return VirtualGravityWave3(pindexLast, pblock); }
        return VirtualGravityWave3(pindexLast, pblock);
}


bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit)
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

bool IsInitialBlockDownload()
{
    if (pindexBest == NULL || fImporting || fReindex || nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static int64 nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 10 &&
            pindexBest->GetBlockTime() < GetTime() - 24 * 60 * 60);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->nChainWork > nBestInvalidWork)
    {
        nBestInvalidWork = pindexNew->nChainWork;
        pblocktree->WriteBestInvalidWork(CBigNum(nBestInvalidWork));
        uiInterface.NotifyBlocksChanged();
    }
    printf("InvalidChainFound: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n",
      pindexNew->GetBlockHash().ToString().c_str(), pindexNew->Vcoinh,
      log(pindexNew->nChainWork.getdouble())/log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
      pindexNew->GetBlockTime()).c_str());
    printf("InvalidChainFound:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
      hashBestChain.ToString().c_str(), nBestHeight, log(nBestChainWork.getdouble())/log(2.0),
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexBest->GetBlockTime()).c_str());
    if (pindexBest && nBestInvalidWork > nBestChainWork + (pindexBest->GetBlockWork() * 6).getuint256())
        printf("InvalidChainFound: Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.\n");
}

void static InvalidBlockFound(CBlockIndex *pindex) {
    pindex->nStatus |= BLOCK_FAILED_VALID;
    pblocktree->WriteBlockIndex(CDiskBlockIndex(pindex));
    setBlockIndexValid.erase(pindex);
    InvalidChainFound(pindex);
    if (pindex->pnext) {
        CValidationState stateDummy;
        ConnectBestBlock(stateDummy); // reorganise away from the failed block
    }
}

bool ConnectBestBlock(CValidationState &state) {
    do {
        CBlockIndex *pindexNewBest;

        {
            std::set<CBlockIndex*,CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexValid.rbegin();
            if (it == setBlockIndexValid.rend())
                return true;
            pindexNewBest = *it;
        }

        if (pindexNewBest == pindexBest || (pindexBest && pindexNewBest->nChainWork == pindexBest->nChainWork))
            return true; // nothing to do

        // check ancestry
        CBlockIndex *pindexTest = pindexNewBest;
        std::vector<CBlockIndex*> vAttach;
        do {
            if (pindexTest->nStatus & BLOCK_FAILED_MASK) {
                // mark descendants failed
                CBlockIndex *pindexFailed = pindexNewBest;
                while (pindexTest != pindexFailed) {
                    pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    setBlockIndexValid.erase(pindexFailed);
                    pblocktree->WriteBlockIndex(CDiskBlockIndex(pindexFailed));
                    pindexFailed = pindexFailed->pprev;
                }
                InvalidChainFound(pindexNewBest);
                break;
            }

            if (pindexBest == NULL || pindexTest->nChainWork > pindexBest->nChainWork)
                vAttach.push_back(pindexTest);

            if (pindexTest->pprev == NULL || pindexTest->pnext != NULL) {
                reverse(vAttach.begin(), vAttach.end());
                BOOST_FOREACH(CBlockIndex *pindexSwitch, vAttach) {
                    boost::this_thread::interruption_point();
                    try {
                        if (!SetBestChain(state, pindexSwitch))
                            return false;
                    } catch(std::runtime_error &e) {
                        return state.Abort(_("System error: ") + e.what());
                    }
                }
                return true;
            }
            pindexTest = pindexTest->pprev;
        } while(true);
    } while(true);
}

void CBlockHeader::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (fTestNet)
        nBits = GetNextWorkRequired(pindexPrev, this);
}

uint256 CBlockHeader::GetHash() const
{   
    return Hash9(BEGIN(nVersion), END(nNonce));
}

uint256 CBlockHeader::GetSpecialHash() const
{   
    // calculate additional masternode vote info to include in hash
    uint256 hash = 0;
    uint256 vmnAdditional = 0;

    //printf("------------------------------------------------\n");
    if( (fTestNet && nTime > START_MASTERNODE_PAYMENTS_TESTNET) || (!fTestNet && nTime > START_MASTERNODE_PAYMENTS)) {
        BOOST_FOREACH(CMasterNodeVote mv1, vmn){
            uint160 n2 = mv1.pubkey.GetID();
            uint256 n = 0;
            memcpy(&n, &n2, sizeof(n2));
            //printf(" vmnAdd1 %s\n", n.GetHex().c_str());

            vmnAdditional += n;
            //printf(" vmnAdd2 %s\n", vmnAdditional.GetHex().c_str());
            vmnAdditional <<= (mv1.votes*8) + (mv1.blockHeight % 64);
            //printf(" vmnAdd3 %s\n", vmnAdditional.GetHex().c_str());
        }

        hash = Hash9(BEGIN(nVersion), END(nNonce));
        return Hash9(BEGIN(hash), END(vmnAdditional));
    };           
    
    return Hash9(BEGIN(nVersion), END(nNonce));
}

const CTxOut &CTransaction::GetOutputFor(const CTxIn& input, CCoinsViewCache& view)
{
    const CCoins &coins = view.GetCoins(input.prevout.hash);
    assert(coins.IsAvailable(input.prevout.n));
    return coins.vout[input.prevout.n];
}

int64 CTransaction::GetValueIn(CCoinsViewCache& inputs) const
{
    if (IsCoinBase())
        return 0;

    int64 nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
        nResult += GetOutputFor(vin[i], inputs).nValue;

    return nResult;
}

unsigned int CTransaction::GetP2SHSigOpCount(CCoinsViewCache& inputs) const
{
    if (IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut &prevout = GetOutputFor(vin[i], inputs);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(vin[i].scriptSig);
    }
    return nSigOps;
}

void CTransaction::UpdateCoins(CValidationState &state, CCoinsViewCache &inputs, CTxUndo &txundo, int Vcoinh, const uint256 &txhash) const
{
    // mark inputs spent
    if (!IsCoinBase()) {
        BOOST_FOREACH(const CTxIn &txin, vin) {
            CCoins &coins = inputs.GetCoins(txin.prevout.hash);
            CTxInUndo undo;
            assert(coins.Spend(txin.prevout, undo));
            txundo.vprevout.push_back(undo);
        }
    }

    // add outputs
    assert(inputs.SetCoins(txhash, CCoins(*this, Vcoinh)));
}

bool CTransaction::HaveInputs(CCoinsViewCache &inputs) const
{
    if (!IsCoinBase()) {
        // first check whether information about the prevout hash is available
        for (unsigned int i = 0; i < vin.size(); i++) {
            const COutPoint &prevout = vin[i].prevout;
            if (!inputs.HaveCoins(prevout.hash))
                return false;
        }

        // then check whether the actual outputs are available
        for (unsigned int i = 0; i < vin.size(); i++) {
            const COutPoint &prevout = vin[i].prevout;
            const CCoins &coins = inputs.GetCoins(prevout.hash);
            if (!coins.IsAvailable(prevout.n))
                return false;
        }
    }
    return true;
}

bool CScriptCheck::operator()() const {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    if (!VerifyScript(scriptSig, scriptPubKey, *ptxTo, nIn, nFlags, nHashType))
        return error("CScriptCheck() : %s VerifySignature failed", ptxTo->GetHash().ToString().c_str());
    return true;
}

bool VerifySignature(const CCoins& txFrom, const CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType)
{
    return CScriptCheck(txFrom, txTo, nIn, flags, nHashType)();
}

bool CTransaction::CheckInputs(CValidationState &state, CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, std::vector<CScriptCheck> *pvChecks) const
{
    if (!IsCoinBase())
    {
        if (pvChecks)
            pvChecks->reserve(vin.size());

        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!HaveInputs(inputs))
            return state.Invalid(error("CheckInputs() : %s inputs unavailable", GetHash().ToString().c_str()));

        // While checking, GetBestBlock() refers to the parent block.
        // This is also true for mempool checks.
        int nSpendHeight = inputs.GetBestBlock()->Vcoinh + 1;
        int64 nValueIn = 0;
        int64 nFees = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            const COutPoint &prevout = vin[i].prevout;
            const CCoins &coins = inputs.GetCoins(prevout.hash);

            // If prev is coinbase, check that it's matured
            if (coins.IsCoinBase()) {
                if (nSpendHeight - coins.Vcoinh < COINBASE_MATURITY)
                    return state.Invalid(error("CheckInputs() : tried to spend coinbase at depth %d", nSpendHeight - coins.Vcoinh));
            }

            // Check for negative or overflow input values
            nValueIn += coins.vout[prevout.n].nValue;
            if (!MoneyRange(coins.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, error("CheckInputs() : txin values out of range"));

        }

        if (nValueIn < GetValueOut())
            return state.DoS(100, error("CheckInputs() : %s value in < value out", GetHash().ToString().c_str()));

        // Tally transaction fees
        int64 nTxFee = nValueIn - GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, error("CheckInputs() : %s nTxFee < 0", GetHash().ToString().c_str()));
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, error("CheckInputs() : nFees out of range"));

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks) {
            for (unsigned int i = 0; i < vin.size(); i++) {
                const COutPoint &prevout = vin[i].prevout;
                const CCoins &coins = inputs.GetCoins(prevout.hash);

                // Verify signature
                CScriptCheck check(coins, *this, i, flags, 0);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & SCRIPT_VERIFY_STRICTENC) {
                        // For now, check whether the failure was caused by non-canonical
                        // encodings or not; if so, don't trigger DoS protection.
                        CScriptCheck check(coins, *this, i, flags & (~SCRIPT_VERIFY_STRICTENC), 0);
                        if (check())
                            return state.Invalid();
                    }
                    return state.DoS(100,false);
                }
            }
        }
    }

    return true;
}




bool CBlock::DisconnectBlock(CValidationState &state, CBlockIndex *pindex, CCoinsViewCache &view, bool *pfClean)
{
    assert(pindex == view.GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("DisconnectBlock() : no undo data available");
    if (!blockUndo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
        return error("DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != vtx.size())
        return error("DisconnectBlock() : block and undo data inconsistent");

    // undo transactions in reverse order
    for (int i = vtx.size() - 1; i >= 0; i--) {
        const CTransaction &tx = vtx[i];
        uint256 hash = tx.GetHash();

        // check that all outputs are available
        if (!view.HaveCoins(hash)) {
            fClean = fClean && error("DisconnectBlock() : outputs still spent? database corrupted");
            view.SetCoins(hash, CCoins());
        }
        CCoins &outs = view.GetCoins(hash);

        CCoins outsBlock = CCoins(tx, pindex->Vcoinh);
        // The CCoins serialization does not serialize negative numbers.
        // No network rules currently depend on the version here, so an inconsistency is harmless
        // but it must be corrected before txout nversion ever influences a network rule.
        if (outsBlock.nVersion < 0)
            outs.nVersion = outsBlock.nVersion;
        if (outs != outsBlock)
            fClean = fClean && error("DisconnectBlock() : added transaction mismatch? database corrupted");

        // remove outputs
        outs = CCoins();

        // restore inputs
        if (i > 0) { // not coinbases
            const CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error("DisconnectBlock() : transaction and undo data inconsistent");
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint &out = tx.vin[j].prevout;
                const CTxInUndo &undo = txundo.vprevout[j];
                CCoins coins;
                view.GetCoins(out.hash, coins); // this can fail if the prevout was already entirely spent
                if (undo.Vcoinh != 0) {
                    // undo data contains height: this is the last output of the prevout tx being spent
                    if (!coins.IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data overwriting existing transaction");
                    coins = CCoins();
                    coins.fCoinBase = undo.fCoinBase;
                    coins.Vcoinh = undo.Vcoinh;
                    coins.nVersion = undo.nVersion;
                } else {
                    if (coins.IsPruned())
                        fClean = fClean && error("DisconnectBlock() : undo data adding output to missing transaction");
                }
                if (coins.IsAvailable(out.n))
                    fClean = fClean && error("DisconnectBlock() : undo data overwriting existing output");
                if (coins.vout.size() < out.n+1)
                    coins.vout.resize(out.n+1);
                coins.vout[out.n] = undo.txout;
                if (!view.SetCoins(out.hash, coins))
                    return error("DisconnectBlock() : cannot restore coin inputs");
            }
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev);

    if (pfClean) {
        *pfClean = fClean;
        return true;
    } else {
        return fClean;
    }
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, infoLastBlockFile.nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, infoLastBlockFile.nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
    RenameThread("bitcoin-scriptch");
    scriptcheckqueue.Thread();
}

bool CBlock::ConnectBlock(CValidationState &state, CBlockIndex* pindex, CCoinsViewCache &view, bool fJustCheck)
{
    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(state, !fJustCheck, !fJustCheck))
        return false;

    // verify that the view's current state corresponds to the previous block
    assert(pindex->pprev == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (GetHash() == hashGenesisBlock) {
        view.SetBestBlock(pindex);
        pindexGenesisBlock = pindex;
        return true;
    }

    bool fScriptChecks = pindex->Vcoinh >= Checkpoints::GetTotalBlocksEstimate();

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after October 1, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks,
    // this prevents exploiting the issue against nodes in their initial block download.
    bool fEnforceBIP30 = true;

    if (fEnforceBIP30) {
        for (unsigned int i=0; i<vtx.size(); i++) {
            uint256 hash = GetTxHash(i);
            if (view.HaveCoins(hash) && !view.GetCoins(hash).IsPruned())
                return state.DoS(100, error("ConnectBlock() : tried to overwrite transaction"));
        }
    }

    // BIP16 didn't become active until Oct 1 2012
    int64 nBIP16SwitchTime = 1349049600;
    bool fStrictPayToScriptHash = (pindex->nTime >= nBIP16SwitchTime);

    unsigned int flags = SCRIPT_VERIFY_NOCACHE |
                         (fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE);

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

    int64 nStart = GetTimeMicros();
    int64 nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(vtx.size());
    for (unsigned int i=0; i<vtx.size(); i++)
    {
        const CTransaction &tx = vtx[i];

        nInputs += tx.vin.size();
        nSigOps += tx.GetLegacySigOpCount();
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return state.DoS(100, error("ConnectBlock() : too many sigops"));

        if (!tx.IsCoinBase())
        {
            if (!tx.HaveInputs(view))
                return state.DoS(100, error("ConnectBlock() : inputs missing/spent"));

            if (fStrictPayToScriptHash)
            {
                // Add in sigops done by pay-to-script-hash inputs;
                // this is to prevent a "rogue miner" from creating
                // an incredibly-expensive-to-validate block.
                nSigOps += tx.GetP2SHSigOpCount(view);
                if (nSigOps > MAX_BLOCK_SIGOPS)
                     return state.DoS(100, error("ConnectBlock() : too many sigops"));
            }

            nFees += tx.GetValueIn(view)-tx.GetValueOut();

            std::vector<CScriptCheck> vChecks;
            if (!tx.CheckInputs(state, view, fScriptChecks, flags, nScriptCheckThreads ? &vChecks : NULL))
                return false;
            control.Add(vChecks);
        }

        CTxUndo txundo;
        tx.UpdateCoins(state, view, txundo, pindex->Vcoinh, GetTxHash(i));
        if (!tx.IsCoinBase())
            blockundo.vtxundo.push_back(txundo);

        vPos.push_back(std::make_pair(GetTxHash(i), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }
    int64 nTime = GetTimeMicros() - nStart;
    if (fBenchmark)
        printf("- Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin)\n", (unsigned)vtx.size(), 0.001 * nTime, 0.001 * nTime / vtx.size(), nInputs <= 1 ? 0 : 0.001 * nTime / (nInputs-1));

    if (vtx[0].GetValueOut() > GetBlockValue(pindex->pprev->nBits, pindex->pprev->Vcoinh, nFees))
        return state.DoS(100, error("ConnectBlock() : coinbase pays too much (actual=%"PRI64d" vs limit=%"PRI64d")", vtx[0].GetValueOut(), GetBlockValue(pindex->pprev->nBits, pindex->pprev->Vcoinh, nFees)));

    if (!control.Wait())
        return state.DoS(100, false);
    int64 nTime2 = GetTimeMicros() - nStart;
    if (fBenchmark)
        printf("- Verify %u txins: %.2fms (%.3fms/txin)\n", nInputs - 1, 0.001 * nTime2, nInputs <= 1 ? 0 : 0.001 * nTime2 / (nInputs-1));

    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
    {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos pos;
            if (!FindUndoPos(state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock() : FindUndoPos failed");
            if (!blockundo.WriteToDisk(pos, pindex->pprev->GetBlockHash()))
                return state.Abort(_("Failed to write undo data"));

            // update nUndoPos in block index
            pindex->nUndoPos = pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_SCRIPTS;

        CDiskBlockIndex blockindex(pindex);
        if (!pblocktree->WriteBlockIndex(blockindex))
            return state.Abort(_("Failed to write block index"));
    }

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return state.Abort(_("Failed to write transaction index"));

    // add this block to the view's block chain
    assert(view.SetBestBlock(pindex));

    // Watch for transactions paying to me
    for (unsigned int i=0; i<vtx.size(); i++)
        SyncWithWallets(GetTxHash(i), vtx[i], this, true);

    return true;
}

bool SetBestChain(CValidationState &state, CBlockIndex* pindexNew)
{
    // All modifications to the coin state will be done in this cache.
    // Only when all have succeeded, we push it to pcoinsTip.
    CCoinsViewCache view(*pcoinsTip, true);

    // Find the fork (typically, there is none)
    CBlockIndex* pfork = view.GetBestBlock();
    CBlockIndex* plonger = pindexNew;
    while (pfork && pfork != plonger)
    {
        while (plonger->Vcoinh > pfork->Vcoinh) {
            plonger = plonger->pprev;
            assert(plonger != NULL);
        }
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
        assert(pfork != NULL);
    }

    // List of what to disconnect (typically nothing)
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = view.GetBestBlock(); pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect (typically only pindexNew)
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());

    if (vDisconnect.size() > 0) {
        printf("REORGANIZE: Disconnect %"PRIszu" blocks; %s..\n", vDisconnect.size(), pfork->GetBlockHash().ToString().c_str());
        printf("REORGANIZE: Connect %"PRIszu" blocks; ..%s\n", vConnect.size(), pindexNew->GetBlockHash().ToString().c_str());
    }

    // Disconnect shorter branch
    vector<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect) {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return state.Abort(_("Failed to read block"));
        int64 nStart = GetTimeMicros();
        if (!block.DisconnectBlock(state, pindex, view))
            return error("SetBestBlock() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());
        if (fBenchmark)
            printf("- Disconnect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);

        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            if (!tx.IsCoinBase() && pindex->Vcoinh > Checkpoints::GetTotalBlocksEstimate())
                vResurrect.push_back(tx);
    }

    // Connect longer branch
    vector<CTransaction> vDelete;
    BOOST_FOREACH(CBlockIndex *pindex, vConnect) {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return state.Abort(_("Failed to read block"));
        int64 nStart = GetTimeMicros();
        if (!block.ConnectBlock(state, pindex, view)) {
            if (state.IsInvalid()) {
                InvalidChainFound(pindexNew);
                InvalidBlockFound(pindex);
            }
            return error("SetBestBlock() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());
        }
        if (fBenchmark)
            printf("- Connect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);
    }

    // Flush changes to global coin state
    int64 nStart = GetTimeMicros();
    int nModified = view.GetCacheSize();
    assert(view.Flush());
    int64 nTime = GetTimeMicros() - nStart;
    if (fBenchmark)
        printf("- Flush %i transactions: %.2fms (%.4fms/tx)\n", nModified, 0.001 * nTime, 0.001 * nTime / nModified);

    // Make sure it's successfully written to disk before changing memory structure
    bool fIsInitialDownload = IsInitialBlockDownload();
    if (!fIsInitialDownload || pcoinsTip->GetCacheSize() > nCoinCacheSize) {
        // Typical CCoins structures on disk are around 100 bytes in size.
        // Pushing a new one to the database can cause it to be written
        // twice (once in the log, and once in the tables). This is already
        // an overestimation, as most will delete an existing entry or
        // overwrite one. Still, use a conservative safety factor of 2.
        if (!CheckDiskSpace(100 * 2 * 2 * pcoinsTip->GetCacheSize()))
            return state.Error();
        FlushBlockFile();
        pblocktree->Sync();
        if (!pcoinsTip->Flush())
            return state.Abort(_("Failed to write to coin database"));
    }

    // At this point, all changes have been done to the database.
    // Proceed by updating the memory structures.

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect) {
        // ignore validation errors in resurrected transactions
        CValidationState stateDummy;
        if (!tx.AcceptToMemoryPool(stateDummy, true, false))
            mempool.remove(tx, true);
    }

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete) {
        mempool.remove(tx);
        mempool.removeConflicts(tx);
    }

    // Update best block in wallet (so we can detect restored wallets)
    if ((pindexNew->Vcoinh % 20160) == 0 || (!fIsInitialDownload && (pindexNew->Vcoinh % 144) == 0))
    {
        const CBlockLocator locator(pindexNew);
        ::SetBestChain(locator);
    }

    // New best block
    hashBestChain = pindexNew->GetBlockHash();
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->Vcoinh;
    nBestChainWork = pindexNew->nChainWork;
    nTimeBestReceived = GetTime();
    nTransactionsUpdated++;
    printf("SetBestChain: new best=%s  height=%d  log2_work=%.8g  tx=%lu  date=%s progress=%f\n",
      hashBestChain.ToString().c_str(), nBestHeight, log(nBestChainWork.getdouble())/log(2.0), (unsigned long)pindexNew->nChainTx,
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexBest->GetBlockTime()).c_str(),
      Checkpoints::GuessVerificationProgress(pindexBest));

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            printf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is no longer supported, upgrade to new version from vcoin.ca");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    return true;
}


bool CBlock::AddToBlockIndex(CValidationState &state, const CDiskBlockPos &pos)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return state.Invalid(error("AddToBlockIndex() : %s already exists", hash.ToString().c_str()));

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(*this);
    assert(pindexNew);
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->Vcoinh = pindexNew->pprev->Vcoinh + 1;
    }
    pindexNew->nTx = vtx.size();
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + pindexNew->GetBlockWork().getuint256();
    pindexNew->nChainTx = (pindexNew->pprev ? pindexNew->pprev->nChainTx : 0) + pindexNew->nTx;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus = BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA;
    setBlockIndexValid.insert(pindexNew);

    if (!pblocktree->WriteBlockIndex(CDiskBlockIndex(pindexNew)))
        return state.Abort(_("Failed to write block index"));

    // New best?
    if (!ConnectBestBlock(state))
        return false;

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = GetTxHash(0);
    }

    if (!pblocktree->Flush())
        return state.Abort(_("Failed to sync block index"));

    uiInterface.NotifyBlocksChanged();
    return true;
}


bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos, unsigned int nAddSize, unsigned int Vcoinh, uint64 nTime, bool fKnown = false)
{
    bool fUpdatedLast = false;

    LOCK(cs_LastBlockFile);

    if (fKnown) {
        if (nLastBlockFile != pos.nFile) {
            nLastBlockFile = pos.nFile;
            infoLastBlockFile.SetNull();
            pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile);
            fUpdatedLast = true;
        }
    } else {
        while (infoLastBlockFile.nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            printf("Leaving block file %i: %s\n", nLastBlockFile, infoLastBlockFile.ToString().c_str());
            FlushBlockFile(true);
            nLastBlockFile++;
            infoLastBlockFile.SetNull();
            pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile); // check whether data for the new file somehow already exist; can fail just fine
            fUpdatedLast = true;
        }
        pos.nFile = nLastBlockFile;
        pos.nPos = infoLastBlockFile.nSize;
    }

    infoLastBlockFile.nSize += nAddSize;
    infoLastBlockFile.AddBlock(Vcoinh, nTime);

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (infoLastBlockFile.nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    printf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error();
        }
    }

	// V.I.R.T.U.A.L  C.O.I.N  C.O.D.E

    if (!pblocktree->WriteBlockFileInfo(nLastBlockFile, infoLastBlockFile))
        return state.Abort(_("Failed to write file info"));
    if (fUpdatedLast)
        pblocktree->WriteLastBlockFile(nLastBlockFile);

    return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    if (nFile == nLastBlockFile) {
        pos.nPos = infoLastBlockFile.nUndoSize;
        nNewSize = (infoLastBlockFile.nUndoSize += nAddSize);
        if (!pblocktree->WriteBlockFileInfo(nLastBlockFile, infoLastBlockFile))
            return state.Abort(_("Failed to write block info"));
    } else {
        CBlockFileInfo info;
        if (!pblocktree->ReadBlockFileInfo(nFile, info))
            return state.Abort(_("Failed to read block info"));
        pos.nPos = info.nUndoSize;
        nNewSize = (info.nUndoSize += nAddSize);
        if (!pblocktree->WriteBlockFileInfo(nFile, info))
            return state.Abort(_("Failed to write block info"));
    }

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                printf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        }
        else
            return state.Error();
    }

    return true;
}


bool CBlock::CheckBlock(CValidationState &state, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckVotes) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return state.DoS(100, error("CheckBlock() : size limits failed"));

    // VirtualCoin: Special short-term limits to avoid 10,000 BDB lock limit:
    if (GetBlockTime() < 1376568000)  // stop enforcing 15 August 2013 00:00:00
    {
        // Rule is: #unique txids referenced <= 4,500
        // ... to prevent 10,000 BDB lock exhaustion on old clients
        set<uint256> setTxIn;
        for (size_t i = 0; i < vtx.size(); i++)
        {
            setTxIn.insert(vtx[i].GetHash());
            if (i == 0) continue; // skip coinbase txin
            BOOST_FOREACH(const CTxIn& txin, vtx[i].vin)
                setTxIn.insert(txin.prevout.hash);
        }
        size_t nTxids = setTxIn.size();
        if (nTxids > 4500)
            return error("CheckBlock() : 15 August maxlocks violation");
    }

    // Check proof of work matches claimed amount
    if (fCheckPOW && !CheckProofOfWork(GetPoWHash(), nBits))
        return state.DoS(50, error("CheckBlock() : proof of work failed"));

    // Check timestamp
    if (GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
        return state.Invalid(error("CheckBlock() : block timestamp too far in the future"));

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return state.DoS(100, error("CheckBlock() : first tx is not coinbase"));

    bool MasternodePayments = MasterNodePaymentsOn();
    bool EnforceMasternodePayments = MasterNodePaymentsEnforcing();

    if(MasternodePayments && EnforceMasternodePayments)
    {
        LOCK2(cs_main, mempool.cs);

        CBlockIndex* pindexPrev = NULL;

        CBlock blockTmp;
        int votingRecordsBlockPrev = 0;
        int matchingVoteRecords = 0;
        int badVote = 0;
        int foundMasterNodePayment = 0;
        int removedMasterNodePayments = 0;

        int64 masternodePaymentAmount = vtx[0].GetValueOut()/5;
        bool fIsInitialDownload = IsInitialBlockDownload();
        
        CBlock blockLast;
        
        // Work back to the first block in the orphan chain
        if (mapBlockIndex.count(hashPrevBlock)){
            printf("CheckBlock() : loading prev block  %s\n", hashPrevBlock.ToString().c_str());
            pindexPrev = mapBlockIndex[hashPrevBlock];
            if(!blockLast.ReadFromDisk(pindexPrev)){
                return error("CheckBlock() : Load previous block failed");
            }
        } else {
            // no previous block, can't check votes
            fCheckVotes = false;
            pindexPrev = NULL;
        }

        if (!fCheckVotes) EnforceMasternodePayments = false;

        if (pindexPrev != NULL && !fIsInitialDownload){
            {
                if(blockLast.GetHash() != pindexPrev->GetBlockHash()){
                    printf ("CheckBlock() : blockLast.GetHash() != pindexPrev->GetBlockHash() : %s != %s\n", blockLast.GetHash().ToString().c_str(), pindexPrev->GetBlockHash().ToString().c_str());
                    if(EnforceMasternodePayments) return state.DoS(100, error("CheckBlock() : blockLast.GetHash() != pindexPrev->GetBlockHash()"));
                }

                printf ("CheckBlock() : Vcoinh : %d\n", pindexPrev->Vcoinh);
                printf ("CheckBlock() : pindexPrev->GetBlockHash() : %s\n", pindexPrev->GetBlockHash().ToString().c_str());

                votingRecordsBlockPrev = blockLast.vmn.size();
                BOOST_FOREACH(CMasterNodeVote mv1, blockLast.vmn){
                    if((pindexPrev->Vcoinh+1) - mv1.GetHeight() > MASTERNODE_PAYMENTS_EXPIRATION){
                        if(EnforceMasternodePayments) return state.DoS(100, error("CheckBlock() : Vote too old"));
                    } else if((pindexPrev->Vcoinh+1) - mv1.GetHeight() == MASTERNODE_PAYMENTS_EXPIRATION){
                        removedMasterNodePayments++;
                    } else if(mv1.GetVotes() >= MASTERNODE_PAYMENTS_MIN_VOTES-1 && foundMasterNodePayment < MASTERNODE_PAYMENTS_MAX) {
                        for (unsigned int i = 0; i < vtx[0].vout.size(); i++)
                            if(vtx[0].vout[i].nValue == masternodePaymentAmount && mv1.GetPubKey() == vtx[0].vout[i].scriptPubKey) {
                                foundMasterNodePayment++;
                            } else if(mv1.GetPubKey() == vtx[0].vout[i].scriptPubKey) {
                                printf(" BAD MASTERNODE PAYMENT DETECTED:  %"PRI64u"\n", vtx[0].vout[i].nValue);
                            }
                    } else {
                        BOOST_FOREACH(CMasterNodeVote mv2, vmn){
                            if((mv1.blockHeight == mv2.blockHeight && mv1.GetPubKey() == mv2.GetPubKey())){
                                matchingVoteRecords++;
                                if(mv1.GetVotes() != mv2.GetVotes() && mv1.GetVotes()+1 != mv2.GetVotes()) {
                                    printf(" BAD VOTE DETECTED:  %"PRI64u" %s\n", mv1.blockHeight, mv1.GetPubKey().ToString().c_str());
                                    printf("  -- %d %d\n", mv1.GetVotes(), mv2.GetVotes());
                                    badVote++;
                                }
                            }
                        }
                    }
                }

                printf ("CheckBlock(): votingRecordsBlockPrev %d\n", votingRecordsBlockPrev);
                printf ("CheckBlock(): matchingVoteRecords %d\n", matchingVoteRecords);
                printf ("CheckBlock(): badVote %d\n", badVote);\
                printf ("CheckBlock(): foundMasterNodePayment %d\n", foundMasterNodePayment);
                printf ("CheckBlock(): removedMasterNodePayments %d\n", removedMasterNodePayments);


                //find new votes, must be for this block height
                bool foundThisBlock = false;
                BOOST_FOREACH(CMasterNodeVote mv2, vmn){ 
                    {      
                        std::string blockHeight = boost::lexical_cast<std::string>(mv2.blockHeight);

                        CScript pubkey;
                        pubkey.SetDestination(mv2.pubkey.GetID());
                        CTxDestination address1;
                        ExtractDestination(pubkey, address1);
                        CBitcoinAddress address2(address1);

                        std::string votes = boost::lexical_cast<std::string>(mv2.votes);

                        printf("CheckBlock():  %s       %s       %s\n",  blockHeight.c_str(), address2.ToString().c_str(), votes.c_str());
                    }
                    
                    if(mv2.GetPubKey().size() != 25){
                        return state.DoS(100, error("CheckBlock() : pubkey wrong size"));
                    }

                    bool found = false;
                    if(!foundThisBlock && mv2.blockHeight == pindexPrev->Vcoinh+1) {
                        foundThisBlock = true;
                        continue;
                    }

                    BOOST_FOREACH(CMasterNodeVote mv1, blockLast.vmn){
                        if((mv1.blockHeight == mv2.blockHeight && mv1.GetPubKey() == mv2.GetPubKey())){
                            found = true;
                        }
                    }
                    
                    if(!found){
                        printf("CheckBlock() : pubkey wrong size");
                        if(EnforceMasternodePayments) return state.DoS(100, error("CheckBlock() : Vote not found in previous block"));
                    }
                }
            }
            
            
            if(badVote!=0){
                printf("CheckBlock() : Bad vote detected");
                if(EnforceMasternodePayments) return state.DoS(100, error("CheckBlock() : Bad vote detected"));
            }

            if(matchingVoteRecords+foundMasterNodePayment+removedMasterNodePayments!=votingRecordsBlockPrev) {
                printf("CheckBlock() : Missing masternode votes");
                if(EnforceMasternodePayments) return state.DoS(100, error("CheckBlock() : Missing masternode votes"));
            }

            if(matchingVoteRecords+foundMasterNodePayment>MASTERNODE_PAYMENTS_EXPIRATION){
                printf("CheckBlock() : Too many vote records found");
                if(EnforceMasternodePayments) return state.DoS(100, error("CheckBlock() : Too many vote records found"));
            }
        }
    }

    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return state.DoS(100, error("CheckBlock() : more than one coinbase"));

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!tx.CheckTransaction(state))
            return error("CheckBlock() : CheckTransaction failed");

    // Build the merkle tree already. We need it anyway later, and it makes the
    // block cache the transaction hashes, which means they don't need to be
    // recalculated many times during this block's validation.
    BuildMerkleTree();

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    for (unsigned int i=0; i<vtx.size(); i++) {
        uniqueTx.insert(GetTxHash(i));
    }
    if (uniqueTx.size() != vtx.size())
        return state.DoS(100, error("CheckBlock() : duplicate transaction"), true);

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += tx.GetLegacySigOpCount();
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return state.DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return state.DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"));

    return true;
}

bool CBlock::AcceptBlock(CValidationState &state, CDiskBlockPos *dbp)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return state.Invalid(error("AcceptBlock() : block already in mapBlockIndex"));

    // Get prev block index
    CBlockIndex* pindexPrev = NULL;
    int Vcoinh = 0;
    if (hash != hashGenesisBlock) {
        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("AcceptBlock() : prev block not found"));
        pindexPrev = (*mi).second;
        Vcoinh = pindexPrev->Vcoinh+1;

        if(fTestNet) {
            if (nBits != GetNextWorkRequired(pindexPrev, this))
                return state.DoS(100, error("AcceptBlock() : incorrect proof of work"));
        } else {
            // Check proof of work (Here for the architecture issues with DGW v1 and v2)
            if(Vcoinh <= 45000){
                unsigned int nBitsNext = GetNextWorkRequired(pindexPrev, this);
                double n1 = ConvertBitsToDouble(nBits);
                double n2 = ConvertBitsToDouble(nBitsNext);

                if (abs(n1-n2) > n1*0.2) 
                    return state.DoS(100, error("AcceptBlock() : incorrect proof of work (DGW pre-fork)"));
            } else {
                if (nBits != GetNextWorkRequired(pindexPrev, this))
                    return state.DoS(100, error("AcceptBlock() : incorrect proof of work"));
            }
        }

        // Prevent blocks from too far in the future
        if(fTestNet || Vcoinh >= 45000){
            if (GetBlockTime() > GetAdjustedTime() + 15 * 60) {
                return error("AcceptBlock() : block's timestamp too far in the future");
            }

            // Check timestamp is not too far in the past
            if (GetBlockTime() <= pindexPrev->GetBlockTime() - 15 * 60) {
                return error("AcceptBlock() : block's timestamp is too early compare to last block");
            }
        }

        // Check timestamp against prev
        if (GetBlockTime() <= pindexPrev->GetMedianTimePast())
            return state.Invalid(error("AcceptBlock() : block's timestamp is too early"));

        // Check that all transactions are finalized
        BOOST_FOREACH(const CTransaction& tx, vtx)
            if (!tx.IsFinal(Vcoinh, GetBlockTime()))
                return state.DoS(10, error("AcceptBlock() : contains a non-final transaction"));

        // Check that the block chain matches the known block chain up to a checkpoint
        if (!Checkpoints::CheckBlock(Vcoinh, hash))
            return state.DoS(100, error("AcceptBlock() : rejected by checkpoint lock-in at %d", Vcoinh));

		// Check that the block satisfies synchronized checkpoint
        if (IsSyncCheckpointEnforced() && !IsInitialBlockDownload() && !CheckSyncCheckpoint(hash, pindexPrev))
            return error("AcceptBlock() : rejected by synchronized checkpoint");

        // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
        if (nVersion < 2)
        {
            if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 950, 1000)) ||
                (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 75, 100)))
            {
                return state.Invalid(error("AcceptBlock() : rejected nVersion=1 block"));
            }
        }
        // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
        if (nVersion >= 2)
        {
            // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
            if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 750, 1000)) ||
                (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 51, 100)))
            {
                CScript expect = CScript() << Vcoinh;
                if (vtx[0].vin[0].scriptSig.size() < expect.size() ||
                    !std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
                    return state.DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));
            }
        }
    }

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize+8, Vcoinh, nTime, dbp != NULL))
            return error("AcceptBlock() : FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteToDisk(blockPos))
                return state.Abort(_("Failed to write block"));
        if (!AddToBlockIndex(state, blockPos))
            return error("AcceptBlock() : AddToBlockIndex failed");
    } catch(std::runtime_error &e) {
        return state.Abort(_("System error: ") + e.what());
    }

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    if (hashBestChain == hash)
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
    }

   	// Check pending sync-checkpoint
    AcceptPendingSyncCheckpoint();

    return true;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    // VirtualCoin: temporarily disable v2 block lockin until we are ready for v2 transition
    return false;
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

bool ProcessBlock(CValidationState &state, CNode* pfrom, CBlock* pblock, CDiskBlockPos *dbp)
{
    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
        return state.Invalid(error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->Vcoinh, hash.ToString().c_str()));
    if (mapOrphanBlocks.count(hash))
        return state.Invalid(error("ProcessBlock() : already have block (orphan) %s", hash.ToString().c_str()));

    // Preliminary checks
    if (!pblock->CheckBlock(state))
        return error("ProcessBlock() : CheckBlock FAILED");

    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
    if (pcheckpoint && pblock->hashPrevBlock != hashBestChain)
    {
        if((pblock->GetBlockTime() - pcheckpoint->nTime) < 0) {
            if(pfrom) pfrom->Misbehaving(100);
            return error("ProcessBlock() : block has a time stamp of %lld before the last checkpoint of %u", pblock->GetBlockTime(), pcheckpoint->nTime);
        }
    }

    // Ask for pending sync-checkpoint if any
    if (!IsInitialBlockDownload())
        AskForPendingSyncCheckpoint(pfrom);

    // If we don't already have its previous block, shunt it off to holding area until we get it
    if (pblock->hashPrevBlock != 0 && !mapBlockIndex.count(pblock->hashPrevBlock))
    {
        printf("ProcessBlock: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.ToString().c_str());

        // Accept orphans as long as there is a node to request its parents from
        if (pfrom) {
            CBlock* pblock2 = new CBlock(*pblock);
            mapOrphanBlocks.insert(make_pair(hash, pblock2));
            mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

            // Ask this guy to fill in what we're missing
            if (pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(pblock2)))
                printf("send fill-in getblocks for %s peer=%d\n", hash.ToString().c_str(), pfrom->id);
        }
        return true;
    }

    // Store to disk
    if (!pblock->AcceptBlock(state, dbp))
        return error("ProcessBlock() : AcceptBlock FAILED");

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;
            // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan resolution (that is, feeding people an invalid block based on LegitBlockX in order to get anyone relaying LegitBlockX banned)
            CValidationState stateDummy;
            if (pblockOrphan->AcceptBlock(stateDummy))
                vWorkQueue.push_back(pblockOrphan->GetHash());
            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    //might need to reset pool
    virtualSendPool.NewBlock();

    printf("ProcessBlock: ACCEPTED\n");

    if (pfrom && !CSyncCheckpoint::strMasterPrivKey.empty() &&
        (int)GetArg("-checkpointdepth", -1) >= 0)
        SendSyncCheckpoint(AutoSelectSyncCheckpoint());

    return true;
}








CMerkleBlock::CMerkleBlock(const CBlock& block, CBloomFilter& filter)
{
    header = block.GetBlockHeader();

    vector<bool> vMatch;
    vector<uint256> vHashes;

    vMatch.reserve(block.vtx.size());
    vHashes.reserve(block.vtx.size());

    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        uint256 hash = block.vtx[i].GetHash();
        if (filter.IsRelevantAndUpdate(block.vtx[i], hash))
        {
            vMatch.push_back(true);
            vMatchedTxn.push_back(make_pair(i, hash));
        }
        else
            vMatch.push_back(false);
        vHashes.push_back(hash);
    }

    txn = CPartialMerkleTree(vHashes, vMatch);
}








uint256 CPartialMerkleTree::CalcHash(int height, unsigned int pos, const std::vector<uint256> &vTxid) {
    if (height == 0) {
        // hash at height 0 is the txids themself
        return vTxid[pos];
    } else {
        // calculate left hash
        uint256 left = CalcHash(height-1, pos*2, vTxid), right;
        // calculate right hash if not beyong the end of the array - copy left hash otherwise1
        if (pos*2+1 < CalcTreeWidth(height-1))
            right = CalcHash(height-1, pos*2+1, vTxid);
        else
            right = left;
        // combine subhashes
        return Hash(BEGIN(left), END(left), BEGIN(right), END(right));
    }
}

void CPartialMerkleTree::TraverseAndBuild(int height, unsigned int pos, const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch) {
    // determine whether this node is the parent of at least one matched txid
    bool fParentOfMatch = false;
    for (unsigned int p = pos << height; p < (pos+1) << height && p < nTransactions; p++)
        fParentOfMatch |= vMatch[p];
    // store as flag bit
    vBits.push_back(fParentOfMatch);
    if (height==0 || !fParentOfMatch) {
        // if at height 0, or nothing interesting below, store hash and stop
        vHash.push_back(CalcHash(height, pos, vTxid));
    } else {
        // otherwise, don't store any hash, but descend into the subtrees
        TraverseAndBuild(height-1, pos*2, vTxid, vMatch);
        if (pos*2+1 < CalcTreeWidth(height-1))
            TraverseAndBuild(height-1, pos*2+1, vTxid, vMatch);
    }
}

uint256 CPartialMerkleTree::TraverseAndExtract(int height, unsigned int pos, unsigned int &nBitsUsed, unsigned int &nHashUsed, std::vector<uint256> &vMatch) {
    if (nBitsUsed >= vBits.size()) {
        // overflowed the bits array - failure
        fBad = true;
        return 0;
    }
    bool fParentOfMatch = vBits[nBitsUsed++];
    if (height==0 || !fParentOfMatch) {
        // if at height 0, or nothing interesting below, use stored hash and do not descend
        if (nHashUsed >= vHash.size()) {
            // overflowed the hash array - failure
            fBad = true;
            return 0;
        }
        const uint256 &hash = vHash[nHashUsed++];
        if (height==0 && fParentOfMatch) // in case of height 0, we have a matched txid
            vMatch.push_back(hash);
        return hash;
    } else {
        // otherwise, descend into the subtrees to extract matched txids and hashes
        uint256 left = TraverseAndExtract(height-1, pos*2, nBitsUsed, nHashUsed, vMatch), right;
        if (pos*2+1 < CalcTreeWidth(height-1))
            right = TraverseAndExtract(height-1, pos*2+1, nBitsUsed, nHashUsed, vMatch);
        else
            right = left;
        // and combine them before returning
        return Hash(BEGIN(left), END(left), BEGIN(right), END(right));
    }
}

CPartialMerkleTree::CPartialMerkleTree(const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch) : nTransactions(vTxid.size()), fBad(false) {
    // reset state
    vBits.clear();
    vHash.clear();

    // calculate height of tree
    int Vcoinh = 0;
    while (CalcTreeWidth(Vcoinh) > 1)
        Vcoinh++;

    // traverse the partial tree
    TraverseAndBuild(Vcoinh, 0, vTxid, vMatch);
}

CPartialMerkleTree::CPartialMerkleTree() : nTransactions(0), fBad(true) {}

uint256 CPartialMerkleTree::ExtractMatches(std::vector<uint256> &vMatch) {
    vMatch.clear();
    // An empty set will not work
    if (nTransactions == 0)
        return 0;
    // check for excessively high numbers of transactions
    if (nTransactions > MAX_BLOCK_SIZE / 60) // 60 is the lower bound for the size of a serialized CTransaction
        return 0;
    // there can never be more hashes provided than one for every txid
    if (vHash.size() > nTransactions)
        return 0;
    // there must be at least one bit per node in the partial tree, and at least one node per hash
    if (vBits.size() < vHash.size())
        return 0;
    // calculate height of tree
    int Vcoinh = 0;
    while (CalcTreeWidth(Vcoinh) > 1)
        Vcoinh++;
    // traverse the partial tree
    unsigned int nBitsUsed = 0, nHashUsed = 0;
    uint256 hashMerkleRoot = TraverseAndExtract(Vcoinh, 0, nBitsUsed, nHashUsed, vMatch);
    // verify that no problems occured during the tree traversal
    if (fBad)
        return 0;
    // verify that all bits were consumed (except for the padding caused by serializing it as a byte sequence)
    if ((nBitsUsed+7)/8 != (vBits.size()+7)/8)
        return 0;
    // verify that all hashes were consumed
    if (nHashUsed != vHash.size())
        return 0;
    return hashMerkleRoot;
}







bool AbortNode(const std::string &strMessage) {
    strMiscWarning = strMessage;
    printf("*** %s\n", strMessage.c_str());
    uiInterface.ThreadSafeMessageBox(strMessage, "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool CheckDiskSpace(uint64 nAdditionalBytes)
{
    uint64 nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode(_("Error: Disk space is low!"));

    return true;
}

CCriticalSection cs_LastBlockFile;
CBlockFileInfo infoLastBlockFile;
int nLastBlockFile = 0;

FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        printf("Unable to open file %s\n", path.string().c_str());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            printf("Unable to seek to position %u of %s\n", pos.nPos, path.string().c_str());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

CBlockIndex * InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB()
{
    if (!pblocktree->LoadBlockIndexGuts())
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->Vcoinh, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + pindex->GetBlockWork().getuint256();
        pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS && !(pindex->nStatus & BLOCK_FAILED_MASK))
            setBlockIndexValid.insert(pindex);
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    printf("LoadBlockIndexDB(): last block file = %i\n", nLastBlockFile);
    if (pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile))
        printf("LoadBlockIndexDB(): last block file info: %s\n", infoLastBlockFile.ToString().c_str());

    if (!pblocktree->ReadSyncCheckpoint(hashSyncCheckpoint))
        printf("LoadBlockIndexDB(): synchronized checkpoint not read\n");
    else
        printf("LoadBlockIndexDB(): synchronized checkpoint %s\n", hashSyncCheckpoint.ToString().c_str());

    // Load nBestInvalidWork, OK if it doesn't exist
    CBigNum bnBestInvalidWork;
    pblocktree->ReadBestInvalidWork(bnBestInvalidWork);
    nBestInvalidWork = bnBestInvalidWork.getuint256();

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    printf("LoadBlockIndexDB(): transaction index %s\n", fTxIndex ? "enabled" : "disabled");

    // Load hashBestChain pointer to end of best chain
    pindexBest = pcoinsTip->GetBestBlock();
    if (pindexBest == NULL)
        return true;
    hashBestChain = pindexBest->GetBlockHash();
    nBestHeight = pindexBest->Vcoinh;
    nBestChainWork = pindexBest->nChainWork;

    // set 'next' pointers in best chain
    CBlockIndex *pindex = pindexBest;
    while(pindex != NULL && pindex->pprev != NULL) {
         CBlockIndex *pindexPrev = pindex->pprev;
         pindexPrev->pnext = pindex;
         pindex = pindexPrev;
    }
    printf("LoadBlockIndexDB(): hashBestChain=%s  height=%d date=%s\n",
        hashBestChain.ToString().c_str(), nBestHeight,
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    return true;
}

bool VerifyDB(int nCheckLevel, int nCheckDepth)
{
    if (pindexBest == NULL || pindexBest->pprev == NULL)
        return true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > nBestHeight)
        nCheckDepth = nBestHeight;
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    printf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(*pcoinsTip, true);
    CBlockIndex* pindexState = pindexBest;
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
    for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev)
    {
        boost::this_thread::interruption_point();
        if (pindex->Vcoinh < nBestHeight-nCheckDepth)
            break;
        CBlock block;
        // check level 0: read from disk
        if (!block.ReadFromDisk(pindex))
            return error("VerifyDB() : *** block.ReadFromDisk failed at %d, hash=%s", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !block.CheckBlock(state, true, true, false))
            return error("VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!undo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.GetCacheSize() + pcoinsTip->GetCacheSize()) <= 2*nCoinCacheSize + 32000) {
            bool fClean = true;
            if (!block.DisconnectBlock(state, pindex, coins, &fClean))
                return error("VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
    }
    if (pindexFailure)
        return error("VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", pindexBest->Vcoinh - pindexFailure->Vcoinh + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != pindexBest) {
            boost::this_thread::interruption_point();
            pindex = pindex->pnext;
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                return error("VerifyDB() : *** block.ReadFromDisk failed at %d, hash=%s", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
            if (!block.ConnectBlock(state, pindex, coins))
                return error("VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
        }
    }

    printf("No coin database inconsistencies in last %i blocks (%i transactions)\n", pindexBest->Vcoinh - pindexState->Vcoinh, nGoodTransactions);

    return true;
}

void UnloadBlockIndex()
{
    mapBlockIndex.clear();
    setBlockIndexValid.clear();
    pindexGenesisBlock = NULL;
    nBestHeight = 0;
    nBestChainWork = 0;
    nBestInvalidWork = 0;
    hashBestChain = 0;
    pindexBest = NULL;
}

bool LoadBlockIndex()
{
    if (fTestNet)
    {
//        pchMessageStart[0] = 0xfc;
//        pchMessageStart[1] = 0xc1;
//        pchMessageStart[2] = 0xb7;
//        pchMessageStart[3] = 0xdc;
        hashGenesisBlock = uint256("0x1111111111111111111111111111111111111111111111111111111111111111");
    }

    //
    // Load block index from databases
    //
    if (!fReindex && !LoadBlockIndexDB())
        return false;

    return true;
}


bool InitBlockIndex() {
    // Check whether we're already initialized
    if (pindexGenesisBlock != NULL) {
        // Check whether the master checkpoint key has changed and reset the sync checkpoint if needed.
        if (!CheckCheckpointPubKey())
            return error("LoadBlockIndex() : failed to reset checkpoint master pubkey");  
        return true;
    }

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", false);
    pblocktree->WriteFlag("txindex", fTxIndex);
    printf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {
        // Genesis Block:
        // CBlock(hash=12a765e31ffd4059bada, PoW=0000050c34a64b415b6b, ver=1, hashPrevBlock=00000000000000000000, hashMerkleRoot=97ddfbbae6, nTime=1317972665, nBits=1e0ffff0, nNonce=2084524493, vtx=1)
        //   CTransaction(hash=97ddfbbae6, ver=1, vin.size=1, vout.size=1, nLockTime=0)
        //     CTxIn(COutPoint(0000000000, -1), coinbase 04ffff001d0104404e592054696d65732030352f4f63742f32303131205374657665204a6f62732c204170706c65e280997320566973696f6e6172792c2044696573206174203536)
        //     CTxOut(nValue=50.00000000, scriptPubKey=040184710fa689ad5023690c80f3a4)
        //   vMerkleTree: 97ddfbbae6

        // Genesis block        
        const char* pszTimestamp = "April-12-2014: Virtual Coin Launched Worldwide";
        CTransaction txNew;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 486604799 << CBigNum(4) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].nValue = 50 * COIN;
        txNew.vout[0].scriptPubKey = CScript() << ParseHex("041BE73D303408CF5D2A3CE128E25B11185509CB31D733AD94FF062381B40E5E1CED463510F8D70236C28A071DBE206EE8C4256BECCD38F769096FF3ADB33ACF35") << OP_CHECKSIG;
        CBlock block;
        block.vtx.push_back(txNew);
        block.hashPrevBlock = 0;
        block.hashMerkleRoot = block.BuildMerkleTree();
        block.nVersion = 1;
        block.nTime = 1397260800;
        block.nBits = 0x1e0ffff0;
        block.nNonce = 4589243;
 
        if (fTestNet)
        {
            block.nTime = 1397260801;
            block.nBits = 0x1e0ffff0;
            block.nNonce   = 0;
        }

		
        ///// debug print
        uint256 hash = block.GetHash();
        printf("%s\n", hash.ToString().c_str());
        printf("%s\n", hashGenesisBlock.ToString().c_str());
        printf("%s\n", block.hashMerkleRoot.ToString().c_str());
        if (fTestNet)
        {
            assert(block.hashMerkleRoot == uint256("0x4fec71c084e84581a303133f5a461a852aa121b713025d25daa733dd3d66db19"));
        }
        else
        {
            assert(block.hashMerkleRoot == uint256("0x4fec71c084e84581a303133f5a461a852aa021b713025d25daa733dd3d66db19"));
        }
 
       block.print();
       assert(hash == hashGenesisBlock);

        // Start new block file
        try {
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize+8, 0, block.nTime))
                return error("LoadBlockIndex() : FindBlockPos failed");
            if (!block.WriteToDisk(blockPos))
                return error("LoadBlockIndex() : writing genesis block to disk failed");
            if (!block.AddToBlockIndex(state, blockPos))
                return error("LoadBlockIndex() : genesis block not accepted");
            if (!WriteSyncCheckpoint(hashGenesisBlock))
                return error("LoadBlockIndex() : failed to init sync checkpoint");
        } catch(std::runtime_error &e) {
            return error("LoadBlockIndex() : failed to initialize block database: %s", e.what());
        }
    }

	// If checkpoint master key changed must reset sync-checkpoint
    if (!CheckCheckpointPubKey())
        return error("LoadBlockIndex() : failed to reset checkpoint master pubkey");

    return true;
}



void PrintBlockTree()
{
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        printf("%d (blk%05u.dat:0x%x)  %s  tx %"PRIszu"",
            pindex->Vcoinh,
            pindex->GetBlockPos().nFile, pindex->GetBlockPos().nPos,
            DateTimeStrFormat("%Y-%m-%d %H:%M:%S", block.GetBlockTime()).c_str(),
            block.vtx.size());

        PrintWallets(block);

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos *dbp)
{
    int64 nStart = GetTimeMillis();

    unsigned char pchMessageStart[4];

    int nLoaded = 0;
    try {
        CBufferedFile blkdat(fileIn, 2*MAX_BLOCK_SIZE, MAX_BLOCK_SIZE+8, SER_DISK, CLIENT_VERSION);
        uint64 nStartByte = 0;
        if (dbp) {
            // (try to) skip already indexed part
            CBlockFileInfo info;
            if (pblocktree->ReadBlockFileInfo(dbp->nFile, info)) {
                nStartByte = info.nSize;
                blkdat.Seek(info.nSize);
            }
        }
        uint64 nRewind = blkdat.GetPos();
        while (blkdat.good() && !blkdat.eof()) {
            boost::this_thread::interruption_point();
            GetMessageStart(pchMessageStart);

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[4];
                blkdat.FindByte(pchMessageStart[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, pchMessageStart, 4))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE)
                    continue;
            } catch (std::exception &e) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64 nBlockPos = blkdat.GetPos();
                blkdat.SetLimit(nBlockPos + nSize);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // process block
                if (nBlockPos >= nStartByte) {
                    LOCK(cs_main);
                    if (dbp)
                        dbp->nPos = nBlockPos;
                    CValidationState state;
                    if (ProcessBlock(state, NULL, &block, dbp))
                        nLoaded++;
                    if (state.IsError())
                        break;
                }
            } catch (std::exception &e) {
                printf("%s() : Deserialize or I/O error caught during load\n", __PRETTY_FUNCTION__);
            }
        }
        fclose(fileIn);
    } catch(std::runtime_error &e) {
        AbortNode(_("Error: system error: ") + e.what());
    }
    if (nLoaded > 0)
        printf("Loaded %i blocks from external file in %"PRI64d"ms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}










//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

    // Checkpoint warning
    if (strCheckpointWarning != "")
    {
        nPriority = 900;
        strStatusBar = strCheckpointWarning;
    }

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // Longer invalid proof-of-work chain
    if (pindexBest && nBestInvalidWork > nBestChainWork + (pindexBest->GetBlockWork() * 6).getuint256())
    {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.");
    }

	// If detected invalid checkpoint enter safe mode
    if (hashInvalidCheckpoint != 0)
    {
        nPriority = 3000;
        strStatusBar = strRPC = "WARNING: Inconsistent checkpoint found! Stop enforcing checkpoints and notify developers to resolve the issue.";
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
            bool txInMap = false;
            {
                LOCK(mempool.cs);
                txInMap = mempool.exists(inv.hash);
            }
            return txInMap || mapOrphanTransactions.count(inv.hash) ||
                pcoinsTip->HaveCoins(inv.hash);
        }
    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}

unsigned char pchMessageStart[4] = { 0xfb, 0xc0, 0xb6, 0xdb }; // VirtualCoin: increase each by adding 2 to bitcoin's value.


void static ProcessGetData(CNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // Don't waste work on slow peers until they catch up on the blocks we
        // give them. 80 bytes is just the size of a block header - obviously
        // the minimum we might return.
        if (pfrom->nBlocksRequested * 80 > pfrom->nSendBytes)
            break;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                bool send = true;
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                pfrom->nBlocksRequested++;
                if (mi != mapBlockIndex.end())
                {
                    // If the requested block is at a height below our last
                    // checkpoint, only serve it if it's in the checkpointed chain
                    int Vcoinh = ((*mi).second)->Vcoinh;
                    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
                    if (pcheckpoint && Vcoinh < pcheckpoint->Vcoinh) {
                       if (!((*mi).second)->IsInMainChain())
                       {
                         printf("ProcessGetData(): ignoring request for old block that isn't in the main chain\n");
                         send = false;
                       }
                    }
                } else {
                    send = false;
                }
                if (send)
                {
                    // Send block from disk
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessage("block", block);
                    else // MSG_FILTERED_BLOCK)
                    {
                        LOCK(pfrom->cs_filter);
                        if (pfrom->pfilter)
                        {
                            CMerkleBlock merkleBlock(block, *pfrom->pfilter);
                            pfrom->PushMessage("merkleblock", merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didnt send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            BOOST_FOREACH(PairType& pair, merkleBlock.vMatchedTxn)
                                if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
                                    pfrom->PushMessage("tx", block.vtx[pair.first]);
                        }
                        // else
                            // no response
                    }

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    LOCK(mempool.cs);
                    if (mempool.exists(inv.hash)) {
                        CTransaction tx = mempool.lookup(inv.hash);
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                        pushed = true;
                    }
                }
                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
{
    RandAddSeedPerfmon();
    if (fDebug)
        printf("received: %s (%"PRIszu" bytes)\n", strCommand.c_str(), vRecv.size());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        printf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }




    unsigned char pchMessageStart[4];
    GetMessageStart(pchMessageStart);
    static int64 nTimeLastPrintMessageStart = 0;
    if (fDebug && GetBoolArg("-printmessagestart") && nTimeLastPrintMessageStart + 30 < GetAdjustedTime())
    {
        string strMessageStart((const char *)pchMessageStart);
        vector<unsigned char> vchMessageStart(strMessageStart.begin(), strMessageStart.end());
        printf("ProcessMessages : AdjustedTime=%"PRI64d" MessageStart=%s\n", GetAdjustedTime(), HexStr(vchMessageStart).c_str());
        nTimeLastPrintMessageStart = GetAdjustedTime();
    }


    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->Misbehaving(1);
            return false;
        }

        int64 nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64 nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            printf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString().c_str(), pfrom->nVersion);
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> pfrom->strSubVer;
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        if (!vRecv.empty())
            vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
        else
            pfrom->fRelayTxes = true;

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            printf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        AddTimeData(pfrom->addr, nTime);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));


        if (pfrom->nVersion >= virtualSendPool.MIN_PEER_PROTO_VERSION) {
            if(RequestedMasterNodeList <= 3) {
                bool fIsInitialDownload = IsInitialBlockDownload();
                if(!fIsInitialDownload) {
                    pfrom->PushMessage("dseg");
                    RequestedMasterNodeList++;
                }
            }
        }

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

		// Relay sync-checkpoint
		{
            LOCK(cs_hashSyncCheckpoint);
            if (!checkpointMessage.IsNull())
                checkpointMessage.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        printf("receive version message: %s: version %d, blocks=%d, us=%s, them=%s, peer=%d\n", pfrom->cleanSubVer.c_str(), pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString().c_str(), addrFrom.ToString().c_str(), pfrom->id);

        cPeerBlockCounts.input(pfrom->nStartingHeight);

        if (!IsInitialBlockDownload())
            AskForPendingSyncCheckpoint(pfrom);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        pfrom->Misbehaving(1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }

    else if (strCommand == "misbehave") {
        int howmuch;
        vRecv >> howmuch;
        printf("peer=%d says we are misbehaving %d\n", pfrom->id, howmuch);
    }

    else if (strCommand == "dseg") { //VirtualSend Election Get
        if (pfrom->nVersion < virtualSendPool.MIN_PEER_PROTO_VERSION) {
            return false;
        }

        int count = virtualSendMasterNodes.size()-1;
        int i = 0;

        BOOST_FOREACH(CMasterNode mn, virtualSendMasterNodes) {
            printf("Sending master node entry - %s \n", mn.addr.ToString().c_str());
            mn.Check();
            if(mn.IsEnabled()) {
                pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.now, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen);
                i++;
            }
        }
    }

    else if (strCommand == "dsee") { //VirtualSend Election Entry   
        if (pfrom->nVersion < virtualSendPool.MIN_PEER_PROTO_VERSION) {
            return false;
        }
        bool fIsInitialDownload = IsInitialBlockDownload();
        if(fIsInitialDownload) return true;

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64 sigTime;
        int count;
        int current;
        int64 lastUpdated;
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        
        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            printf("dsee - pubkey the wrong size\n");
            pfrom->Misbehaving(100);
            return false;  
        }

        std::string strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey; 

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubkey2.GetID());
        
        if(pubkeyScript2.size() != 25) {
            printf("dsee - pubkey the wrong size\n");
            pfrom->Misbehaving(100);
            return false;  
        }

        std::string errorMessage = "";
        if(!virtualSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)){
            printf("dsee - Got bad masternode address signature\n");
            pfrom->Misbehaving(100);
            return false;
        }

        if((fTestNet && addr.GetPort() != 19999) || (!fTestNet && addr.GetPort() != 9999)) return true;

        //printf("Searching existing masternodes : %s - %s\n", addr.ToString().c_str(),  vin.ToString().c_str());
        
        BOOST_FOREACH(CMasterNode& mn, virtualSendMasterNodes) {
            //printf(" -- %s\n", mn.vin.ToString().c_str());

            if(mn.vin == vin) {
                if(!mn.UpdatedWithin(MASTERNODE_MIN_MICROSECONDS)){
                    mn.UpdateLastSeen();

                    if(count == -1)
                        RelayVirtualSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated);
                }

                return true;
            }
        }

        printf("dsee - Got NEW masternode entry %s\n", addr.ToString().c_str());

        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(999.99*COIN, virtualSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        if(tx.AcceptableInputs(state, true)){
            printf("dsee - Accepted masternode entry %i %i\n", count, current);

            if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS){
                printf("dsee - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
                pfrom->Misbehaving(20);
                return false;
            }

            addrman.Add(CAddress(addr), pfrom->addr, 2*60*60);

            CMasterNode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2);
            mn.UpdateLastSeen(lastUpdated);
            virtualSendMasterNodes.push_back(mn);

            if(count == -1)
                RelayVirtualSendElectionEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated); 

        } else {
            printf("dsee - Rejected masternode entry\n");
            // if caught up on blocks, then do this:
            //pfrom->Misbehaving(20);
        }
    }

    else if (strCommand == "dseep") { //VirtualSend Election Entry Ping 
        if (pfrom->nVersion < virtualSendPool.MIN_PEER_PROTO_VERSION) {
            return false;
        }
        bool fIsInitialDownload = IsInitialBlockDownload();
        if(fIsInitialDownload) return true;

        CTxIn vin;
        vector<unsigned char> vchSig;
        int64 sigTime;
        bool stop;
        vRecv >> vin >> vchSig >> sigTime >> stop;

        CBlockIndex* pindexPrev = pindexBest;

        if (sigTime/1000000 > GetAdjustedTime() + 15 * 60) {
            printf("dseep: Signature rejected, too far into the future");
            //pfrom->Misbehaving(20);
            return false;
        }

        if (sigTime/1000000 <= pindexPrev->GetBlockTime() - 15 * 60) {
            printf("dseep: Signature rejected, too far into the past");
            //pfrom->Misbehaving(20);
            return false;
        }

        //printf("Searching existing masternodes : %s - %s\n", addr.ToString().c_str(),  vin.ToString().c_str());

        BOOST_FOREACH(CMasterNode& mn, virtualSendMasterNodes) {

            if(mn.vin == vin) {
                std::string strMessage = mn.addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop); 

                std::string errorMessage = "";
                if(!virtualSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)){
                    printf("Got bad masternode address signature\n");
                    //pfrom->Misbehaving(20);
                    return false;
                }

                if(stop) {
                    if(mn.IsEnabled()){
                        mn.Disable();
                        mn.Check();
                        RelayVirtualSendElectionEntryPing(vin, vchSig, sigTime, stop);
                    }
                    return true;
                } else if(!mn.UpdatedWithin(MASTERNODE_MIN_MICROSECONDS)){
                    mn.UpdateLastSeen();
                    RelayVirtualSendElectionEntryPing(vin, vchSig, sigTime, stop);
                    return true;
                }
            }
        }
    }

    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            pfrom->Misbehaving(20);
            return error("message addr size() = %"PRIszu"", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64 nNow = GetAdjustedTime();
        int64 nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64 hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot) {
            printf("OneShot. Disconnecting\n");
            pfrom->fDisconnect = true;
        }
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message inv size() = %"PRIszu"", vInv.size());
        }

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            if (fDebug)
                printf("  got inventory: %s  %s\n", inv.ToString().c_str(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave) {
                if (!fImporting && !fReindex)
                    pfrom->AskFor(inv);
            } else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                if (pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(mapOrphanBlocks[inv.hash])))
                    printf("send getblocks for %s peer=%d\n", inv.hash.ToString().c_str(), pfrom->id);
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                if (pfrom->PushGetBlocks(mapBlockIndex[inv.hash], uint256(0)))
                    printf("send last getblocks for %s peer=%d\n", inv.hash.ToString().c_str(), pfrom->id);
                if (fDebug)
                    printf("force request: %s\n", inv.ToString().c_str());
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message getdata size() = %"PRIszu"", vInv.size());
        }

        if (fDebugNet || (vInv.size() != 1))
            printf("received getdata (%"PRIszu" invsz) peer=%d\n", vInv.size(), pfrom->id);

        if ((fDebugNet && vInv.size() > 0) || (vInv.size() == 1))
            printf("received getdata for: %s peer=%d\n", vInv[0].ToString().c_str(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = locator.GetBlockIndex();

        // Send the rest of the chain
        if (pindex)
            pindex = pindex->pnext;
        int nLimit = 500;
        printf("getblocks %d to %s limit %d peer=%d\n", (pindex ? pindex->Vcoinh : -1), hashStop==uint256(0) ? "0" : hashStop.ToString().c_str(), nLimit, pfrom->id);
        for (; pindex; pindex = pindex->pnext)
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                printf("  getblocks stopping at %d %s\n", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                printf("  getblocks stopping at limit %d %s\n", pindex->Vcoinh, pindex->GetBlockHash().ToString().c_str());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<CBlock> vHeaders;
        int nLimit = 2000;
        printf("getheaders %d to %s\n", (pindex ? pindex->Vcoinh : -1), hashStop.ToString().c_str());
        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CDataStream vMsg(vRecv);
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        bool fMissingInputs = false;
        CValidationState state;
        if (tx.AcceptToMemoryPool(state, true, true, &fMissingInputs))
        {
            RelayTransaction(tx, inv.hash);
            mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            printf("AcceptToMemoryPool: %s %s : accepted %s (poolsz %"PRIszu")\n",
                pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str(),
                tx.GetHash().ToString().c_str(),
                mempool.mapTx.size());

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const uint256& orphanHash = *mi;
                    const CTransaction& orphanTx = mapOrphanTransactions[orphanHash];
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;

                    if (tx.AcceptToMemoryPool(stateDummy, true, true, &fMissingInputs2))
                    {
                        printf("   accepted orphan tx %s\n", orphanHash.ToString().c_str());
                        RelayTransaction(orphanTx, orphanHash);
                        mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanHash));
                        vWorkQueue.push_back(orphanHash);
                        vEraseQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid or too-little-fee orphan
                        vEraseQueue.push_back(orphanHash);
                        printf("   removed orphan tx %s\n", orphanHash.ToString().c_str());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                printf("mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
        {
            printf("%s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
            if (nDoS > 0)
                pfrom->Misbehaving(nDoS);
        }
    }


    else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;

        printf("received block %s peer=%d\n", block.GetHash().ToString().c_str(), pfrom->id);
        // block.print();

        CInv inv(MSG_BLOCK, block.GetHash());
        pfrom->AddInventoryKnown(inv);

        CValidationState state;
        if (ProcessBlock(state, pfrom, &block) || state.CorruptionPossible())
            mapAlreadyAskedFor.erase(inv);
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
            if (nDoS > 0)
                pfrom->Misbehaving(nDoS);
    }


    else if (strCommand == "getaddr")
    {
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        std::vector<uint256> vtxid;
        LOCK2(mempool.cs, pfrom->cs_filter);
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        BOOST_FOREACH(uint256& hash, vtxid) {
            CInv inv(MSG_TX, hash);
            if ((pfrom->pfilter && pfrom->pfilter->IsRelevantAndUpdate(mempool.lookup(hash), hash)) ||
               (!pfrom->pfilter))
                vInv.push_back(inv);
            if (vInv.size() == MAX_INV_SZ)
                break;
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64 nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(10);
            }
        }
    }


    else if (!fBloomFilters &&
             (strCommand == "filterload" ||
              strCommand == "filteradd" ||
              strCommand == "filterclear"))
    {
        pfrom->CloseSocketDisconnect();
        return error("peer %s attempted to set a bloom filter even though we do not advertise that service",
                     pfrom->addr.ToString().c_str());
    }

	else if (strCommand == "checkpoint") // Synchronized checkpoint
    {
        CSyncCheckpoint checkpoint;
        vRecv >> checkpoint;


        if (checkpoint.ProcessSyncCheckpoint(pfrom))
        {
            // Relay
            pfrom->hashCheckpointKnown = checkpoint.hashCheckpoint;
            printf("!!! ENFORCING PAYMENTS %"PRI64u"\n", checkpoint.enforcingPaymentsTime);
            enforceMasternodePaymentsTime = checkpoint.enforcingPaymentsTime;

            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                checkpoint.RelayTo(pnode);
        }
    }

    else if (strCommand == "filterload")
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
            // There is no excuse for sending a too-large filter
            pfrom->Misbehaving(100);
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
        }
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == "filteradd")
    {
        vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
        {
            pfrom->Misbehaving(100);
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter)
                pfrom->pfilter->insert(vData);
            else
                pfrom->Misbehaving(100);
        }
    }


    else if (strCommand == "filterclear")
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else
    {
        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
    //if (fDebug)
    //    printf("ProcessMessages(%zu messages)\n", pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    unsigned char pchMessageStart[4];
    GetMessageStart(pchMessageStart);

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        //if (fDebug)
        //    printf("ProcessMessages(message %u msgsz, %zu bytes, complete:%s)\n",
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, pchMessageStart, sizeof(pchMessageStart)) != 0) {
            printf("\n\nPROCESSMESSAGE: INVALID MESSAGESTART\n\n");
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid())
        {
            printf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand().c_str());
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            printf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
               strCommand.c_str(), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            {
                LOCK(cs_main);
                fRet = ProcessMessage(pfrom, strCommand, vRecv);
            }
            boost::this_thread::interruption_point();
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            printf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand.c_str(), nMessageSize);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
    TRY_LOCK(cs_main, lockMain);
    if (lockMain) {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        // Keep-alive ping. We send a nonce of zero because we don't use it anywhere
        // right now.
        if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSendMsg.empty()) {
            uint64 nonce = 0;
            if (pto->nVersion > BIP0031_VERSION)
                pto->PushMessage("ping", nonce);
            else
                pto->PushMessage("ping");
        }

        // Start block sync
        if (!pto->fAskedForBlocks && !fImporting && !fReindex && !pto->fClient && !pto->fOneShot &&
            !pto->fDisconnect && pto->fSuccessfullyConnected &&
            (pto->nStartingHeight > (nBestHeight - 144)) &&
            (pto->nVersion < NOBLKS_VERSION_START || pto->nVersion >= NOBLKS_VERSION_END)) {
            nAskedForBlocks++;
            pto->fAskedForBlocks = true;
            if (pto->PushGetBlocks(pindexBest, uint256(0)))
                printf("send initial getblocks peer=%d\n", pto->id);
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload())
        {
            ResendWalletTransactions();
        }

        // Address refresh broadcast
        static int64 nLastRebroadcast;
        if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
        {
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (!fNoListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }
                }
            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }


        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    // always trickle our own transactions
                    if (!fTrickleWait)
                    {
                        CWalletTx wtx;
                        if (GetTransaction(inv.hash, wtx))
                            if (wtx.fFromMe)
                                fTrickleWait = true;
                    }

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        //
        // Message: getdata
        //
        vector<CInv> vGetData;
        int64 nNow = GetTime() * 1000000;
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv))
            {
                if (fDebugNet)
                    printf("sending getdata: %s peer=%d\n", inv.ToString().c_str(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}














//////////////////////////////////////////////////////////////////////////////
//
// VirtualCoinMiner
//

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*)pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));
    for (int i = 0; i < 8; i++)
        ((uint32_t*)pstate)[i] = ctx.h[i];
}

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().c_str(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().c_str());
    }
};


uint64 nLastBlockTx = 0;
uint64 nLastBlockSize = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};


CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn)
{
    // Create new block
    auto_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    int payments = 1;
    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1); 
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey = scriptPubKeyIn;

    printf("%d\n", scriptPubKeyIn[0]);


    // start masternode payments


    bool bMasterNodePayment = false;

    // fees to foundation
    if ( fTestNet ){
        if (GetTimeMicros() > START_MASTERNODE_PAYMENTS_TESTNET ){
            bMasterNodePayment = true;
        }
    }else{
        if (GetTimeMicros() > START_MASTERNODE_PAYMENTS){
            bMasterNodePayment = true;
        }
    }
    
    int64 nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CCoinsViewCache view(*pcoinsTip, true);
        CBlockIndex* pindexPrev = pindexBest;
    
        if(bMasterNodePayment) {
            if(!pblock->MasterNodePaymentsEnforcing()){
                int winningNode = virtualSendPool.GetCurrentMasterNode(1);
                if(winningNode >= 0){
                    pblock->payee.SetDestination(virtualSendMasterNodes[winningNode].pubkey.GetID());
                    
                    payments++;
                    txNew.vout.resize(payments);

                    //txNew.vout[0].scriptPubKey = scriptPubKeyIn;
                    txNew.vout[payments-1].scriptPubKey.SetDestination(virtualSendMasterNodes[winningNode].pubkey.GetID());
                    txNew.vout[payments-1].nValue = 0;

                    printf("Masternode payment to %s\n", txNew.vout[payments-1].scriptPubKey.ToString().c_str());
                }
            } else {
                CBlock blockLast;
                if(blockLast.ReadFromDisk(pindexPrev)){
                    BOOST_FOREACH(CMasterNodeVote& mv1, blockLast.vmn){
                        // vote if you agree with it, if you're the last vote you must vote yes to avoid the greedy voter exploit
                        // i.e: You only vote yes when you're not the one that is going to pay
                        if(mv1.GetVotes() >= MASTERNODE_PAYMENTS_MIN_VOTES-1){
                            mv1.Vote();
                        } else {
                            BOOST_FOREACH(CMasterNodeVote& mv2, virtualSendMasterNodeVotes) {
                                if((mv1.blockHeight == mv2.blockHeight && mv1.GetPubKey() == mv2.GetPubKey())) {
                                    mv1.Vote();
                                    break;
                                }
                            }
                        }

                        if (((pindexPrev->Vcoinh+1) - mv1.GetHeight()) >= MASTERNODE_PAYMENTS_EXPIRATION) {
                            // do nothing
                        } else if( (mv1.GetVotes() >= MASTERNODE_PAYMENTS_MIN_VOTES && pblock->MasterNodePaymentsEnforcing()) && payments <= MASTERNODE_PAYMENTS_MAX) {
                            pblock->payee = mv1.GetPubKey();
                            
                            payments++;
                            txNew.vout.resize(payments);

                            //txNew.vout[0].scriptPubKey = scriptPubKeyIn;
                            txNew.vout[payments-1].scriptPubKey = mv1.GetPubKey();
                            txNew.vout[payments-1].nValue = 0;

                            printf("Masternode payment to %s\n", txNew.vout[payments-1].scriptPubKey.ToString().c_str());
                        } else if (((pindexPrev->Vcoinh+1) - mv1.GetHeight()) < MASTERNODE_PAYMENTS_EXPIRATION) {
                            pblock->vmn.push_back(mv1);
                        }
                    } 
                }

                int winningNode = virtualSendPool.GetCurrentMasterNode(1);
                if(winningNode >= 0){
                    CMasterNodeVote mv;
                    mv.Set(virtualSendMasterNodes[winningNode].pubkey, pindexPrev->Vcoinh + 1);
                    pblock->vmn.push_back(mv);
                }
            }
        }

        // Add our coinbase tx as first transaction
        pblock->vtx.push_back(txNew);
        pblocktemplate->vTxFees.push_back(-1); // updated at end
        pblocktemplate->vTxSigOps.push_back(-1); // updated at end

        // end masternode payments


        // Largest block you're willing to create:
        unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
        // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
        nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

        // How much of the block should be dedicated to high-priority transactions,
        // included regardless of the fees they pay
        unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
        nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

        // Minimum block size you want to create; block will be filled with free transactions
        // until there are no more or the block reaches this size:
        unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
        nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

        // Collect memory pool transactions into the block
        {
            // Priority order to process transactions
            list<COrphan> vOrphan; // list memory doesn't move
            map<uint256, vector<COrphan*> > mapDependers;
            bool fPrintPriority = GetBoolArg("-printpriority");

            // This vector will be sorted into a priority queue:
            vector<TxPriority> vecPriority;
            vecPriority.reserve(mempool.mapTx.size());
            for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
            {
                CTransaction& tx = (*mi).second;
                if (tx.IsCoinBase() || !tx.IsFinal())
                    continue;

                COrphan* porphan = NULL;
                double dPriority = 0;
                int64 nTotalIn = 0;
                bool fMissingInputs = false;
                BOOST_FOREACH(const CTxIn& txin, tx.vin)
                {
                    // Read prev transaction
                    if (!view.HaveCoins(txin.prevout.hash))
                    {
                        // This should never happen; all transactions in the memory
                        // pool should connect to either transactions in the chain
                        // or other transactions in the memory pool.
                        if (!mempool.mapTx.count(txin.prevout.hash))
                        {
                            printf("ERROR: mempool transaction missing input %s\n", txin.prevout.hash.ToString().c_str());
                            if (!fTestNet && fDebug) assert("mempool transaction missing input" == 0);
                            fMissingInputs = true;
                            if (porphan)
                                vOrphan.pop_back();
                            break;
                        }

                        // Has to wait for dependencies
                        if (!porphan)
                        {
                            // Use list for automatic deletion
                            vOrphan.push_back(COrphan(&tx));
                            porphan = &vOrphan.back();
                        }
                        mapDependers[txin.prevout.hash].push_back(porphan);
                        porphan->setDependsOn.insert(txin.prevout.hash);
                        nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                        continue;
                    }
                    const CCoins &coins = view.GetCoins(txin.prevout.hash);

                    int64 nValueIn = coins.vout[txin.prevout.n].nValue;
                    nTotalIn += nValueIn;

                    int nConf = pindexPrev->Vcoinh - coins.Vcoinh + 1;

                    dPriority += (double)nValueIn * nConf;
                }
                if (fMissingInputs) continue;

                // Priority is sum(valuein * age) / txsize
                unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
                dPriority /= nTxSize;

                // This is a more accurate fee-per-kilobyte than is used by the client code, because the
                // client code rounds up the size to the nearest 1K. That's good, because it gives an
                // incentive to create smaller transactions.
                double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

                if (porphan)
                {
                    porphan->dPriority = dPriority;
                    porphan->dFeePerKb = dFeePerKb;
                }
                else
                    vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
            }

            // Collect transactions into block
            uint64 nBlockSize = 1000;
            uint64 nBlockTx = 0;
            int nBlockSigOps = 100;
            bool fSortedByFee = (nBlockPrioritySize <= 0);

            TxPriorityCompare comparer(fSortedByFee);
            std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

            // Create coinbase tx
            CTransaction txMerged;

            while (!vecPriority.empty())
            {
                // Take highest priority transaction off the priority queue:
                double dPriority = vecPriority.front().get<0>();
                double dFeePerKb = vecPriority.front().get<1>();
                CTransaction& tx = *(vecPriority.front().get<2>());

                std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
                vecPriority.pop_back();

                // Size limits
                unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
                if (nBlockSize + nTxSize >= nBlockMaxSize)
                    continue;

                // Legacy limits on sigOps:
                unsigned int nTxSigOps = tx.GetLegacySigOpCount();
                if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                    continue;

                // Skip free transactions if we're past the minimum block size:
                if (fSortedByFee && (dFeePerKb < CTransaction::nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                    continue;

                // Prioritize by fee once past the priority size or we run out of high-priority
                // transactions:
                if (!fSortedByFee &&
                    ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 576 / 250)))
                {
                    fSortedByFee = true;
                    comparer = TxPriorityCompare(fSortedByFee);
                    std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
                }

                if (!tx.HaveInputs(view))
                    continue;

                int64 nTxFees = tx.GetValueIn(view)-tx.GetValueOut();

                nTxSigOps += tx.GetP2SHSigOpCount(view);
                if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                    continue;

                CValidationState state;
                if (!tx.CheckInputs(state, view, true, SCRIPT_VERIFY_P2SH))
                    continue;

                CTxUndo txundo;
                uint256 hash = tx.GetHash();
                tx.UpdateCoins(state, view, txundo, pindexPrev->Vcoinh+1, hash);

                // Added
                pblock->vtx.push_back(tx);

                //* END MERGE *//
                pblocktemplate->vTxFees.push_back(nTxFees);
                pblocktemplate->vTxSigOps.push_back(nTxSigOps);
                nBlockSize += nTxSize;
                ++nBlockTx;
                nBlockSigOps += nTxSigOps;
                nFees += nTxFees;

                if (fPrintPriority)
                {
                    printf("priority %.1f feeperkb %.1f txid %s\n",
                           dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
                }

                // Add transactions that depend on this one to the priority queue
                if (mapDependers.count(hash))
                {
                    BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                    {
                        if (!porphan->setDependsOn.empty())
                        {
                            porphan->setDependsOn.erase(hash);
                            if (porphan->setDependsOn.empty())
                            {
                                vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                                std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                            }
                        }
                    }
                }
            }

            nLastBlockTx = nBlockTx;
            nLastBlockSize = nBlockSize;
            printf("CreateNewBlock(): total size %"PRI64u"\n", nBlockSize);

            int64 blockValue = GetBlockValue(pindexPrev->nBits, pindexPrev->Vcoinh, nFees);
            int64 blockValueFifth = blockValue/5;
            
            for(int i = 1; i < payments; i++){
                printf("%d\n", i);
                pblock->vtx[0].vout[i].nValue = blockValueFifth;
                blockValue -= blockValueFifth;
            }
            pblock->vtx[0].vout[0].nValue = blockValue;

            pblocktemplate->vTxFees[0] = -nFees;

            // Fill in header
            pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
            pblock->UpdateTime(pindexPrev);
            pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock);
            pblock->nNonce         = 0;
            pblock->vtx[0].vin[0].scriptSig = CScript() << OP_0 << OP_0;
            pblocktemplate->vTxSigOps[0] = pblock->vtx[0].GetLegacySigOpCount();
            

            CBlockIndex indexDummy(*pblock);
            indexDummy.pprev = pindexPrev;
            indexDummy.Vcoinh = pindexPrev->Vcoinh + 1;
            CCoinsViewCache viewNew(*pcoinsTip, true);
            CValidationState state;
            if (!pblock->ConnectBlock(state, &indexDummy, viewNew, true))
                throw std::runtime_error("CreateNewBlock() : ConnectBlock failed");
        }
    }


    return pblocktemplate.release();
}


CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey)
{
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << pubkey << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey);
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int Vcoinh = pindexPrev->Vcoinh+1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << Vcoinh << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hash = pblock->GetPoWHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if (hash > hashTarget)
        return false;

    //// debug print
    printf("VirtualCoinMiner:\n");
    printf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("VirtualCoinMiner : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[pblock->GetHash()] = 0;
        }

        // Process this block the same as if we had received it from another node
        CValidationState state;
        if (!ProcessBlock(state, NULL, pblock))
            return error("VirtualCoinMiner : ProcessBlock, block not accepted");
    }

    return true;
}

void static VirtualCoinMiner(CWallet *pwallet)
{
    printf("VirtualCoinMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("virtualcoin-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    try { loop {
        while (vNodes.empty())
            MilliSleep(1000);

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrev = pindexBest;

        auto_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey));
        if (!pblocktemplate.get())
            return;
        CBlock *pblock = &pblocktemplate->block;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        printf("Running VirtualCoinMiner with %"PRIszu" transactions in block (%u bytes)\n", pblock->vtx.size(),
               ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

        //
        // Pre-build hash buffers
        //
        char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
        char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
        char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

        FormatHashBuffers(pblock, pmidstate, pdata, phash1);

        unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
        unsigned int& nBlockBits = *(unsigned int*)(pdata + 64 + 8);
        //unsigned int& nBlockNonce = *(unsigned int*)(pdata + 64 + 12);


        //
        // Search
        //
        int64 nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
        loop
        {
            unsigned int nHashesDone = 0;

            uint256 thash;
            loop
            {
                thash = pblock->GetHash();
                if (thash <= hashTarget)
                {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    CheckWork(pblock, *pwallet, reservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    break;
                }
                pblock->nNonce += 1;
                nHashesDone += 1;
                if ((pblock->nNonce & 0xFF) == 0)
                    break;
            }

            // Meter hashes/sec
            static int64 nHashCounter;
            if (nHPSTimerStart == 0)
            {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            }
            else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
                static CCriticalSection cs;
                {
                    LOCK(cs);
                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64 nLogTime;
                        if (GetTime() - nLogTime > 30 * 60)
                        {
                            nLogTime = GetTime();
                            printf("hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            boost::this_thread::interruption_point();
            if (vNodes.empty())
                break;
            if (pblock->nNonce >= 0xffff0000)
                break;
            if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                break;
            if (pindexPrev != pindexBest)
                break;

            // Update nTime every few seconds
            pblock->UpdateTime(pindexPrev);
            nBlockTime = ByteReverse(pblock->nTime);
            if (fTestNet)
            {
                // Changing pblock->nTime can change work required on testnet:
                nBlockBits = ByteReverse(pblock->nBits);
                hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
            }
        }
    } }
    catch (boost::thread_interrupted)
    {
        printf("VirtualCoinMiner terminated\n");
        throw;
    }
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet)
{
    static boost::thread_group* minerThreads = NULL;

    int nThreads = GetArg("-genproclimit", -1);
    if (nThreads < 0)
        nThreads = boost::thread::hardware_concurrency();

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&VirtualCoinMiner, pwallet));
}

// Amount compression:
// * If the amount is 0, output 0
// * first, divide the amount (in base units) by the largest power of 10 possible; call the exponent e (e is max 9)
// * if e<9, the last digit of the resulting number cannot be 0; store it as d, and drop it (divide by 10)
//   * call the result n
//   * output 1 + 10*(9*n + d - 1) + e
// * if e==9, we only know the resulting number is not zero, so output 1 + 10*(n - 1) + 9
// (this is decodable, as d is in [1-9] and e is in [0-9])

uint64 CTxOutCompressor::CompressAmount(uint64 n)
{
    if (n == 0)
        return 0;
    int e = 0;
    while (((n % 10) == 0) && e < 9) {
        n /= 10;
        e++;
    }
    if (e < 9) {
        int d = (n % 10);
        assert(d >= 1 && d <= 9);
        n /= 10;
        return 1 + (n*9 + d - 1)*10 + e;
    } else {
        return 1 + (n - 1)*10 + 9;
    }
}

uint64 CTxOutCompressor::DecompressAmount(uint64 x)
{
    // x = 0  OR  x = 1+10*(9*n + d - 1) + e  OR  x = 1+10*(n - 1) + 9
    if (x == 0)
        return 0;
    x--;
    // x = 10*(9*n + d - 1) + e
    int e = x % 10;
    x /= 10;
    uint64 n = 0;
    if (e < 9) {
        // x = 9*n + d - 1
        int d = (x % 9) + 1;
        x /= 9;
        // x = n
        n = x*10 + d;
    } else {
        n = x+1;
    }
    while (e) {
        n *= 10;
        e--;
    }
    return n;
}


class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        std::map<uint256, CBlockIndex*>::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();

        // orphan blocks
        std::map<uint256, CBlock*>::iterator it2 = mapOrphanBlocks.begin();
        for (; it2 != mapOrphanBlocks.end(); it2++)
            delete (*it2).second;
        mapOrphanBlocks.clear();

        // orphan transactions
        mapOrphanTransactions.clear();
    }
} instance_of_cmaincleanup;


/* *** BEGIN VIRTUALSEND MAGIC - VIRTUALCOIN **********
    Copyright 2014, Virtualcoin Developers 
        Some Dude - dude@vcoin.ca
        InternetApe - kyle@vcoin.ca
*/


bool CVirtualSendPool::SetCollateralAddress(std::string strAddress){
    CBitcoinAddress address;
    if (!address.SetString(strAddress))
    {
        printf("CVirtualSendPool::SetCollateralAddress - Invalid VirtualSend collateral address\n");
        return false;
    }
    collateralPubKey.SetDestination(address.Get());
    return true;
}

//Get last block hash
bool CVirtualSendPool::GetLastValidBlockHash(uint256& hash, int mod)
{
    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->Vcoinh == 0) { return false; }
    
    for (unsigned int i = 1; BlockReading && BlockReading->Vcoinh > 0; i++) {
        if(BlockReading->Vcoinh % mod == 0) {
            hash = BlockReading->GetBlockHash();
            return true;
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;    
}

void CVirtualSendPool::NewBlock()
{
    if(fDebug) printf("CVirtualSendPool::NewBlock \n");

    {    
        LOCK2(cs_main, mempool.cs);
        if(pindexBest != NULL) {
            int winningNode = virtualSendPool.GetCurrentMasterNode(1);
            if(winningNode >= 0){
                CMasterNodeVote mv;
                mv.Set(virtualSendMasterNodes[winningNode].pubkey, pindexBest->Vcoinh + 1);
                virtualSendMasterNodeVotes.push_back(mv);

                if(virtualSendMasterNodeVotes.size() > MASTERNODE_PAYMENTS_EXPIRATION){
                    virtualSendMasterNodeVotes.erase(virtualSendMasterNodeVotes.begin(), virtualSendMasterNodeVotes.end()-MASTERNODE_PAYMENTS_EXPIRATION);
                }
            }
        }
    }
}


uint256 CMasterNode::CalculateScore(int mod)
{
    if(pindexBest == NULL) return 0;

    uint256 n1 = 0;
    if(!virtualSendPool.GetLastValidBlockHash(n1, mod)) return 0;

    uint256 n2 = Hash9(BEGIN(n1), END(n1));
    uint256 n3 = vin.prevout.hash > n2 ? (vin.prevout.hash - n2) : (n2 - vin.prevout.hash);
    
    /*
    printf(" -- MasterNode CalculateScore() n1 = %s \n", n1.ToString().c_str());
    printf(" -- MasterNode CalculateScore() n11 = %u \n", n11);
    printf(" -- MasterNode CalculateScore() n2 = %s \n", n2.ToString().c_str());
    printf(" -- MasterNode CalculateScore() vin = %s \n", vin.prevout.hash.ToString().c_str());
    printf(" -- MasterNode CalculateScore() n3 = %s \n", n3.ToString().c_str());*/

    return n3;
}

int CVirtualSendPool::GetCurrentMasterNode(int mod)
{
    int i = 0;
    unsigned int score = 0;
    int winner = -1;

    BOOST_FOREACH(CMasterNode mn, virtualSendMasterNodes) {
        mn.Check();
        if(!mn.IsEnabled()) {
            i++;
            continue;
        }

        uint256 n = mn.CalculateScore(mod);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        //printf("GetCurrentMasterNode: %d : %s : %u > %u\n", i, mn.addr.ToString().c_str(), n2, score);
        if(n2 > score){
            score = n2;
            winner = i;
        }
        i++;
    }
    //printf("GetCurrentMasterNode: winner %d\n", winner);

    return winner;
}

void CMasterNode::Check()
{
    if(!UpdatedWithin(MASTERNODE_REMOVAL_MICROSECONDS)){
        enabled = 4;
        return;
    }

    if(!UpdatedWithin(MASTERNODE_EXPIRATION_MICROSECONDS)){
        enabled = 2;
        return;
    }

    CValidationState state;
    CTransaction tx = CTransaction();
    CTxOut vout = CTxOut(999.99*COIN, virtualSendPool.collateralPubKey);
    tx.vin.push_back(vin);
    tx.vout.push_back(vout);

    if(!tx.AcceptableInputs(state, true)) {
        enabled = 3;
        return; 
    }

    enabled = 1; // OK
}

bool CVirtualSendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey){
    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strSecret);

    if (!fGood) {
        errorMessage = "Invalid private key";
        return false;
    }     

    key = vchSecret.GetKey();
    pubkey = key.GetPubKey();

    return true;
}

bool CVirtualSendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Sign failed";
        return false;
    }

    return true;
}

bool CVirtualSendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey2;
    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Error recovering pubkey";
        return false;
    }

    return (pubkey2.GetID() == pubkey.GetID());
}