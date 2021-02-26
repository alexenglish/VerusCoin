/********************************************************************
 * (C) 2019 Michael Toutonghi
 * 
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 * 
 * This implements the public blockchains as a service (PBaaS) notarization protocol, VerusLink.
 * VerusLink is a new distributed consensus protocol that enables multiple public blockchains 
 * to operate as a decentralized ecosystem of chains, which can interact and easily engage in cross 
 * chain transactions.
 * 
 */

#include <univalue.h>
#include "main.h"
#include "txdb.h"
#include "rpc/pbaasrpc.h"
#include "transaction_builder.h"

#include <assert.h>

using namespace std;

extern uint160 VERUS_CHAINID;
extern uint160 ASSETCHAINS_CHAINID;
extern string VERUS_CHAINNAME;
extern string PBAAS_HOST;
extern string PBAAS_USERPASS;
extern int32_t PBAAS_PORT;

CNotaryEvidence::CNotaryEvidence(const UniValue &uni)
{
    version = uni_get_int(find_value(uni, "version"));
    type = uni_get_int(find_value(uni, "type"));
    systemID = GetDestinationID(DecodeDestination(uni_get_str(find_value(uni, "systemid"))));
    output = CUTXORef(find_value(uni, "output"));
    confirmed = uni_get_bool(find_value(uni, "confirmed"));
    UniValue sigArr = find_value(uni, "signatures");
    UniValue evidenceArr = find_value(uni, "evidence");
    if (sigArr.isObject())
    {
        auto sigKeys = sigArr.getKeys();
        auto sigValues = sigArr.getValues();
        for (int i = 0; i < sigKeys.size(); i++)
        {
            CTxDestination destKey = DecodeDestination(sigKeys[i]);
            if (destKey.which() != COptCCParams::ADDRTYPE_ID)
            {
                version = VERSION_INVALID;
            }
            signatures.insert(std::make_pair(CIdentityID(GetDestinationID(destKey)), CIdentitySignature(sigValues[i])));
        }
    }
    if (evidenceArr.isArray())
    {
        for (int i = 0; i < evidenceArr.size(); i++)
        {
            evidence.push_back(CPartialTransactionProof(evidenceArr[i]));
        }
    }
}

