#include "synthetic_code.h"
#include "NativeCreationFixture.h"
int main(){try{
    using namespace native_pool_membership;
    require(matches(NativeRenderProbe::TIMESTAMP,SYNTH(RELEASE_BYTES)) && !matches(0,SYNTH(RELEASE_BYTES)),"entire native pool release body and exact shipping timestamp are guarded");
    for(size_t i=0;i<SYNTH(RELEASE_BYTES).size();++i){auto bytes=SYNTH(RELEASE_BYTES);bytes[i]^=1;require(!matches(NativeRenderProbe::TIMESTAMP,bytes),"every changed native release instruction byte refuses");}
    require(hash(1)==0x34c2cb2c && hash(0x1234)==0xbe6be66e && hash(0x123456789abcdef0)==0xf6f42398,"native unsigned64 mixing and low32 hash match fixed independent vectors");
    std::array<uint8_t,0x22e0> lookup_pool{};std::array<uintptr_t,3> lookup_node{0,0x1234,1};uintptr_t lookup_head=reinterpret_cast<uintptr_t>(lookup_node.data());
    const auto lookup_base=reinterpret_cast<uintptr_t>(lookup_pool.data());put(lookup_base,0x22d0,Header{1,1,reinterpret_cast<uintptr_t>(&lookup_head)});
    const auto original_pool=lookup_pool;const auto original_node=lookup_node;
    auto membership=lookup(lookup_base,0x1234,2);require(membership.known && membership.found && membership.record==1 && membership.nodes.size()==1,"positive registered native source maps to its exact pool record");
    membership=lookup(lookup_base,0x5678,2);require(membership.known && !membership.found && membership.record==-1 && membership.nodes.size()==1 && lookup_pool==original_pool && lookup_node==original_node,"absent key follows the native no-op release branch without native writes");
    require(!lookup(0,0x1234,2).known && !lookup(UINTPTR_MAX-8,0x1234,2).known && !lookup(lookup_base,0,2).known && !lookup(lookup_base,0x1234,0).known,"invalid pool/key/record bounds refuse before native memory reads");
    lookup_node[0]=lookup_head;require(!lookup(lookup_base,0x5678,2).known,"cyclic collision chain is bounded and refused");lookup_node=original_node;
    lookup_node[2]=2;require(!lookup(lookup_base,0x1234,2).known,"out-of-range registered record refuses");lookup_node=original_node;
    lookup_head=0x1234;require(!lookup(lookup_base,0x1234,2).known,"unreadable collision node refuses without raw dereference");lookup_head=reinterpret_cast<uintptr_t>(lookup_node.data());
    put(lookup_base,0x22d0,Header{1,0,reinterpret_cast<uintptr_t>(&lookup_head)});require(!lookup(lookup_base,0x1234,2).known,"zero capacity cannot divide or become absence proof");
    put(lookup_base,0x22d0,Header{0,0,0});require(lookup(lookup_base,0x1234,2).known && !lookup(lookup_base,0x1234,2).found,"actual empty hash follows guarded native empty-count path");
    auto guard=SYNTH_ALL(Creation::BYTES);auto ref=SYNTH(Creation::RETAIN_BYTES);require(Creation::matches(NativeRenderProbe::TIMESTAMP,guard,ref),"exact native creation/init/reference instruction guards pass");for(size_t i=0;i<4;++i){guard=SYNTH_ALL(Creation::BYTES);guard[i][0]^=1;require(!Creation::matches(NativeRenderProbe::TIMESTAMP,guard,ref),"changed native callable implementation refuses");}require(!Creation::matches(0,SYNTH_ALL(Creation::BYTES),ref),"wrong native build refuses");ref[5]^=1;require(!Creation::matches(NativeRenderProbe::TIMESTAMP,SYNTH_ALL(Creation::BYTES),ref),"changed native ref instruction refuses");
    Host host;Fixture fixture(host);auto& owner=*new Creation;require(owner.configure(host.device.Get(),fixture.contract.module,fixture.api()),"real GPU allocation observation configured before fresh native factory calls");fixture.reenter=&owner;fixture.run(owner);
    require(owner.accepted() && fixture.factory_calls==2 && fixture.retain_calls==2 && fixture.pre_calls==2 && fixture.init_calls==2 && fixture.names[0]!=fixture.names[1],"bounded exact native ABI allocates two uniquely named textures once, including safe reentry");
    require(native_pool_membership::lookup(fixture.contract.pool,fixture.contract.wrappers[1].address,2).found && owner.textures()[0].pool_membership.known && !owner.textures()[0].pool_membership.found && owner.textures()[1].pool_membership.known && !owner.textures()[1].pool_membership.found,"actual registered source is a positive lookup control while both fresh owned native wrappers are absent");
    const auto& textures=owner.textures();require(textures[0].native_reference && textures[1].native_reference && textures[0].refs_before==0 && textures[0].refs_after==1 && textures[0].resource && textures[1].resource && textures[0].identity!=textures[1].identity && GpuAllocationEvidence::disjoint(textures[0].allocation->range,textures[1].allocation->range),"fresh native references and real committed COM leases have independently observed disjoint backing");
    fixture.run(owner);require(fixture.factory_calls==2,"accepted bounded pair never recreates or steals native pool resources");
    // 24 Oct armoury, 26 Oct class changes: the pool was rebuilt (0x80, one bank more), so the pair is
    // retired and a fresh one made for the new layout follows.
    const auto layout_a=fixture.contract;auto layout_b=layout_a;layout_b.bank_count=layout_a.bank_count+1;
    const auto first=owner.owned_pair();
    require(first && owner.generation()==1 && owner.renew(layout_b) && !owner.owned_pair() && !owner.accepted() && owner.generation()==1 && owner.retired()==1 && !owner.renewal().reused && owner.renewal().generation==2,
        "renew to a new layout retires the accepted pair and keeps it; a fresh pair is due");
    require(!owner.renew(layout_b),"nothing accepted to retire until the bootstrap");
    fixture.contract=layout_b;fixture.origin.dispatch+=10;fixture.run(owner);const auto second=owner.owned_pair();
    require(owner.accepted() && fixture.factory_calls==4 && second && second->origin.dispatch==fixture.origin.dispatch && second->identity[0]!=first->identity[0] && second->identity[1]!=first->identity[1] && second->native[0]!=first->native[0] &&
        owner.generation()==2 && owner.retired()==1 && !owner.reuses() && !owner.renewal().reused,"a new layout creates a fresh, distinct pair: A's pair stays retired");
    fixture.run(owner);require(fixture.factory_calls==4,"the fresh pair is again created once");
    // The pool flips back to A's layout: A's pair is current again, re-checked, with no factory call.
    require(owner.renew(layout_a) && owner.renewal().reused && owner.renewal().generation==2 && owner.retired()==2,"renew to a layout seen before expects its retired pair and creates nothing");
    const auto calls=std::array{fixture.factory_calls,fixture.retain_calls,fixture.pre_calls,fixture.init_calls};
    fixture.contract=layout_a;fixture.origin.dispatch+=10;fixture.run(owner);const auto third=owner.owned_pair();
    require(owner.accepted() && third && third->native==first->native && third->resource==first->resource && third->identity==first->identity && third->origin.dispatch==fixture.origin.dispatch && third->contract==layout_a,
        "A's retired pair is current again, with this job's origin and contract");
    require(calls==std::array{fixture.factory_calls,fixture.retain_calls,fixture.pre_calls,fixture.init_calls} && owner.generation()==2 && owner.retired()==1 && owner.reuses()==1 && owner.renewal().reused,
        "a reuse calls no factory, takes no reference, adds no generation and leaves the pair retired no longer");
    require(owner.textures()[0].pool_membership.known && !owner.textures()[0].pool_membership.found && owner.textures()[1].pool_membership.known && !owner.textures()[1].pool_membership.found,"the reused pair is absent from the game's pool");
    fixture.run(owner);require(owner.owned_pair()->origin.dispatch==third->origin.dispatch && owner.reuses()==1 && fixture.factory_calls==calls[0],"the reused pair is taken once");
    // 26 Oct loads (0x120): the same layout at other addresses. The current pair is retired and taken again, its contract re-based.
    auto moved=layout_a;moved.banks+=0x10000;moved.records+=0x20000;moved.record_count=3;moved.targets[0].record=2;moved.targets[0].wrappers[0]+=0x1000;
    require(native_texture_contract::difference(moved,layout_a) && native_texture_contract::same_layout(moved,layout_a),"an address-only rebuild differs from the contract but has its layout");
    require(owner.renew(moved) && owner.renewal().reused,"an address-only rebuild expects the pair just retired");
    fixture.contract=moved;fixture.origin.dispatch+=10;fixture.run(owner);const auto fourth=owner.owned_pair();
    require(owner.accepted() && fourth && fourth->native==first->native && fourth->contract==moved && fourth->contract.records==moved.records && fourth->contract.banks==moved.banks && fixture.factory_calls==calls[0] && owner.generation()==2 && owner.reuses()==2,
        "the pair is current again under the moved pool's addresses");
    // B again: B's pair, not a new one.
    auto moved_b=moved;moved_b.bank_count=layout_b.bank_count;
    require(owner.renew(moved_b) && owner.renewal().reused && (fixture.contract=moved_b,fixture.origin.dispatch+=10,fixture.run(owner),owner.accepted()) && owner.owned_pair()->native==second->native && fixture.factory_calls==calls[0] && owner.generation()==2,
        "B's layout takes B's retired pair");
    // A retired pair the game's pool now holds is never made current: it is spent, and a fresh pair is created.
    require(owner.renew(moved) && owner.renewal().reused,"A's layout again");
    const auto held_bucket=native_pool_membership::hash(first->native[1])%fixture.pool_buckets.size();const auto chain=fixture.pool_buckets[held_bucket];
    std::array<uintptr_t,3> held{chain,first->native[1],0};fixture.pool_buckets[held_bucket]=reinterpret_cast<uintptr_t>(held.data());
    fixture.contract=moved;fixture.origin.dispatch+=10;fixture.run(owner);const auto fifth=owner.owned_pair();
    require(owner.accepted() && fifth && fifth->native[0]!=first->native[0] && fifth->native[1]!=first->native[1] && fifth->native[0]!=second->native[0] && fixture.factory_calls==calls[0]+2 && owner.generation()==3,
        "a retired pair registered in the game's pool is not reused: a fresh pair instead");
    fixture.pool_buckets[held_bucket]=chain;
    require(owner.renew(moved_b) && owner.renewal().reused && (fixture.contract=moved_b,fixture.origin.dispatch+=10,fixture.run(owner),owner.owned_pair()->native==second->native),"B again");
    require(owner.renew(moved) && owner.renewal().reused && (fixture.contract=moved,fixture.origin.dispatch+=10,fixture.run(owner),owner.owned_pair()->native==fifth->native) && fixture.factory_calls==calls[0]+2,
        "a spent pair stays spent: A's layout now takes the pair made in its place");
    // Nothing matches: every new layout up to the cap creates a pair.
    int32_t banks=8;
    while(owner.generation()<Creation::GENERATIONS){auto fresh=moved;fresh.bank_count=++banks;const auto made=fixture.factory_calls;
        require(owner.renew(fresh) && !owner.renewal().reused && (fixture.contract=fresh,fixture.origin.dispatch+=10,fixture.run(owner),owner.accepted()) && fixture.factory_calls==made+2,"every new layout up to the cap creates a fresh pair");}
    auto beyond=moved;beyond.bank_count=++banks;
    require(owner.generation()==Creation::GENERATIONS && !owner.renew(beyond) && owner.accepted() && owner.retired()==Creation::GENERATIONS-1 && Creation::GENERATIONS>=12,"at most GENERATIONS pairs per process: a new layout at the cap is refused and the pair kept");
    // At the cap a layout seen before is still renewed. If the pool changes again before the bootstrap, it is refused without a factory call, and retried.
    require(owner.renew(moved_b) && owner.renewal().reused && !owner.accepted(),"at the cap a layout seen before still renews");
    const auto made=fixture.factory_calls;fixture.contract=beyond;fixture.origin.dispatch+=10;fixture.run(owner);
    require(!owner.accepted() && owner.refusal()==Creation::Refusal::generations && fixture.factory_calls==made && owner.generation()==Creation::GENERATIONS,"a new layout at the bootstrap past the cap is refused before any factory call");
    fixture.contract=moved_b;fixture.origin.dispatch+=10;fixture.run(owner);
    require(owner.accepted() && owner.owned_pair()->native==second->native && fixture.factory_calls==made,"the next job of a known layout takes its pair");
    fixture.contract=layout_a;
    auto& no_source=*new Fixture(host);no_source.pool_node[2]=0;auto& no_source_owner=*new Creation;require(no_source_owner.configure(host.device.Get(),no_source.contract.module,no_source.api()),"source pool control fixture configured");no_source.run(no_source_owner);require(no_source_owner.refusal()==Creation::Refusal::pool && !no_source.factory_calls,"incorrect native source hash record refuses before any fresh factory call");
    auto& variants=*new Fixture(host);variants.contract.bank_variant=1;
    for(auto& target:variants.contract.targets){target.variants=1;target.initialization_words.insert(target.initialization_words.begin(),{4,2,1,1,38,1});target.wrappers.insert(target.wrappers.begin(),uintptr_t(0x789000+target.id*0x100));}
    auto& variant_owner=*new Creation;require(variant_owner.configure(host.device.Get(),variants.contract.module,variants.api()),"selected native resolution-variant fixture configured");variants.run(variant_owner);
    require(variant_owner.accepted() && variants.init_calls==2,"actual active bank row selects both matching pool wrappers and actual RTV dimensions, rather than assuming row zero");
    for(unsigned bad=0;bad<6;++bad){auto& f=*new Fixture(host);f.contract.bank_variant=1;for(auto& target:f.contract.targets){target.variants=1;target.initialization_words.insert(target.initialization_words.begin(),{4,2,1,1,38,1});target.wrappers.insert(target.wrappers.begin(),uintptr_t(0x789000+target.id*0x100));}
        if(bad==0)f.contract.bank_variant=-1;if(bad==1)f.contract.bank_variant=2;if(bad==2)f.contract.targets[1].variants=0;if(bad==3)f.contract.targets[1].initialization_words[1][0]++;if(bad==4)std::swap(f.contract.targets[1].wrappers[0],f.contract.targets[1].wrappers[1]);if(bad==5)f.contract.targets[0].initialization_words[0][5]=0;
        auto& refused=*new Creation;require(refused.configure(host.device.Get(),f.contract.module,f.api()),"variant refusal fixture configured");f.run(refused);require(!refused.accepted() && !f.factory_calls,"invalid/out-of-range/inconsistent resolution selection refuses before native calls");}
    std::ostringstream out;owner.write(out,57);require(out.str().find("\"creation_pair_verified\":true")!=std::string::npos && out.str().find("\"used_in_native_pass\":false")!=std::string::npos && out.str().find("\"native_gpu_histories_verified\":false")!=std::string::npos && out.str().find("\"instruction_guards\":false")!=std::string::npos,"fixture creation evidence never fabricates shipping guards, history use or retirement");
    for(unsigned fault:{1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u}){auto& f=*new Fixture(host);f.fault=fault;auto& failed=*new Creation;require(failed.configure(host.device.Get(),f.contract.module,f.api()),"same live device safely reuses its actual allocation observer");f.run(failed);require(!failed.accepted() && failed.refusal()!=Creation::Refusal::none,"native creation/reference/init/resource/placement/context/pool failures remain refused");const auto calls=f.factory_calls;const auto tested_fault=fault;f.current=true;f.fault=0;f.run(failed);require(f.factory_calls==calls,"partial native failure poisons bounded creation without retries or unsafe retirement");if(tested_fault==2 || tested_fault==11)require(f.retain_calls==0,"borrowed or expired native factory result never receives a reference grant");if(tested_fault==12)require(!failed.textures()[0].resource,"borrowed cached resource pointer never receives a COM AddRef");if(tested_fault==13)require(failed.refusal()==Creation::Refusal::pool,"a supposedly fresh wrapper registered in the pool cannot pass the owned membership gate");}
    for(unsigned bad=0;bad<7;++bad){auto& f=*new Fixture(host);auto& refused=*new Creation;require(refused.configure(host.device.Get(),f.contract.module,f.api()),"precondition fixture configured");if(bad==0)f.current=false;if(bad==1)f.origin.slot=0;if(bad==2)f.contract.complete=false;if(bad==3)f.contract.targets[1].definition[0]^=1;if(bad==4)f.contract.targets[0].initialization_words[0][0]++;if(bad==5)f.outputs[1].descriptors.views[0].subresources=2;if(bad==6)f.contract.targets[1].wrappers[0]++;
        f.run(refused);require(!refused.accepted() && f.factory_calls==0,"invalid scope or native contract cannot call the factory");}
    // Diagnostic (25 Oct level-exit freeze): no extra native reference, counts left as the factory made them.
    auto& unreferenced=*new Fixture(host);auto skip_api=unreferenced.api();skip_api.skip_reference=true;auto& skip_owner=*new Creation;
    require(skip_owner.configure(host.device.Get(),unreferenced.contract.module,std::move(skip_api)),"diagnostic fixture configured");unreferenced.run(skip_owner);
    require(skip_owner.accepted() && unreferenced.retain_calls==0 && skip_owner.textures()[0].refs_after==skip_owner.textures()[0].refs_before && skip_owner.textures()[1].refs_after==skip_owner.textures()[1].refs_before,"diagnostic creation takes no native reference");
    std::cout<<"native ordinary creation: guarded ABI, real disjoint GPU allocations, native/COM ownership and fail-closed partial lifetime PASS\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
