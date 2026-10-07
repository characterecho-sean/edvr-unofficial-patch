#pragma once
#include "../../src/d3d11/flat_animated_identity_ledger.h"
#include <cstdio>
#include <cstring>
#include <vector>

inline int flatIdentityReceiptTests() {
    using edvr::FlatAnimatedIdentityLedger;
    int failures=0;
    auto check=[&](bool ok,const char* what){if(!ok){std::printf("FAIL: flat identity receipt %s\n",what);++failures;}};
    FlatAnimatedIdentityLedger ledger;
    int instance=0,poolA=0,poolB=0,poolC=0;
    std::vector<unsigned char> bytes(70*FlatAnimatedIdentityLedger::poolStride);
    for(unsigned slot=0;slot<70;++slot){
        const unsigned skeleton=1000+slot,allocation=2000+slot;
        std::memcpy(bytes.data()+slot*336,&skeleton,4);
        std::memcpy(bytes.data()+slot*336+28,&allocation,4);
    }
    unsigned slot=3;
    check(ledger.demandInstances(&instance,4) && ledger.publishWhole(&instance,&slot,4),"actual instance index published");
    check(ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,1) &&
        ledger.publishWhole(&poolA,bytes.data(),bytes.size()),"first requested row published");
    check(ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,slot),"new actual row demanded after publication");
    FlatAnimatedIdentityLedger::Identity identity;
    check(!ledger.lookup(&instance,0,&poolA,identity) &&
        std::strcmp(identity.refusal,"identity-pool-slot-unobserved")==0,
        "publication before new slot demand reproduces the live refusal string");
    auto receipt=ledger.poolReceipt(&poolA,slot);
    check(receipt.lastPublication && receipt.lastPublication<receipt.slotDemand &&
        receipt.requestedAtPublication==1 && receipt.requestedNow==2 && !receipt.rowObserved,
        "receipt distinguishes late new-slot demand from observed publication");
    check(ledger.publishWhole(&poolA,bytes.data(),bytes.size()) &&
        ledger.lookup(&instance,0,&poolA,identity) && identity.slot==slot &&
        identity.skeleton==1003 && identity.allocation==2003,
        "next complete publication authoritatively qualifies the exact row");

    ledger.reset();
    ledger.notePoolPublication(&poolA,unsigned(bytes.size()));
    check(ledger.demandInstances(&instance,4) && ledger.publishWhole(&instance,&slot,4) &&
        ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,slot),
        "candidate publication before any pool demand is recorded without copying bytes");
    receipt=ledger.poolReceipt(&poolA,slot);
    check(receipt.lastPublication<receipt.slotDemand && receipt.requestedAtPublication==0 &&
        !ledger.lookup(&instance,0,&poolA,identity),
        "pre-demand publication cannot fabricate authority");

    ledger.reset();
    check(ledger.demandInstances(&instance,4) && ledger.publishWhole(&instance,&slot,4),"rotation index publication");
    for(int* pool:{&poolA,&poolB,&poolC}){
        check(ledger.demandPoolSlot(pool,unsigned(bytes.size()),336,slot) &&
            ledger.publishWhole(pool,bytes.data(),bytes.size()),"rotating pool publication");
    }
    receipt=ledger.poolReceipt(&poolC,slot);
    check(receipt.resourceEvictions==1 && receipt.rowObserved,
        "third rotating resource evicts one of two resident pool entries");
    check(ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,slot) &&
        !ledger.lookup(&instance,0,&poolA,identity),
        "returning resource requires fresh publication after resident eviction");
    receipt=ledger.poolReceipt(&poolA,slot);
    check(receipt.resourceEvictions==2 && receipt.lastPublication<receipt.slotDemand,
        "receipt distinguishes resource rotation from first slot demand");

    ledger.reset();
    check(ledger.demandInstances(&instance,4) && ledger.publishWhole(&instance,&slot,4),"eviction index publication");
    for(unsigned i=0;i<64;++i)check(ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,i),"bounded row request");
    check(ledger.publishWhole(&poolA,bytes.data(),bytes.size()),"64 requested rows published");
    check(ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,64) &&
        ledger.demandPoolSlot(&poolA,unsigned(bytes.size()),336,0),"65th row and evicted row requested");
    receipt=ledger.poolReceipt(&poolA,0);
    check(receipt.slotEvictions==2 && receipt.slotRedemanded &&
        receipt.lastEvictedSlot==1 && receipt.requestedAtPublication==64 &&
        receipt.lastPublication<receipt.slotDemand,
        "receipt distinguishes 64-row eviction and redemand from a new slot");
    slot=0;ledger.publishWhole(&instance,&slot,4);
    check(!ledger.lookup(&instance,0,&poolA,identity) &&
        std::strcmp(identity.refusal,"identity-pool-slot-unobserved")==0,
        "redemanded evicted row remains unknown until publication");
    check(ledger.publishWhole(&poolA,bytes.data(),bytes.size()) &&
        ledger.lookup(&instance,0,&poolA,identity) && identity.skeleton==1000,
        "evicted row qualifies only after new authoritative publication");
    ledger.invalidate(&poolA);
    check(!ledger.lookup(&instance,0,&poolA,identity),"unobserved mutation invalidates published row");
    if(!failures)std::puts("flat identity receipts: ordering, rotation, eviction and authority PASS");
    return failures;
}