CIdentitySignature::ESignatureVerification CNotaryEvidence::SignConfirmed(const CKeyStore &keyStore, const CTransaction &txToConfirm, const CIdentityID &signWithID, uint32_t height)
{
    if (signatures.size() && !confirmed)
    {
        LogPrintf("%s: Attempting to change existing signature from rejected to confirmed\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    std::pair<CIdentityMapKey, CIdentityMapValue> keyAndIdentity;
    if (!keyStore.GetIdentity(signWithID, keyAndIdentity, height) && keyAndIdentity.first.CanSign())
    {
        LogPrintf("%s: Attempting to sign with notary ID that this wallet does not control\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    COptCCParams p;

    if (txToConfirm.GetHash() != output.hash ||
        txToConfirm.vout.size() <= output.n ||
        !txToConfirm.vout[output.n].scriptPubKey.IsPayToCryptoCondition(p) ||
        !p.vData.size() ||
        !p.vData[0].size() ||
        p.evalCode == EVAL_NONE)
    {
        LogPrintf("%s: Attempting to sign an invalid or incompatible object\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    // write the object to the hash writer without a vector length prefix
    auto hw = CMMRNode<>::GetHashWriter();
    uint256 objHash = hw.write((const char *)&(p.vData[0][0]), p.vData[0].size()).GetHash();

    CIdentitySignature idSignature;
    CIdentitySignature::ESignatureVerification sigResult = idSignature.NewSignature(keyAndIdentity.second, 
                                std::vector<uint160>({NotaryConfirmedKey()}), 
                                std::vector<uint256>(), 
                                systemID, 
                                height, 
                                "", 
                                objHash, 
                                &keyStore);

    if (sigResult != CIdentitySignature::SIGNATURE_INVALID)
    {
        signatures.insert(std::make_pair(signWithID, idSignature));
    }
    return sigResult;
}

CIdentitySignature::ESignatureVerification CNotaryEvidence::SignRejected(const CKeyStore &keyStore, const CTransaction &txToConfirm, const CIdentityID &signWithID, uint32_t height)
{
    if (signatures.size() && confirmed)
    {
        LogPrintf("%sAttempting to change existing signature from confirmed to rejected\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    std::pair<CIdentityMapKey, CIdentityMapValue> keyAndIdentity;
    if (!keyStore.GetIdentity(signWithID, keyAndIdentity, height) && keyAndIdentity.first.CanSign())
    {
        LogPrintf("%s: Attempting to sign with notary ID that this wallet does not control\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    COptCCParams p;

    if (txToConfirm.GetHash() != output.hash ||
        txToConfirm.vout.size() <= output.n ||
        !txToConfirm.vout[output.n].scriptPubKey.IsPayToCryptoCondition(p) ||
        !p.vData.size() ||
        !p.vData[0].size() ||
        p.evalCode == EVAL_NONE)
    {
        LogPrintf("%s: Attempting to sign an invalid or incompatible object\n", __func__);
        return CIdentitySignature::SIGNATURE_INVALID;
    }

    // write the object to the hash writer without a vector length prefix
    auto hw = CMMRNode<>::GetHashWriter();
    uint256 objHash = hw.write((const char *)&(p.vData[0][0]), p.vData[0].size()).GetHash();

    CIdentitySignature idSignature;

    CIdentitySignature::ESignatureVerification sigResult = idSignature.NewSignature(keyAndIdentity.second, 
                                std::vector<uint160>({NotaryRejectedKey()}), 
                                std::vector<uint256>(), 
                                systemID, 
                                height, 
                                "", 
                                objHash, 
                                &keyStore);

    if (sigResult != CIdentitySignature::SIGNATURE_INVALID)
    {
        signatures.insert(std::make_pair(signWithID, idSignature));
    }
    return sigResult;
}

CPBaaSNotarization::CPBaaSNotarization(const CScript &scriptPubKey) :
                    nVersion(VERSION_INVALID),
                    flags(0),
                    notarizationHeight(0),
                    prevHeight(0)
{
    COptCCParams p;
    if (scriptPubKey.IsPayToCryptoCondition(p) && 
        p.IsValid() &&
        (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
        p.vData.size())
    {
        ::FromVector(p.vData[0], *this);
    }
}

CPBaaSNotarization::CPBaaSNotarization(const CTransaction &tx, int32_t *pOutIdx) :
                    nVersion(VERSION_INVALID),
                    flags(0),
                    notarizationHeight(0),
                    prevHeight(0)
{
    // the PBaaS notarization itself is a combination of proper inputs, one output, and
    // a sequence of opret chain objects as proof of the output values on the chain to which the
    // notarization refers, the opret can be reconstructed from chain data in order to validate
    // the txid of a transaction that does not contain the opret itself

    int32_t _outIdx;
    int32_t &outIdx = pOutIdx ? *pOutIdx : _outIdx;
    
    // a notarization must have notarization output that spends to the address indicated by the 
    // ChainID, an opret, that there is only one, and that it can be properly decoded to a notarization 
    // output, whether or not validate is true
    bool found = false;
    for (int i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) && 
            p.IsValid() &&
            (p.evalCode == EVAL_ACCEPTEDNOTARIZATION || p.evalCode == EVAL_EARNEDNOTARIZATION) &&
            p.vData.size())
        {
            if (found)
            {
                nVersion = VERSION_INVALID;
                proofRoots.clear();
                break;
            }
            else
            {
                found = true;
                outIdx = i;
                ::FromVector(p.vData[0], *this);
            }
        }
    }
}

uint160 ValidateCurrencyName(std::string currencyStr, bool ensureCurrencyValid=false, CCurrencyDefinition *pCurrencyDef=NULL);

CPBaaSNotarization::CPBaaSNotarization(const UniValue &obj)
{
    nVersion = (uint32_t)uni_get_int(find_value(obj, "version"));
    flags = FLAGS_NONE;
    SetDefinitionNotarization(uni_get_bool(find_value(obj, "isdefinition")));
    SetBlockOneNotarization(uni_get_bool(find_value(obj, "isblockonenotarization")));
    SetPreLaunch(uni_get_bool(find_value(obj, "prelaunch")));
    SetLaunchCleared(uni_get_bool(find_value(obj, "launchclear")));
    SetRefunding(uni_get_bool(find_value(obj, "refunding")));
    SetLaunchConfirmed(uni_get_bool(find_value(obj, "launchconfirmed")));

    currencyID = ValidateCurrencyName(uni_get_str(find_value(obj, "currencyid")));
    if (currencyID.IsNull())
    {
        nVersion = VERSION_INVALID;
        return;
    }

    UniValue transferID = find_value(obj, "proposer");
    if (transferID.isObject())
    {
        proposer = CTransferDestination(transferID);
    }

    notarizationHeight = uni_get_int(find_value(obj, "notarizationheight"));
    currencyState = CCoinbaseCurrencyState(find_value(obj, "currencystate"));
    prevNotarization = CUTXORef(uint256S(uni_get_str(find_value(obj, "hashprevnotarizationobject"))), (uint32_t)uni_get_int(find_value(obj, "prevnotarizationout")));
    hashPrevNotarization = uint256S(uni_get_str(find_value(obj, "hashprevnotarizationobject")));
    prevHeight = uni_get_int(find_value(obj, "prevheight"));

    auto curStateArr = find_value(obj, "prevheight");
    auto proofRootArr = find_value(obj, "prevheight");
    auto nodesUni = find_value(obj, "nodes");

    if (curStateArr.isArray())
    {
        for (int i = 0; i < curStateArr.size(); i++)
        {
            std::vector<std::string> keys = curStateArr[i].getKeys();
            std::vector<UniValue> values = curStateArr[i].getValues();
            if (keys.size() != 1 or values.size() != 1)
            {
                nVersion = VERSION_INVALID;
                return;
            }
            currencyStates.insert(std::make_pair(GetDestinationID(DecodeDestination(keys[0])), CCoinbaseCurrencyState(values[0])));
        }
    }

    if (proofRootArr.isArray())
    {
        for (int i = 0; i < proofRootArr.size(); i++)
        {
            std::vector<std::string> keys = proofRootArr[i].getKeys();
            std::vector<UniValue> values = proofRootArr[i].getValues();
            if (keys.size() != 1 or values.size() != 1)
            {
                nVersion = VERSION_INVALID;
                return;
            }
            proofRoots.insert(std::make_pair(GetDestinationID(DecodeDestination(keys[0])), CProofRoot(values[0])));
        }
    }

    if (nodesUni.isArray())
    {
        vector<UniValue> nodeVec = nodesUni.getValues();
        for (auto node : nodeVec)
        {
            nodes.push_back(CNodeData(uni_get_str(find_value(node, "networkaddress")), uni_get_str(find_value(node, "nodeidentity"))));
        }
    }
}

CProofRoot CProofRoot::GetProofRoot(uint32_t blockHeight)
{
    if (blockHeight > chainActive.Height())
    {
        return CProofRoot();
    }
    auto mmv = chainActive.GetMMV();
    mmv.resize(blockHeight);
    return CProofRoot(ASSETCHAINS_CHAINID, 
                      blockHeight, 
                      mmv.GetRoot(), 
                      chainActive[blockHeight]->GetBlockHash(), 
                      chainActive[blockHeight]->chainPower.CompactChainPower());
}

bool CPBaaSNotarization::GetLastNotarization(const uint160 &currencyID,
                                             uint32_t eCode, 
                                             int32_t startHeight,
                                             int32_t endHeight,
                                             uint256 *txIDOut,
                                             CTransaction *txOut)
{
    CPBaaSNotarization notarization;
    std::vector<CAddressIndexDbEntry> notarizationIndex;
    // get the last notarization in the indicated height for this currency, which is valid by definition for a token
    if (GetAddressIndex(CCrossChainRPCData::GetConditionID(currencyID, CPBaaSNotarization::NotaryNotarizationKey()), CScript::P2IDX, notarizationIndex, startHeight, endHeight))
    {
        // filter out all transactions that do not spend from the notarization thread, or originate as the
        // chain definition
        for (auto it = notarizationIndex.rbegin(); it != notarizationIndex.rend(); it++)
        {
            // first unspent notarization that is valid is the one we want, skip spending
            if (it->first.spending)
            {
                continue;
            }
            LOCK(mempool.cs);
            CTransaction oneTx;
            uint256 blkHash;
            if (myGetTransaction(it->first.txhash, oneTx, blkHash))
            {
                if ((notarization = CPBaaSNotarization(oneTx.vout[it->first.index].scriptPubKey)).IsValid())
                {
                    *this = notarization;
                    if (txIDOut)
                    {
                        *txIDOut = it->first.txhash;
                    }
                    if (txOut)
                    {
                        *txOut = oneTx;
                    }
                    break;
                }
            }
            else
            {
                LogPrintf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                printf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                continue;
            }
        }
    }
    return notarization.IsValid();
}

bool CPBaaSNotarization::GetLastUnspentNotarization(const uint160 &currencyID,
                                                    uint32_t eCode, 
                                                    uint256 &txIDOut,
                                                    int32_t &txOutNum,
                                                    CTransaction *txOut)
{
    CPBaaSNotarization notarization;
    std::vector<CAddressUnspentDbEntry> notarizationIndex;
    // get the last notarization in the indicated height for this currency, which is valid by definition for a token
    if (GetAddressUnspent(CCrossChainRPCData::GetConditionID(currencyID, CPBaaSNotarization::NotaryNotarizationKey()), CScript::P2IDX, notarizationIndex))
    {
        // first valid, unspent notarization found is the one we return
        for (auto it = notarizationIndex.rbegin(); it != notarizationIndex.rend(); it++)
        {
            LOCK(mempool.cs);
            CTransaction oneTx;
            uint256 blkHash;
            if (myGetTransaction(it->first.txhash, oneTx, blkHash))
            {
                if ((notarization = CPBaaSNotarization(oneTx.vout[it->first.index].scriptPubKey)).IsValid())
                {
                    *this = notarization;
                    txIDOut = it->first.txhash;
                    txOutNum = it->first.index;
                    if (txOut)
                    {
                        *txOut = oneTx;
                    }
                    break;
                }
            }
            else
            {
                LogPrintf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                printf("%s: error transaction %s not found, may need reindexing\n", __func__, it->first.txhash.GetHex().c_str());
                continue;
            }
        }
    }
    return notarization.IsValid();
}

bool CPBaaSNotarization::NextNotarizationInfo(const CCurrencyDefinition &sourceSystem, 
                                              const CCurrencyDefinition &destCurrency, 
                                              uint32_t lastExportHeight, 
                                              uint32_t currentHeight, 
                                              std::vector<CReserveTransfer> &exportTransfers,       // both in and out. this may refund conversions
                                              uint256 &transferHash,
                                              CPBaaSNotarization &newNotarization,
                                              std::vector<CTxOut> &importOutputs,
                                              CCurrencyValueMap &importedCurrency,
                                              CCurrencyValueMap &gatewayDepositsUsed,
                                              CCurrencyValueMap &spentCurrencyOut) const
{
    uint160 sourceSystemID = sourceSystem.GetID();

    newNotarization = *this;
    newNotarization.SetDefinitionNotarization(false);
    newNotarization.prevNotarization = CUTXORef();
    newNotarization.prevHeight = newNotarization.notarizationHeight;
    newNotarization.notarizationHeight = currentHeight;

    auto hw = CMMRNode<>::GetHashWriter();
    hw << *this;
    newNotarization.hashPrevNotarization = hw.GetHash();

    // if already refunding, numbers don't change
    if (currencyState.IsRefunding())
    {
        return true;
    }

    hw = CMMRNode<>::GetHashWriter();

    for (int i = 0; i < exportTransfers.size(); i++)
    {
        CReserveTransfer &reserveTransfer = exportTransfers[i];

        // add the pre-mutation reserve transfer to the hash
        hw << reserveTransfer;

        // ensure that any pre-conversions or conversions are all valid, based on mined height and
        // maximum pre-conversions
        if (reserveTransfer.IsPreConversion())
        {
            if (lastExportHeight >= destCurrency.startBlock)
            {
                //printf("%s: Invalid pre-conversion, mined after start block\n", __func__);
                LogPrintf("%s: Invalid pre-conversion, mined after start block\n", __func__);
                reserveTransfer = reserveTransfer.GetRefundTransfer();
            }
            else
            {
                // check if it exceeds pre-conversion maximums, and refund if so
                CCurrencyValueMap newReserveIn = CCurrencyValueMap(std::vector<uint160>({reserveTransfer.FirstCurrency()}), 
                                                                   std::vector<int64_t>({reserveTransfer.FirstValue() - CReserveTransactionDescriptor::CalculateConversionFee(reserveTransfer.FirstValue())}));
                CCurrencyValueMap newTotalReserves = CCurrencyValueMap(destCurrency.currencies, newNotarization.currencyState.reserves) + newReserveIn;
                if (destCurrency.maxPreconvert.size() && newTotalReserves > CCurrencyValueMap(destCurrency.currencies, destCurrency.maxPreconvert))
                {
                    LogPrintf("%s: refunding pre-conversion over maximum\n", __func__);
                    reserveTransfer = reserveTransfer.GetRefundTransfer();
                }
            }
        }
        else if (reserveTransfer.IsConversion())
        {
            if (!newNotarization.currencyState.IsLaunchCompleteMarker())
            {
                //printf("%s: Invalid conversion, mined before start block\n", __func__);
                LogPrintf("%s: Invalid conversion, mined before start block\n", __func__);
                reserveTransfer = reserveTransfer.GetRefundTransfer();
            }
        }
    }

    if (exportTransfers.size())
    {
        transferHash = hw.GetHash();
    }

    CReserveTransactionDescriptor rtxd;
    std::vector<CTxOut> dummyImportOutputs;

    // if this is the clear launch notarization after start, make the notarization and determine if we should launch or refund
    if (destCurrency.launchSystemID == sourceSystemID && currentHeight <= (destCurrency.startBlock - 1))
    {
        // we get one pre-launch coming through here, initial supply is set and ready for pre-convert
        // don't revert or emit initial supply, it will be emitted for valid pre-conversions, which must already
        // be included in the currency state
        if (currentHeight == (destCurrency.startBlock - 1) && newNotarization.IsPreLaunch())
        {
            // the first block executes the second time through
            if (newNotarization.IsLaunchCleared())
            {
                newNotarization.SetPreLaunch(false);
                newNotarization.currencyState.SetLaunchClear();
                newNotarization.currencyState.RevertReservesAndSupply();
                newNotarization.currencyState.SetPrelaunch(false);
            }
            else
            {
                newNotarization.SetLaunchCleared();
                newNotarization.currencyState.SetLaunchClear();
                newNotarization.currencyState.RevertReservesAndSupply();
                newNotarization.currencyState.SetPrelaunch(false);

                // first time through is export, second is import, then we finish clearing the launch
                // check if the chain is qualified to launch or should refund
                CCurrencyValueMap minPreMap, fees;
                CCurrencyValueMap preConvertedMap = CCurrencyValueMap(destCurrency.currencies, newNotarization.currencyState.reserves).CanonicalMap();

                if (destCurrency.minPreconvert.size() && destCurrency.minPreconvert.size() == destCurrency.currencies.size())
                {
                    minPreMap = CCurrencyValueMap(destCurrency.currencies, destCurrency.minPreconvert).CanonicalMap();
                }

                if (minPreMap.valueMap.size() && preConvertedMap < minPreMap)
                {
                    // we force the supply to zero
                    // in any case where there was less than minimum participation,
                    newNotarization.currencyState.supply = 0;
                    newNotarization.currencyState.SetRefunding(true);
                    newNotarization.SetRefunding(true);
                }
                else
                {
                    newNotarization.SetLaunchConfirmed();
                    newNotarization.currencyState.SetLaunchConfirmed();
                }
            }
        }
        else if (currentHeight < (destCurrency.startBlock - 1))
        {
            newNotarization.currencyState.SetPrelaunch();
            // if we are about to get the notarization just after the definition notarization,
            // remove the initial contribution amount before continuing
            if (IsDefinitionNotarization())
            {
                if (destCurrency.contributions.size())
                {
                    for (int i = 0; i < destCurrency.contributions.size(); i++)
                    {
                        newNotarization.currencyState.reserves[i] -= destCurrency.contributions[i];
                    }
                }
            }
        }

        CCurrencyDefinition destSystem = ConnectedChains.GetCachedCurrency(destCurrency.systemID);

        CCoinbaseCurrencyState tempState = newNotarization.currencyState;
        bool retVal = rtxd.AddReserveTransferImportOutputs(sourceSystem,
                                                           destSystem,
                                                           destCurrency, 
                                                           newNotarization.currencyState, 
                                                           exportTransfers, 
                                                           importOutputs, 
                                                           importedCurrency,
                                                           gatewayDepositsUsed, 
                                                           spentCurrencyOut,
                                                           &tempState);

        newNotarization.currencyState = tempState;
        return retVal;
    }
    else
    {
        newNotarization.currencyState.SetLaunchCompleteMarker();
        newNotarization.currencyState.SetLaunchClear(false);
        if (destCurrency.systemID != ASSETCHAINS_CHAINID)
        {
            newNotarization.SetSameChain(false);
        }

        // calculate new state from processing all transfers
        // we are not refunding, and it is possible that we also have
        // normal conversions in addition to pre-conversions. add any conversions that may 
        // be present into the new currency state
        bool isValidExport = rtxd.AddReserveTransferImportOutputs(sourceSystem, 
                                                                  ConnectedChains.ThisChain(),
                                                                  destCurrency, 
                                                                  currencyState, 
                                                                  exportTransfers, 
                                                                  dummyImportOutputs, 
                                                                  importedCurrency,
                                                                  gatewayDepositsUsed, 
                                                                  spentCurrencyOut,
                                                                  &newNotarization.currencyState);
        if (!newNotarization.currencyState.IsPrelaunch() && isValidExport && destCurrency.IsFractional())
        {
            // we want the new price and the old state as a starting point to ensure no rounding error impact
            // on reserves
            importedCurrency = CCurrencyValueMap();
            gatewayDepositsUsed = CCurrencyValueMap();
            CCoinbaseCurrencyState tempCurState = currencyState;
            tempCurState.conversionPrice = newNotarization.currencyState.conversionPrice;
            tempCurState.viaConversionPrice = newNotarization.currencyState.viaConversionPrice;
            rtxd = CReserveTransactionDescriptor();
            isValidExport = rtxd.AddReserveTransferImportOutputs(sourceSystem, 
                                                                 ConnectedChains.ThisChain(),
                                                                 destCurrency, 
                                                                 tempCurState, 
                                                                 exportTransfers, 
                                                                 importOutputs, 
                                                                 importedCurrency,
                                                                 gatewayDepositsUsed, 
                                                                 spentCurrencyOut,
                                                                 &newNotarization.currencyState);
            if (isValidExport)
            {
                newNotarization.currencyState.conversionPrice = tempCurState.conversionPrice;
                newNotarization.currencyState.viaConversionPrice = tempCurState.viaConversionPrice;
            }
        }
        if (!isValidExport)
        {
            LogPrintf("%s: invalid export\n", __func__);
            return false;
        }
        return true;
    }

    // based on the last notarization and existing
    return false;
}

CObjectFinalization::CObjectFinalization(const CTransaction &tx, uint32_t *pEcode, int32_t *pFinalizationOutNum)
{
    uint32_t _ecode;
    uint32_t &ecode = pEcode ? *pEcode : _ecode;
    int32_t _finalizeOutNum;
    int32_t &finalizeOutNum = pFinalizationOutNum ? *pFinalizationOutNum : _finalizeOutNum;
    finalizeOutNum = -1;
    for (int i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) && p.IsValid())
        {
            if (p.evalCode == EVAL_FINALIZE_NOTARIZATION || p.evalCode == EVAL_FINALIZE_EXPORT)
            {
                if (finalizeOutNum != -1)
                {
                    this->version = VERSION_INVALID;
                    finalizeOutNum = -1;
                    break;
                }
                else
                {
                    finalizeOutNum = i;
                    ecode = p.evalCode;
                }
            }
        }
    }
}

CChainNotarizationData::CChainNotarizationData(UniValue &obj)
{
    version = (uint32_t)uni_get_int(find_value(obj, "version"));
    UniValue vtxUni = find_value(obj, "vtx");
    if (vtxUni.isArray())
    {
        vector<UniValue> vvtx = vtxUni.getValues();
        for (auto o : vvtx)
        {
            vtx.push_back(make_pair(uint256S(uni_get_str(find_value(o, "txid"))), CPBaaSNotarization(find_value(o, "notarization"))));
        }
    }

    lastConfirmed = (uint32_t)uni_get_int(find_value(obj, "lastconfirmed"));
    UniValue forksUni = find_value(obj, "forks");
    if (forksUni.isArray())
    {
        vector<UniValue> forksVec = forksUni.getValues();
        for (auto fv : forksVec)
        {
            if (fv.isArray())
            {
                forks.push_back(vector<int32_t>());
                vector<UniValue> forkVec = fv.getValues();
                for (auto fidx : forkVec)
                {
                    forks.back().push_back(uni_get_int(fidx));
                }
            }
        }
    }

    bestChain = (uint32_t)uni_get_int(find_value(obj, "bestchain"));
}

UniValue CChainNotarizationData::ToUniValue() const
{
    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("version", (int32_t)version));
    UniValue notarizations(UniValue::VARR);
    for (int64_t i = 0; i < vtx.size(); i++)
    {
        UniValue notarization(UniValue::VOBJ);
        notarization.push_back(Pair("index", i));
        notarization.push_back(Pair("txid", vtx[i].first.hash.GetHex()));
        notarization.push_back(Pair("vout", (int32_t)vtx[i].first.n));
        notarization.push_back(Pair("notarization", vtx[i].second.ToUniValue()));
        notarizations.push_back(notarization);
    }
    obj.push_back(Pair("notarizations", notarizations));
    UniValue Forks(UniValue::VARR);
    for (int32_t i = 0; i < forks.size(); i++)
    {
        UniValue Fork(UniValue::VARR);
        for (int32_t j = 0; j < forks[i].size(); j++)
        {
            Fork.push_back(forks[i][j]);
        }
        Forks.push_back(Fork);
    }
    obj.push_back(Pair("forks", Forks));
    if (IsConfirmed())
    {
        obj.push_back(Pair("lastconfirmedheight", (int32_t)vtx[lastConfirmed].second.notarizationHeight));
    }
    obj.push_back(Pair("lastconfirmed", lastConfirmed));
    obj.push_back(Pair("bestchain", bestChain));
    return obj;
}

bool CPBaaSNotarization::CreateAcceptedNotarization(const CCurrencyDefinition &externalSystem,
                                                    const CPBaaSNotarization &earnedNotarization,
                                                    const CNotaryEvidence &notaryEvidence,
                                                    CValidationState &state,
                                                    TransactionBuilder &txBuilder)
{
    std::string errorPrefix(strprintf("%s: ", __func__));
    std::set<CIdentityID> notaries;

    // now, verify the evidence. accepted notarizations for another system must have at least one
    // valid piece of evidence, which currently means at least one notary signature
    if (!notaryEvidence.signatures.size())
    {
        return state.Error(errorPrefix + "insufficient notary evidence required to accept notarization");
    }
    for (auto &oneSigID : externalSystem.notaries)
    {
        notaries.insert(oneSigID);
    }

    LOCK(cs_main);

    // create an accepted notarization based on the cross-chain notarization provided
    CPBaaSNotarization newNotarization = earnedNotarization;

    // this should be mirrored for us to continue, if it can't be, it is invalid
    if (earnedNotarization.IsMirror() || !newNotarization.SetMirror())
    {
        return state.Error(errorPrefix + "invalid earned notarization");
    }

    uint160 SystemID = externalSystem.GetID();
    uint32_t height = chainActive.Height();
    CProofRoot ourRoot = newNotarization.proofRoots[ASSETCHAINS_CHAINID];

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> txes;
    if (!GetNotarizationData(SystemID, cnd, &txes))
    {
        return state.Error(errorPrefix + "cannot locate notarization history");
    }

    // any notarization submitted must include a proof root of this chain that is later than the last confirmed
    // notarization
    if (!cnd.IsConfirmed() || 
        !cnd.vtx[cnd.lastConfirmed].second.proofRoots.count(ASSETCHAINS_CHAINID) || 
        ourRoot.rootHeight <= cnd.vtx[cnd.lastConfirmed].second.proofRoots.find(ASSETCHAINS_CHAINID)->second.rootHeight)
    {
        return state.Error(errorPrefix + "earned notarization proof root is not later than prior confirmed for this chain");
    }

    auto hw = CMMRNode<>::GetHashWriter();
    hw << earnedNotarization;
    uint256 objHash = hw.GetHash();

    for (auto &oneSig : notaryEvidence.signatures)
    {
        if (!notaries.count(oneSig.first))
        {
            return state.Error(errorPrefix + "unauthorized notary signature");
        }
        CIdentity sigIdentity = CIdentity::LookupIdentity(oneSig.first);
        if (!sigIdentity.IsValidUnrevoked())
        {
            return state.Error(errorPrefix + "invalid notary identity");
        }
        // we currently require accepted notarizations to be completely authorized by notaries
        if (oneSig.second.CheckSignature(sigIdentity,
                                         std::vector<uint160>({notaryEvidence.NotaryConfirmedKey()}), 
                                         std::vector<uint256>(), 
                                         SystemID, 
                                         "", 
                                         objHash) != oneSig.second.SIGNATURE_COMPLETE)
        {
            return state.Error(errorPrefix + "invalid or incomplete notary signature");
        }
    }

    auto mmv = chainActive.GetMMV();
    mmv.resize(ourRoot.rootHeight);

    // we only create accepted notarizations for notarizations that are earned for this chain on another system
    // currently, we support ethereum and PBaaS types.
    if (!newNotarization.proofRoots.count(SystemID) ||
        !newNotarization.proofRoots.count(ASSETCHAINS_CHAINID) ||
        !(ourRoot = newNotarization.proofRoots[ASSETCHAINS_CHAINID]).IsValid() ||
        ourRoot.rootHeight > height ||
        ourRoot.blockHash != chainActive[ourRoot.rootHeight]->GetBlockHash() ||
        ourRoot.stateRoot != mmv.GetRoot() ||
        (ourRoot.type != ourRoot.TYPE_PBAAS && ourRoot.type != ourRoot.TYPE_ETHEREUM))
    {
        return state.Error(errorPrefix + "can only create accepted notarization from notarization with valid proof root of this chain");
    }

    // ensure that the data present is valid, as of the height
    CCoinbaseCurrencyState oldCurState = ConnectedChains.GetCurrencyState(ASSETCHAINS_CHAINID, ourRoot.rootHeight);
    if (!oldCurState.IsValid() ||
        ::GetHash(oldCurState) != ::GetHash(earnedNotarization.currencyState))
    {
        return state.Error(errorPrefix + "currecy state is invalid in accepted notarization. is:\n" + 
                                            newNotarization.currencyState.ToUniValue().write(1,2) + 
                                            "\nshould be:\n" + 
                                            oldCurState.ToUniValue().write(1,2) + "\n");
    }

    // ensure that all locally provable info is valid as of our root height
    // and determine if the new notarization should be already finalized or not
    for (auto &oneCur : newNotarization.currencyStates)
    {
        if (oneCur.first == SystemID)
        {
            continue;
        }
        else if (oneCur.first != ASSETCHAINS_CHAINID)
        {
            // see if this currency is on our chain, and if so, it must be correct as of the proof root of this chain
            CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(oneCur.first);
            // we must have all currencies
            if (!curDef.IsValid())
            {
                return state.Error(errorPrefix + "all currencies in accepted notarizatoin must be registered on this chain");
            }
            // if the currency is not from this chain, we cannot validate it
            if (curDef.systemID != ASSETCHAINS_CHAINID)
            {
                continue;
            }
            // ensure that the data present is valid, as of the height
            oldCurState = ConnectedChains.GetCurrencyState(oneCur.first, ourRoot.rootHeight);
            if (!oldCurState.IsValid() ||
                ::GetHash(oldCurState) != ::GetHash(oneCur.second))
            {
                return state.Error(errorPrefix + "currecy state is invalid in accepted notarization. is:\n" + 
                                                 oneCur.second.ToUniValue().write(1,2) + 
                                                 "\nshould be:\n" + 
                                                 oldCurState.ToUniValue().write(1,2) + "\n");
            }
        }
        else
        {
            return state.Error(errorPrefix + "cannot accept redundant currency state in notarization for " + ConnectedChains.ThisChain().name);
        }
    }
    for (auto &oneRoot : newNotarization.proofRoots)
    {
        if (oneRoot.first == SystemID)
        {
            continue;
        }
        else
        {
            // see if this currency is on our chain, and if so, it must be correct as of the proof root of this chain
            CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(oneRoot.first);
            // we must have all currencies in this notarization registered
            if (!curDef.IsValid())
            {
                return state.Error(errorPrefix + "all currencies in accepted notarizatoin must be registered on this chain");
            }
            uint160 curDefID = curDef.GetID();

            // only check other currencies on this chain, not the main chain itself
            if (curDefID != ASSETCHAINS_CHAINID && curDef.systemID == ASSETCHAINS_CHAINID)
            {
                return state.Error(errorPrefix + "proof roots are not accepted for token currencies");
            }
        }
    }

    // now create the new notarization, add the proof, finalize if appropriate, and finish

    // add spend of prior notarization and then outputs
    CPBaaSNotarization lastUnspentNotarization;
    uint256 lastTxId;
    int32_t lastTxOutNum;
    CTransaction lastTx;
    if (!lastUnspentNotarization.GetLastUnspentNotarization(SystemID, EVAL_ACCEPTEDNOTARIZATION, lastTxId, lastTxOutNum, &lastTx))
    {
        return state.Error(errorPrefix + "invalid prior notarization");
    }

    // add prior unspent accepted notarization as our input
    txBuilder.AddTransparentInput(CUTXORef(lastTxId, lastTxOutNum), lastTx.vout[lastTxOutNum].scriptPubKey, lastTx.vout[lastTxOutNum].nValue);

    CCcontract_info CC;
    CCcontract_info *cp;
    std::vector<CTxDestination> dests;

    // make the earned notarization output
    cp = CCinit(&CC, EVAL_ACCEPTEDNOTARIZATION);

    if (externalSystem.notarizationProtocol == externalSystem.NOTARIZATION_NOTARY_CHAINID)
    {
        dests = std::vector<CTxDestination>({CIdentityID(externalSystem.GetID())});
    }
    else
    {
        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
    }

    txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CPBaaSNotarization>(EVAL_ACCEPTEDNOTARIZATION, dests, 1, &newNotarization)), 0);

    // now add the notary evidence and finalization that uses it to assert validity
    // make the earned notarization output
    cp = CCinit(&CC, EVAL_NOTARY_EVIDENCE);
    dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
    txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &notaryEvidence)), CNotaryEvidence::DEFAULT_OUTPUT_VALUE);

    if (externalSystem.notarizationProtocol != externalSystem.NOTARIZATION_NOTARY_CHAINID)
    {
        // make the finalization output
        cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);
        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

        // we need to store the input that we confirmed if we spent finalization outputs
        CObjectFinalization of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION, VERUS_CHAINID, uint256(), txBuilder.mtx.vout.size(), height + 15);
        if (notaryEvidence.signatures.size() >= externalSystem.notaries.size())
        {
            of.SetConfirmed();
            of.evidenceOutputs.push_back(txBuilder.mtx.vout.size() - 1);
        }
        txBuilder.AddTransparentOutput(MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of)), 0);
    }
    return true;
}

