#include "BoundedTable.h"
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Row {uintptr_t key{};uint64_t calls{};std::chrono::steady_clock::time_point received{};};
}
int main(){try{
    using namespace std::chrono_literals;using R=bounded_table::Result;
    const auto t0=std::chrono::steady_clock::time_point{100s};std::vector<Row> rows;
    const auto put=[&](uintptr_t key,std::chrono::steady_clock::time_point at){return bounded_table::upsert(rows,Row{key,0,at},[&](const Row& r){return r.key==key;},3);};
    require(put(1,t0)==R::added && put(2,t0+10ms)==R::added && put(3,t0+20ms)==R::added,"rows fill up to capacity");
    require(put(2,t0+30ms)==R::updated && rows[1].calls==2 && rows[1].received==t0+30ms,"a known key updates its row and counts the call");
    require(put(4,t0+500ms)==R::dropped && rows.size()==3,"every row seen within STALE: the new key is dropped, live rows stay");
    require(put(4,t0+1s)==R::evicted && rows[0].key==4 && rows[0].calls==1,"the row unseen for STALE goes to the new key");
    require(put(5,t0+1010ms)==R::dropped,"the next stalest (key 3, seen 0.99 s ago) is still live");
    require(put(5,t0+1100ms)==R::evicted && rows[2].key==5,"then it goes too; the least recently seen row first");
    require(put(6,t0)==R::dropped,"a sample older than every row never evicts");
    std::cout<<"bounded table: fill, update, live rows kept, stale rows reused\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
