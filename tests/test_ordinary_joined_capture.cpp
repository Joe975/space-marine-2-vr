#include "OrdinaryJoinedCapture.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    OrdinaryJoinedCapture gate;
    require(!gate.reserve("../escape",1,1) && !gate.reserve(std::string(41,'x'),1,1) && !gate.reserve("zero",0,1),"only bounded names and real frame identities reserve");
    require(gate.reserve("first",100,90) && !gate.reserve("first",101,91) && !gate.reserve("other",100,90),"a name and native dispatch are each reserved once");
    require(!gate.claim(100,90,90,true),"an accepted but incomplete draw cannot publish an image");
    require(!gate.complete("wrong",100,90) && !gate.complete("first",100,91) && gate.complete("first",100,90),"only the exact completed draw releases its target");
    require(!gate.claim(100,90,90,false) && !gate.claim(100,90,91,true) && !gate.claim(101,91,91,true),"refused/mismatched/later pairs never replace or consume the target");
    const auto first=gate.claim(100,90,90,true);
    require(first && first->id=="first" && first->matches(100,90,90) && !gate.claim(100,90,90,true),"matching submitted stereo images claim the completed draw once");
    for(unsigned i=1;i<8;++i)require(gate.reserve("next_"+std::to_string(i),100+i,90+i),"up to eight explicit requests fit");
    require(!gate.reserve("ninth",999,999),"the process ledger is bounded even when old targets were captured");
    OrdinaryJoinedCapture concurrent;require(concurrent.reserve("race",5,4) && concurrent.complete("race",5,4),"concurrent target prepared");
    std::atomic_uint winners{};std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&]{if(concurrent.claim(5,4,4,true))++winners;});
    for(auto& thread:threads)thread.join();require(winners==1,"multiple callbacks cannot capture the same GPU frame twice");
    std::cout<<"joined capture tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