// create a notarization that is validated as part of the block, generally benefiting the miner or staker if the
// cross notarization is valid
bool CPBaaSNotarization::CreateEarnedNotarization(const CRPCChainData &externalSystem,
                                                  const CTransferDestination &Proposer,
                                                  CValidationState &state,
                                                  std::vector<CTxOut> &txOutputs,
                                                  CPBaaSNotarization &notarization)
{
    std::string errorPrefix(strprintf("%s: ", __func__));

    uint32_t height;
    uint160 SystemID;
    const CCurrencyDefinition &systemDef = externalSystem.chainDefinition;
    SystemID = externalSystem.chainDefinition.GetID();

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> txes;

    {
        LOCK2(cs_main, mempool.cs);
        height = chainActive.Height();

        // we can only create an earned notarization for a notary chain, so there must be a notary chain and a network connection to it
        // we also need to ensure that our notarization would be the first notarization in this notary block period  with which we agree.
        if (!externalSystem.IsValid() || externalSystem.rpcHost.empty())
        {
            // technically not a real error
            return state.Error("no-notary");
        }

        if (!GetNotarizationData(SystemID, cnd, &txes))
        {
            return state.Error(errorPrefix + "no prior notarization found");
        }
    }

    // all we really want is the system proof roots for each notarization to make the JSON for the API smaller
    UniValue proofRootsUni(UniValue::VARR);
    for (auto &oneNot : cnd.vtx)
    {
        auto rootIt = oneNot.second.proofRoots.find(SystemID);
        if (rootIt != oneNot.second.proofRoots.end())
        {
            proofRootsUni.push_back(rootIt->second.ToUniValue());
        }
    }

    if (!proofRootsUni.size())
    {
        return state.Error(errorPrefix + "no valid prior state root found");
    }

    // call notary to determine the prior notarization that we agree with
    UniValue params(UniValue::VARR);

    UniValue oneParam(UniValue::VOBJ);
    oneParam.push_back(Pair("proofroots", proofRootsUni));
    oneParam.push_back(Pair("lastconfirmed", cnd.lastConfirmed));
    params.push_back(oneParam);

    //printf("%s: about to get cross notarization with %lu notarizations found\n", __func__, cnd.vtx.size());

    UniValue result;
    try
    {
        result = find_value(RPCCallRoot("getbestproofroot", params), "result");
    } catch (exception e)
    {
        result = NullUniValue;
    }

    int32_t notaryIdx = uni_get_int(find_value(result, "bestproofrootindex"), -1);

    if (result.isNull() || notaryIdx == -1)
    {
        return state.Error(result.isNull() ? "no-notary" : "no-matching-proof-roots-found");
    }

    // now, we have the index for the transaction and notarization we agree with, a list of those we consider invalid,
    // and the most recent notarization to use when creating the new one
    const CTransaction &priorNotarizationTx = txes[notaryIdx].first;
    uint256 priorBlkHash = txes[notaryIdx].second;
    const CUTXORef &priorUTXO = cnd.vtx[notaryIdx].first;
    const CPBaaSNotarization &priorNotarization = cnd.vtx[notaryIdx].second;

    // find out the block height holding the last notarization we agree with
    auto mapBlockIt = mapBlockIndex.find(priorBlkHash);
    if (mapBlockIt == mapBlockIndex.end() || !chainActive.Contains(mapBlockIt->second))
    {
        return state.Error(errorPrefix + "prior notarization not in blockchain");
    }

    // first determine if the prior notarization we agree with would make this one moot
    int blockPeriodNumber = (height + 1) / BLOCK_NOTARIZATION_MODULO;
    int priorBlockPeriod = mapBlockIt->second->GetHeight() / BLOCK_NOTARIZATION_MODULO;

    if (blockPeriodNumber <= priorBlockPeriod)
    {
        return state.Error("ineligible");
    }

    notarization = priorNotarization;
    notarization.proposer = Proposer;
    notarization.notarizationHeight = height;

    // get the latest notarization information for the new, earned notarization
    // one system may provide one proof root and multiple currency states
    CProofRoot latestProofRoot = CProofRoot(find_value(result, "latestproofroot"));
    if (!latestProofRoot.IsValid())
    {
        return state.Error("no-latest-proof-root");
    }
    notarization.proofRoots[SystemID] = latestProofRoot;

    UniValue currencyStatesUni = find_value(result, "currencystates");
    if (!(currencyStatesUni.isArray() && currencyStatesUni.size()))
    {
        return state.Error(errorPrefix + "invalid or missing currency state data from notary");
    }

    // take the lock again, now that we're back from calling out
    LOCK2(cs_main, mempool.cs);

    // if height changed, we need to fail and possibly try again
    if (height != chainActive.Height())
    {
        return state.Error("stale-block");
    }

    notarization.currencyStates.clear();
    for (int i = 0; i < currencyStatesUni.size(); i++)
    {
        CCoinbaseCurrencyState oneCurState(currencyStatesUni[i]);
        CCurrencyDefinition oneCurDef;
        if (!oneCurState.IsValid())
        {
            return state.Error(errorPrefix + "invalid or missing currency state data from notary");
        }
        if (!(oneCurDef = ConnectedChains.GetCachedCurrency(oneCurState.GetID())).IsValid())
        {
            // if we don't have the currency for the state specified, and it isn't critical, ignore
            if (oneCurDef.GetID() == SystemID)
            {
                return state.Error(errorPrefix + "system currency invalid - possible corruption");
            }
            continue;
        }
        if (oneCurDef.systemID == SystemID)
        {
            uint160 oneCurDefID = oneCurDef.GetID();
            if (notarization.currencyID == oneCurDefID)
            {
                notarization.currencyState = oneCurState;
            }
            else
            {
                notarization.currencyStates[oneCurDefID] = oneCurState;
            }
        }
    }

    // add this blockchain's info, based on the requested height
    CBlockIndex &curBlkIndex = *chainActive[height];
    auto mmv = chainActive.GetMMV();
    if (chainActive.Height() != height)
    {
        mmv.resize(height);
    }
    uint160 thisChainID = ConnectedChains.ThisChain().GetID();
    notarization.proofRoots[thisChainID] = CProofRoot(thisChainID, 
                                                      height, 
                                                      mmv.GetRoot(), 
                                                      curBlkIndex.GetBlockHash(), 
                                                      curBlkIndex.chainPower.CompactChainPower(), 
                                                      CProofRoot::TYPE_PBAAS);

    // add currency states that we should include and then we're done
    // currency states to include are either a gateway currency indicated by the
    // gateway or our gateway converter for our PBaaS chain
    uint160 gatewayConverterID;
    if (systemDef.IsGateway() && !systemDef.gatewayConverterName.empty())
    {
        gatewayConverterID = CCurrencyDefinition::GetID(systemDef.gatewayConverterName, thisChainID);
    }
    else if (SystemID == ConnectedChains.FirstNotaryChain().chainDefinition.GetID() && !ConnectedChains.ThisChain().gatewayConverterName.empty())
    {
        gatewayConverterID = CCurrencyDefinition::GetID(ConnectedChains.ThisChain().gatewayConverterName, thisChainID);
    }
    if (!gatewayConverterID.IsNull())
    {
        // get the gateway converter currency from the gateway definition
        CChainNotarizationData gatewayCND;
        if (GetNotarizationData(gatewayConverterID, gatewayCND) && gatewayCND.vtx.size())
        {
            notarization.currencyStates[gatewayConverterID] = gatewayCND.vtx[gatewayCND.lastConfirmed].second.currencyState;
        }
    }

    notarization.prevNotarization = cnd.vtx[notaryIdx].first;
    auto hw = CMMRNode<>::GetHashWriter();
    hw << cnd.vtx[notaryIdx].second;
    notarization.hashPrevNotarization = hw.GetHash();
    notarization.prevHeight = cnd.vtx[notaryIdx].second.notarizationHeight;

    CCcontract_info CC;
    CCcontract_info *cp;
    std::vector<CTxDestination> dests;

    // make the earned notarization output
    cp = CCinit(&CC, EVAL_EARNEDNOTARIZATION);

    if (systemDef.notarizationProtocol == systemDef.NOTARIZATION_NOTARY_CHAINID)
    {
        dests = std::vector<CTxDestination>({CIdentityID(systemDef.GetID())});
    }
    else
    {
        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
    }

    txOutputs.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CPBaaSNotarization>(EVAL_EARNEDNOTARIZATION, dests, 1, &notarization))));

    if (systemDef.notarizationProtocol != systemDef.NOTARIZATION_NOTARY_CHAINID)
    {
        // make the finalization output
        cp = CCinit(&CC, EVAL_FINALIZE_NOTARIZATION);

        dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});

        // we need to store the input that we confirmed if we spent finalization outputs
        CObjectFinalization of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION, VERUS_CHAINID, uint256(), txOutputs.size(), height + 15);
        txOutputs.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of))));
    }
    return true;
}

std::vector<std::pair<uint32_t, CInputDescriptor>> CObjectFinalization::GetUnspentNotaryEvidence() const
{
    LOCK(mempool.cs);
    std::vector<std::pair<uint32_t, CInputDescriptor>> retVal;
    std::vector<CAddressUnspentDbEntry> indexUnspent;
    std::vector<std::pair<CMempoolAddressDeltaKey, CMempoolAddressDelta>> mempoolUnspent;

    uint160 indexKey = CCrossChainRPCData::GetConditionID(currencyID, ObjectFinalizationConfirmedKey());
    if ((GetAddressUnspent(indexKey, CScript::P2IDX, indexUnspent) ||
         mempool.getAddressIndex(std::vector<std::pair<uint160, int32_t>>({{indexKey, CScript::P2IDX}}), mempoolUnspent)) &&
        (indexUnspent.size() || mempoolUnspent.size()))
    {
        for (auto &oneConfirmed : indexUnspent)
        {
            retVal.push_back(std::make_pair(oneConfirmed.second.blockHeight,
                             CInputDescriptor(oneConfirmed.second.script, oneConfirmed.second.satoshis, CTxIn(oneConfirmed.first.txhash, oneConfirmed.first.index))));
        }
        for (auto &oneUnconfirmed : mempoolUnspent)
        {
            auto txProxy = mempool.mapTx.find(oneUnconfirmed.first.txhash);
            if (txProxy != mempool.mapTx.end())
            {
                auto &mpEntry = *txProxy;
                auto &tx = mpEntry.GetTx();
                retVal.push_back(std::make_pair(0,
                                 CInputDescriptor(tx.vout[oneUnconfirmed.first.index].scriptPubKey,
                                                  tx.vout[oneUnconfirmed.first.index].nValue, 
                                                  CTxIn(oneUnconfirmed.first.txhash, oneUnconfirmed.first.index))));
            }
        }
    }
    return retVal;
}

// this is called by notaries to locate any notarizations of a specific system that they can notarize, to determine if we
// agree with the notarization ni question, and to confirm or reject the notarization
bool CPBaaSNotarization::ConfirmOrRejectNotarizations(const CWallet *pWallet,
                                                      const CRPCChainData &externalSystem,
                                                      CValidationState &state,
                                                      TransactionBuilder &txBuilder,
                                                      bool &finalized)
{
    std::string errorPrefix(strprintf("%s: ", __func__));

    finalized = false;

    CChainNotarizationData cnd;
    std::vector<std::pair<CTransaction, uint256>> txes;

    uint32_t height;
    uint160 SystemID = externalSystem.chainDefinition.GetID();

    std::vector<std::pair<CIdentityMapKey, CIdentityMapValue>> mine;
    {
        std::vector<std::pair<CIdentityMapKey, CIdentityMapValue>> imsigner, watchonly;
        LOCK(pWallet->cs_wallet);
        // sign with all IDs under our control that are eligible for this currency
        pWallet->GetIdentities(externalSystem.chainDefinition.notaries, mine, imsigner, watchonly);
        if (!mine.size())
        {
            return state.Error("no-notary");
        }
    }

    {
        LOCK2(cs_main, mempool.cs);
        height = chainActive.Height();

        // we can only create an earned notarization for a notary chain, so there must be a notary chain and a network connection to it
        // we also need to ensure that our notarization would be the first notarization in this notary block period  with which we agree.
        if (!externalSystem.IsValid() || externalSystem.rpcHost.empty())
        {
            // technically not a real error
            return state.Error("no-notary");
        }

        if (!GetNotarizationData(SystemID, cnd, &txes))
        {
            return state.Error(errorPrefix + "no prior notarization found");
        }
    }

    // all we really want is the system proof roots for each notarization to make the JSON for the API smaller
    UniValue proofRootsUni(UniValue::VARR);
    for (auto &oneNot : cnd.vtx)
    {
        auto rootIt = oneNot.second.proofRoots.find(SystemID);
        if (rootIt != oneNot.second.proofRoots.end())
        {
            proofRootsUni.push_back(rootIt->second.ToUniValue());
        }
    }

    if (!proofRootsUni.size())
    {
        return state.Error(errorPrefix + "no valid prior state root found");
    }

    UniValue firstParam(UniValue::VOBJ);
    firstParam.push_back(Pair("proofroots", proofRootsUni));
    firstParam.push_back(Pair("lastconfirmed", cnd.lastConfirmed));

    // call notary to determine the notarization that we should notarize
    UniValue params(UniValue::VARR);
    params.push_back(firstParam);

    //printf("%s: about to get cross notarization with %lu notarizations found\n", __func__, cnd.vtx.size());

    UniValue result;
    try
    {
        result = find_value(RPCCallRoot("getbestproofroot", params), "result");
    } catch (exception e)
    {
        result = NullUniValue;
    }

    int32_t notaryIdx = uni_get_int(find_value(result, "bestproofrootindex"), -1);

    if (result.isNull() || notaryIdx == -1)
    {
        return state.Error(result.isNull() ? "no-notary" : "no-matching-notarization-found");
    }

    // take the lock again, now that we're back from calling out
    LOCK2(cs_main, mempool.cs);

    // if height changed, we need to fail and possibly try again later
    if (height != chainActive.Height())
    {
        return state.Error("stale-block");
    }

    // now, get the list of unconfirmed matches, and sign the latest one that
    // may be signed
    UniValue proofRootArr = find_value(result, "validproofroots");
    if (!proofRootArr.isArray() || !proofRootArr.size())
    {
        return state.Error("no-valid-unconfirmed");
    }

    // latest height we are eligible to notarize
    uint32_t eligibleHeight = height - CPBaaSNotarization::MIN_BLOCKS_BEFORE_NOTARY_FINALIZED;

    bool retVal = false;

    // look from the latest notarization that may qualify
    for (int i = proofRootArr.size() - 1; i >= 0; i--)
    {
        int idx = uni_get_int(proofRootArr[i]);
        if (cnd.vtx[idx].second.notarizationHeight <= eligibleHeight)
        {
            // this is the one we will notarize
            std::set<CInputDescriptor> myIDSigs;

            std::set<CIdentityID> myIDSet;
            for (auto &oneID : mine)
            {
                myIDSet.insert(oneID.first.idID);
            }

            // before signing the one we are about to, we want to ensure that it isn't already signed sufficiently
            // if there are enough signatures to confirm it with out signature, make our signature, then create a finalization
            CObjectFinalization of = CObjectFinalization(CObjectFinalization::FINALIZE_NOTARIZATION + CObjectFinalization::FINALIZE_CONFIRMED,
                                                         SystemID,
                                                         cnd.vtx[idx].first.hash,
                                                         cnd.vtx[idx].first.n,
                                                         eligibleHeight);

            std::vector<std::pair<uint32_t, CInputDescriptor>> evidenceOuts = of.GetUnspentNotaryEvidence();
            std::set<CInputDescriptor> additionalEvidence;
            std::set<CInputDescriptor> evidenceToSpend;

            std::set<uint160> sigSet;

            // if we might have a confirmed notarization, verify, then post
            for (auto &oneEvidenceOut : evidenceOuts)
            {
                COptCCParams p;
                CNotaryEvidence evidence;
                if (oneEvidenceOut.second.scriptPubKey.IsPayToCryptoCondition(p) &&
                    p.IsValid() &&
                    p.evalCode == EVAL_NOTARY_EVIDENCE &&
                    p.vData.size() &&
                    (evidence = CNotaryEvidence(p.vData[0])).IsValid() &&
                    evidence.IsNotarySignature())
                {
                    if (CUTXORef(evidence.output.hash.IsNull() ? oneEvidenceOut.second.txIn.prevout.hash : evidence.output.hash, evidence.output.n) == of.output &&
                        evidence.signatures.size())
                    {
                        bool hasOurSig = false;
                        for (auto &oneSig : evidence.signatures)
                        {
                            sigSet.insert(oneSig.first);
                            if (myIDSet.count(oneSig.first))
                            {
                                hasOurSig = true;
                                myIDSet.erase(oneSig.first);
                            }
                        }
                        if (hasOurSig)
                        {
                            myIDSigs.insert(oneEvidenceOut.second);
                        }
                        else
                        {
                            additionalEvidence.insert(oneEvidenceOut.second);
                        }
                        
                    }
                    else
                    {
                        evidenceToSpend.insert(oneEvidenceOut.second);
                    }
                }
            }

            if (evidenceOuts.size() >= (externalSystem.chainDefinition.minNotariesConfirm - 1))
            {
            }

            // we've already signed
            if (!myIDSet.size())
            {
                return state.Error("ineligible");
            }

            CCcontract_info CC;
            CCcontract_info *cp;
            std::vector<CTxDestination> dests;

            // make the earned notarization output
            cp = CCinit(&CC, EVAL_NOTARY_EVIDENCE);
            dests = std::vector<CTxDestination>({CPubKey(ParseHex(CC.CChexstr))});
            CNotaryEvidence ne = CNotaryEvidence(ASSETCHAINS_CHAINID, cnd.vtx[idx].first);

            {
                LOCK(pWallet->cs_wallet);
                // sign with all IDs under our control that are eligible for this currency
                for (auto &oneID : myIDSet)
                {
                    auto signResult = ne.SignConfirmed(*pWallet, txes[idx].first, oneID, height);
                    if (signResult == CIdentitySignature::SIGNATURE_PARTIAL || signResult == CIdentitySignature::SIGNATURE_COMPLETE)
                    {
                        sigSet.insert(oneID);
                        retVal = true;
                        // if our signatures altogether have provided a complete validation, we can early out
                        if ((ne.signatures.size() + myIDSigs.size()) >= externalSystem.chainDefinition.minNotariesConfirm)
                        {
                            break;
                        }
                    }
                    else
                    {
                        return state.Error(errorPrefix + "invalid identity signature");
                    }
                }
            }

            if (ne.signatures.size())
            {
                CScript evidenceScript = MakeMofNCCScript(CConditionObj<CNotaryEvidence>(EVAL_NOTARY_EVIDENCE, dests, 1, &ne));
                myIDSigs.insert(CInputDescriptor(evidenceScript, 0, CTxIn(COutPoint(uint256(), txBuilder.mtx.vout.size()))));
                txBuilder.AddTransparentOutput(evidenceScript, CNotaryEvidence::DEFAULT_OUTPUT_VALUE);
            }

            // if we have enough to finalize, do so and include all of our signatures allowed
            if (sigSet.size() >= externalSystem.chainDefinition.minNotariesConfirm)
            {
                int sigCount = 0;

                // include all of our signatures to improve chances of reward
                if (ne.signatures.size())
                {
                    of.evidenceOutputs.push_back(txBuilder.mtx.vout.size() - 1);
                    sigCount += ne.signatures.size();
                }

                // spend all priors, and if we need more the new signatures, add them to the finalization evidence
                // prioritizing our signatures
                bool haveNeeded = sigCount >= externalSystem.chainDefinition.minNotariesConfirm;
                for (auto &oneEvidenceOut : myIDSigs)
                {
                    // use up evidence with our ID signatures first, and remove from the remainder
                    COptCCParams p;
                    CNotaryEvidence evidence;
                    // validated above
                    oneEvidenceOut.scriptPubKey.IsPayToCryptoCondition(p);
                    evidence = CNotaryEvidence(p.vData[0]);
                    for (auto &oneSig : evidence.signatures)
                    {
                        if (sigSet.count(oneSig.first))
                        {
                            sigCount++;
                            sigSet.erase(oneSig.first);
                        }
                    }
                    txBuilder.AddTransparentInput(oneEvidenceOut.txIn.prevout, oneEvidenceOut.scriptPubKey, oneEvidenceOut.nValue);
                    if (!haveNeeded)
                    {
                        // until we have enough signatures to confirm, continue to add evidence to the finalization
                        of.evidenceInputs.push_back(txBuilder.mtx.vin.size() - 1);
                        haveNeeded = sigCount >= externalSystem.chainDefinition.minNotariesConfirm;
                    }
                }
                // if we still need more confirmation, add it
                if (!haveNeeded)
                {
                    for (auto &oneEvidenceOut : additionalEvidence)
                    {
                        // use up evidence with our ID signatures first, and remove from the remainder
                        COptCCParams p;
                        CNotaryEvidence evidence;
                        // validated above
                        oneEvidenceOut.scriptPubKey.IsPayToCryptoCondition(p);
                        evidence = CNotaryEvidence(p.vData[0]);
                        for (auto &oneSig : evidence.signatures)
                        {
                            if (sigSet.count(oneSig.first))
                            {
                                sigCount++;
                                sigSet.erase(oneSig.first);
                            }
                        }
                        txBuilder.AddTransparentInput(oneEvidenceOut.txIn.prevout, oneEvidenceOut.scriptPubKey, oneEvidenceOut.nValue);
                        if (!haveNeeded)
                        {
                            // until we have enough signatures to confirm, continue to add evidence to the finalization
                            of.evidenceInputs.push_back(txBuilder.mtx.vin.size() - 1);
                            haveNeeded = sigCount >= externalSystem.chainDefinition.minNotariesConfirm;
                        }
                    }
                }
                
                if (!haveNeeded)
                {
                    // should never get here
                    return state.Error(errorPrefix + "Internal error");
                }

                finalized = true;

                CScript finalizeScript = MakeMofNCCScript(CConditionObj<CObjectFinalization>(EVAL_FINALIZE_NOTARIZATION, dests, 1, &of));
                txBuilder.AddTransparentOutput(finalizeScript, 0);

                // spend all remaining, unnecessary bits of notary evidence for prior finalizations
                for (auto &oneEvidenceOut : evidenceToSpend)
                {
                    txBuilder.AddTransparentInput(oneEvidenceOut.txIn.prevout, oneEvidenceOut.scriptPubKey, oneEvidenceOut.nValue);
                }
            }
        }
    }
    return retVal;
}


/*
 * Validates a notarization output spend by ensuring that the spending transaction fulfills all requirements.
 * to accept an earned notarization as valid on the Verus blockchain, it must prove a transaction on the alternate chain, which is 
 * either the original chain definition transaction, which CAN and MUST be proven ONLY in block 1, or the latest notarization transaction 
 * on the alternate chain that represents an accurate MMR for this chain.
 * In addition, any accepted notarization must fullfill the following requirements:
 * 1) Must prove either a PoS block from the alternate chain or a merge mined block that is owned by the submitter and in either case, 
 *    the block must be exactly 8 blocks behind the submitted MMR used for proof.
 * 2) Must prove a chain definition tx and be block 1 or asserts a previous, valid MMR for the notarizing
 *    chain and properly prove objects using that MMR.
 * 3) Must spend the main notarization thread as well as any finalization outputs of either valid or invalid prior
 *    notarizations, and any unspent notarization contributions for this era. May also spend other inputs.
 * 4) Must output:
 *      a) finalization output of the expected reward amount, which will be sent when finalized
 *      b) normal output of reward from validated/finalized input if present, 50% to recipient / 50% to block miner less miner fee this tx
 *      c) main notarization thread output with remaining funds, no other output or fee deduction
 * 
 */
bool ValidateAcceptedNotarization(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // TODO: this validates the spending transaction
    // check the following things:
    // 1. It represents a valid PoS or merge mined block on the other chain, and contains the header in the opret
    // 2. The MMR and proof provided for the currently asserted block can prove the provided header. The provided
    //    header can prove the last block referenced.
    // 3. This notarization is not a superset of an earlier notarization posted before it that it does not
    //    reference. If that is the case, it is rejected.
    // 4. Has all relevant inputs, including finalizes all necessary transactions, both confirmed and orphaned
    //printf("ValidateAcceptedNotarization\n");
    return true;
}

bool IsAcceptedNotarizationInput(const CScript &scriptSig)
{
    uint32_t ecode;
    return scriptSig.IsPayToCryptoCondition(&ecode) && ecode == EVAL_ACCEPTEDNOTARIZATION;
}


/*
 * Ensures that a spend in an earned notarization of either an OpRet support transaction or summary notarization
 * are valid with respect to this chain. Any transaction that spends from an opret trasaction is either disconnected,
 * or contains the correct hashes of each object and transaction data except for the opret, which can be validated by
 * reconstructing the opret from the hashes on the other chain and verifying that it hashes to the same input value. This
 * enables full validation without copying redundant data back to its original chain.
 * 
 * In addition, each earned notarization must reference the last earned notarization with which it agrees and prove the last
 * accepted notarization on the alternate chain with the latest MMR. The earned notarization will not be accepted if there is
 * a later notarization that agrees with it already present in the alternate chain when it is submitted. 
 * 
 */
bool ValidateEarnedNotarization(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // this needs to validate that the block is mined or staked, that the notarization is properly formed,
    // cryptographically correct, and that it spends the proper finalization outputs
    // if the notarization causes a fork, it must include additional proof of blocks and their
    // power based on random block hash bits
    //printf("ValidateEarnedNotarization\n");
    return true;
}
bool IsEarnedNotarizationInput(const CScript &scriptSig)
{
    // this is an output check, and is incorrect. need to change to input
    uint32_t ecode;
    return scriptSig.IsPayToCryptoCondition(&ecode) && ecode == EVAL_EARNEDNOTARIZATION;
}

CObjectFinalization GetOldFinalization(const CTransaction &spendingTx, uint32_t nIn, CTransaction *pSourceTx=nullptr, uint32_t *pHeight=nullptr);
CObjectFinalization GetOldFinalization(const CTransaction &spendingTx, uint32_t nIn, CTransaction *pSourceTx, uint32_t *pHeight)
{
    CTransaction _sourceTx;
    CTransaction &sourceTx(pSourceTx ? *pSourceTx : _sourceTx);

    CObjectFinalization oldFinalization;
    uint256 blkHash;
    if (myGetTransaction(spendingTx.vin[nIn].prevout.hash, sourceTx, blkHash))
    {
        if (pHeight)
        {
            auto bIt = mapBlockIndex.find(blkHash);
            if (bIt == mapBlockIndex.end() || !bIt->second)
            {
                *pHeight = chainActive.Height();
            }
            else
            {
                *pHeight = bIt->second->GetHeight();
            }
        }
        COptCCParams p;
        if (sourceTx.vout[spendingTx.vin[nIn].prevout.n].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() && 
            p.evalCode == EVAL_IDENTITY_PRIMARY && 
            p.version >= COptCCParams::VERSION_V3 &&
            p.vData.size() > 1)
        {
            oldFinalization = CObjectFinalization(p.vData[0]);
        }
    }
    return oldFinalization;
}


/*
 * Ensures that the finalization, either as validated or orphaned, is determined by
 * 10 confirmations, either of this transaction, or of an alternate transaction on the chain that we do not derive
 * from. If the former, then this should be asserted to be validated, otherwise, it should be asserted to be invalidated.
 *  
 */
bool ValidateFinalizeNotarization(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // to validate a finalization spend, we need to validate the spender's assertion of confirmation or rejection as proven


    // first, determine our notarization finalization protocol
    CTransaction sourceTx;
    uint32_t oldHeight;
    CObjectFinalization oldFinalization = GetOldFinalization(tx, nIn, &sourceTx, &oldHeight);
    if (!oldFinalization.IsValid())
    {
        return eval->Error("Invalid finalization output");
    }

    // get currency to determine system and notarization method
    CCurrencyDefinition curDef = ConnectedChains.GetCachedCurrency(oldFinalization.currencyID);
    if (!curDef.IsValid())
    {
        return eval->Error("Invalid currency ID in finalization output");
    }
    uint160 SystemID = curDef.GetID();

    if (curDef.notarizationProtocol == curDef.NOTARIZATION_AUTO)
    {
        // auto-notarization not yet implemented
        if (!PBAAS_TESTMODE)
        {
            return eval->Error("auto-notarization");
        }
    }
    else if (curDef.notarizationProtocol == curDef.NOTARIZATION_NOTARY_CONFIRM)
    {
        // get the notarization this finalizes and its index output
        int32_t notaryOutNum;
        CTransaction notarizationTx;
        if (oldFinalization.IsConfirmed() || oldFinalization.IsRejected())
        {
            return eval->Error("already-finalized");
        }
        
        if (oldFinalization.output.IsOnSameTransaction())
        {
            notarizationTx = sourceTx;
            // output needs non-null hash below
            oldFinalization.output.hash = notarizationTx.GetHash();
        }
        else
        {
            uint256 blkHash;
            if (!oldFinalization.GetOutputTransaction(sourceTx, notarizationTx, blkHash))
            {
                return eval->Error("notarization-transaction-not-found");
            }
        }
        if (notarizationTx.vout.size() <= oldFinalization.output.n)
        {
            return eval->Error("invalid-finalization");
        }

        CPBaaSNotarization pbn(notarizationTx.vout[oldFinalization.output.n].scriptPubKey);
        if (!pbn.IsValid())
        {
            return eval->Error("invalid-notarization");
        }

        // now, we have an unconfirmed, non-rejected finalization being spent by a transaction
        // confirm that the spender contains one fonalization output either confirming or rejecting
        // the finalization. rejection may be implicit by confirming another, later notarization.

        // First. make sure the oldFinalization is not referring to an earlier notarization than the 
        // one most recently confirmed. If so. then it can be spent by anyone.
        CChainNotarizationData cnd;
        if (!GetNotarizationData(SystemID, cnd) || !cnd.IsConfirmed())
        {
            return eval->Error("invalid-notarization");
        }



        // TODO: now, validate both rejection and confirmation



        CObjectFinalization newFinalization;
        int finalizationOutNum = -1;
        bool foundFinalization = false;
        for (int i = 0; i < tx.vout.size(); i++)
        {
            auto &oneOut = tx.vout[i];
            COptCCParams p;
            // we can accept only one finalization of this notarization as an output, find it and reject more than one
            if (oneOut.scriptPubKey.IsPayToCryptoCondition(p) &&
                p.IsValid() &&
                p.evalCode == EVAL_FINALIZE_NOTARIZATION &&
                p.vData.size() &&
                (newFinalization = CObjectFinalization(p.vData[0])).IsValid() &&
                newFinalization.output == oldFinalization.output)
            {
                if (foundFinalization)
                {
                    return eval->Error("duplicate-finalization");
                }
                foundFinalization = true;
                finalizationOutNum = i;
            }
        }

        if (!foundFinalization)
        {
            return eval->Error("invalid-finalization-spend");
        }


    }
    return true;
}

bool IsFinalizeNotarizationInput(const CScript &scriptSig)
{
    // this is an output check, and is incorrect. need to change to input
    uint32_t ecode;
    return scriptSig.IsPayToCryptoCondition(&ecode) && ecode == EVAL_FINALIZE_NOTARIZATION;
}

bool CObjectFinalization::GetOutputTransaction(const CTransaction &initialTx, CTransaction &tx, uint256 &blockHash) const
{
    if (output.hash.IsNull())
    {
        tx = initialTx;
        return true;
    }
    else if (myGetTransaction(output.hash, tx, blockHash) && tx.vout.size() > output.n)
    {
        return true;
    }
    return false;
}

// Sign the output object with an ID or signing authority of the ID from the wallet.
CNotaryEvidence CObjectFinalization::SignConfirmed(const CWallet *pWallet, const CTransaction &initialTx, const CIdentityID &signatureID) const
{
    CNotaryEvidence retVal = CNotaryEvidence(ASSETCHAINS_CHAINID, output);

    AssertLockHeld(cs_main);
    uint32_t nHeight = chainActive.Height();

    CTransaction tx;
    uint256 blockHash;
    if (GetOutputTransaction(initialTx, tx, blockHash))
    {
        retVal.SignConfirmed(*pWallet, tx, signatureID, nHeight);
    }
    return retVal;
}

CNotaryEvidence CObjectFinalization::SignRejected(const CWallet *pWallet, const CTransaction &initialTx, const CIdentityID &signatureID) const
{
    CNotaryEvidence retVal = CNotaryEvidence(ASSETCHAINS_CHAINID, output);

    AssertLockHeld(cs_main);
    uint32_t nHeight = chainActive.Height();

    CTransaction tx;
    uint256 blockHash;
    if (GetOutputTransaction(initialTx, tx, blockHash))
    {
        retVal.SignRejected(*pWallet, tx, signatureID, nHeight);
    }
    return retVal;
}

// Verify that the output object of "p" is signed appropriately with the indicated signature
// and that the signature is fully authorized to sign
CIdentitySignature::ESignatureVerification CObjectFinalization::VerifyOutputSignature(const CTransaction &initialTx, const CNotaryEvidence &signature, const COptCCParams &p, uint32_t height) const
{
    std::set<uint160> completedSignatures;
    std::set<uint160> partialSignatures;

    CCurrencyDefinition curDef;
    int32_t defHeight;

    if (p.IsValid() && 
        p.version >= p.VERSION_V3 && 
        p.vData.size() &&
        GetCurrencyDefinition(currencyID, curDef, &defHeight) &&
        curDef.IsValid())
    {
        uint256 txId = output.hash.IsNull() ? initialTx.GetHash() : output.hash;
        std::vector<uint160> vdxfCodes = {CCrossChainRPCData::GetConditionID(currencyID, CNotaryEvidence::NotarySignatureKey(), txId, output.n)};
        std::vector<uint256> statements;

        // check that signature is of the hashed vData[0] data
        auto hw = CMMRNode<>::GetHashWriter();
        hw.write((const char *)&(p.vData[0][0]), p.vData[0].size());
        uint256 msgHash = hw.GetHash();

        for (auto &authorizedNotary : curDef.notaries)
        {
            if (signature.signatures.count(authorizedNotary))
            {
                // we might have a partial or complete signature by one notary here
                const CIdentitySignature &oneIDSig = signature.signatures.find(authorizedNotary)->second;

                uint256 sigHash = oneIDSig.IdentitySignatureHash(vdxfCodes, statements, currencyID, height, authorizedNotary, "", msgHash);

                // get identity used to sign
                CIdentity signer = CIdentity::LookupIdentity(authorizedNotary, height);
                if (signer.IsValid())
                {
                    std::set<uint160> idAddresses;
                    std::set<uint160> verifiedSignatures;

                    for (const CTxDestination &oneAddress : signer.primaryAddresses)
                    {
                        if (oneAddress.which() != COptCCParams::ADDRTYPE_PK || oneAddress.which() != COptCCParams::ADDRTYPE_PKH)
                        {
                            // currently, can only check secp256k1 signatures
                            //return state.Error("Unsupported signature type");
                            return CIdentitySignature::SIGNATURE_INVALID;
                        }
                        idAddresses.insert(GetDestinationID(oneAddress));
                    }

                    for (auto &oneSig : signature.signatures.find(authorizedNotary)->second.signatures)
                    {
                        CPubKey pubKey;
                        pubKey.RecoverCompact(sigHash, oneSig);
                        if (!idAddresses.count(pubKey.GetID()))
                        {
                            // invalid signature or ID
                            return CIdentitySignature::SIGNATURE_INVALID;
                        }
                        verifiedSignatures.insert(pubKey.GetID());
                    }
                    if (verifiedSignatures.size() >= signer.minSigs)
                    {
                        completedSignatures.insert(authorizedNotary);
                    }
                    else
                    {
                        partialSignatures.insert(authorizedNotary);
                    }
                }
                else
                {
                    // invalid signing identity in signature
                    return CIdentitySignature::SIGNATURE_INVALID;
                }
            }
        }
        // all IDs in the signature must have been found and either partial or complete signatures
        if (partialSignatures.size() + completedSignatures.size() < signature.signatures.size())
        {
            return CIdentitySignature::SIGNATURE_INVALID;
        }

        if (completedSignatures.size() >= curDef.minNotariesConfirm)
        {
            return CIdentitySignature::SIGNATURE_COMPLETE;
        }
        else if (completedSignatures.size() || partialSignatures.size())
        {
            return CIdentitySignature::SIGNATURE_PARTIAL;
        }
    }
    // missing or invalid
    return CIdentitySignature::SIGNATURE_INVALID;
}

// Verify that the output object is signed with an authorized signing authority
CIdentitySignature::ESignatureVerification CObjectFinalization::VerifyOutputSignature(const CTransaction &initialTx, const CNotaryEvidence &signature, uint32_t height) const
{
    // now, get the output to check and check to ensure the signature is good
    CTransaction tx;
    uint256 blkHash;
    COptCCParams p;
    if (GetOutputTransaction(initialTx, tx, blkHash) &&
        tx.vout.size() > output.n &&
        tx.vout[output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
        p.IsValid() &&
        p.vData.size())
    {
        return VerifyOutputSignature(initialTx, signature, p, height);
    }
    else
    {
        return CIdentitySignature::SIGNATURE_INVALID;
    }
}

// this ensures that the signature is, in fact, both authorized to sign, and also a
// valid signature of the specified output object. if so, this is accepted and
// results in a valid index entry as a confirmation of the notary signature
// all signatures must be from a valid notary, or this returns false and should be
// considered invalid.
// returns the number of valid, unique notary signatures, enabling a single output
// to be sufficient to authorize.
bool ValidateNotarizationEvidence(const CTransaction &tx, int32_t outNum, CValidationState &state, uint32_t height, int &confirmedCount, bool &provenFalse)
{
    // we MUST know that the cs_main lock is held. since it can be held on the validation thread while smart transactions
    // execute, we cannot take it or assert here

    CNotaryEvidence notarySig;
    COptCCParams p;
    CCurrencyDefinition curDef;

    confirmedCount = 0;         // if a unit of evidence, whether signature or otherwise, is validated as confirming
    provenFalse = false;        // if the notarization is proven false

    if (tx.vout[outNum].scriptPubKey.IsPayToCryptoCondition(p) && 
        p.IsValid() && 
        p.version >= p.VERSION_V3 && 
        p.vData.size() && 
        (notarySig = CNotaryEvidence(p.vData[0])).IsValid() &&
        (curDef = ConnectedChains.GetCachedCurrency(notarySig.systemID)).IsValid())
    {
        // now, get the output to check and ensure the signature is good
        CObjectFinalization of;
        CPBaaSNotarization notarization;
        uint256 notarizationTxId;
        CTransaction nTx;
        uint256 blkHash;
        if (notarySig.output.hash.IsNull() ? (nTx = tx), true : myGetTransaction(notarySig.output.hash, nTx, blkHash) &&
            nTx.vout.size() > notarySig.output.n &&
            nTx.vout[notarySig.output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_FINALIZE_NOTARIZATION) &&
            p.vData.size() &&
            (of = CObjectFinalization(p.vData[0])).IsValid() &&
            of.IsNotarizationFinalization() &&
            of.output.hash.IsNull() ? (nTx = tx), true : myGetTransaction(of.output.hash, nTx, blkHash) &&
            !(notarizationTxId = nTx.GetHash()).IsNull() &&
            nTx.vout.size() > of.output.n &&
            nTx.vout[of.output.n].scriptPubKey.IsPayToCryptoCondition(p) &&
            p.IsValid() &&
            (p.evalCode == EVAL_EARNEDNOTARIZATION || p.evalCode == EVAL_ACCEPTEDNOTARIZATION) &&
            p.vData.size() &&
            (notarization = CPBaaSNotarization(p.vData[0])).IsValid() &&
            notarization.proofRoots.count(notarySig.systemID))
        {
            // signature is relative only to the notarization, not the finalization
            // that way, the information we put into the vdxfCodes have some meaning beyond
            // the blockchain on which it was signed, and we do not have to carry the
            // finalizatoin mechanism cross-chain.
            std::vector<uint160> vdxfCodes = {CCrossChainRPCData::GetConditionID(notarySig.systemID, 
                                                                                 CNotaryEvidence::NotarySignatureKey(), 
                                                                                 notarizationTxId, 
                                                                                 of.output.n)};
            std::vector<uint256> statements;

            // check that signature is of the hashed vData[0] data
            auto hw = CMMRNode<>::GetHashWriter();
            hw.write((const char *)&(p.vData[0][0]), p.vData[0].size());
            uint256 msgHash = hw.GetHash();

            for (auto &authorizedNotary : curDef.notaries)
            {
                std::map<CIdentityID, CIdentitySignature>::iterator sigIt = notarySig.signatures.find(authorizedNotary);
                if (sigIt != notarySig.signatures.end())
                {
                    // get identity used to sign
                    CIdentity signer = CIdentity::LookupIdentity(authorizedNotary, height);
                    uint256 sigHash = sigIt->second.IdentitySignatureHash(vdxfCodes, statements, of.currencyID, height, authorizedNotary, "", msgHash);

                    if (signer.IsValid())
                    {
                        std::set<uint160> idAddresses;
                        std::set<uint160> verifiedSignatures;
                        
                        for (const CTxDestination &oneAddress : signer.primaryAddresses)
                        {
                            if (oneAddress.which() != COptCCParams::ADDRTYPE_PK || oneAddress.which() != COptCCParams::ADDRTYPE_PKH)
                            {
                                // currently, can only check secp256k1 signatures
                                return state.Error("Unsupported signature type");
                            }
                            idAddresses.insert(GetDestinationID(oneAddress));
                        }

                        for (auto &oneSig : notarySig.signatures[authorizedNotary].signatures)
                        {
                            CPubKey pubKey;
                            pubKey.RecoverCompact(sigHash, oneSig);
                            uint160 pkID = pubKey.GetID();
                            if (!idAddresses.count(pkID))
                            {
                                return state.Error("Mismatched pubkey and ID in signature");
                            }
                            if (verifiedSignatures.count(pkID))
                            {
                                return state.Error("Duplicate key use in ID signature");
                            }
                            verifiedSignatures.insert(pkID);
                        }
                        if (verifiedSignatures.size() >= signer.minSigs)
                        {
                            confirmedCount++;
                        }
                        else
                        {
                            return state.Error("Insufficient signatures on behalf of ID: " + signer.name);
                        }
                    }
                    else
                    {
                        return state.Error("Invalid notary identity or corrupt local state");
                    }
                }
                else
                {
                    return state.Error("Unauthorized notary");
                }
            }
        }
        else
        {
            return state.Error("Invalid notarization reference");
        }
    }
    else
    {
        return state.Error("Invalid or non-evidence output");
    }
    
    if (!confirmedCount)
    {
        return state.Error("No evidence present");
    }
    else
    {
        return true;
    }
}

