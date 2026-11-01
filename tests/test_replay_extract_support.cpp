#include "ReplayExtractSupport.h"
#include <iostream>
#include <vector>
using namespace replay_extract;
template<class F> void refuses(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"expected refusal");}
int main(){try{
    const char* valid[]={"extract","event","C:/capture.rdc","C:/fresh", "17","--postvs","--max-bytes","4096"};const auto o=parse(8,valid);check(o.event==17 && o.postvs && o.bytes==4096,"actual option parser");
    const char* inventory[]={"extract","inventory","C:/capture.rdc","C:/fresh"};check(parse(4,inventory).command=="inventory","inventory option parser");
    const char* preview[]={"extract","event","C:/capture.rdc","C:/fresh","7","--preview-slot","3"};const auto po=parse(7,preview);check(po.preview && po.preview_slot==3,"actual optional preview parser");
    const char* badpreview[]={"extract","event","C:/capture.rdc","C:/fresh","7","--preview-slot","8"};refuses([&]{parse(7,badpreview);});
    const char* preview_inventory[]={"extract","inventory","C:/capture.rdc","C:/fresh","--preview-slot","0"};refuses([&]{parse(6,preview_inventory);});
    const char* preview_duplicate[]={"extract","event","C:/capture.rdc","C:/fresh","7","--preview-slot","0","--preview-slot","1"};refuses([&]{parse(9,preview_duplicate);});
    const char* scan[]={"extract","scan","C:/capture.rdc","C:/fresh","2","99","--weak","--max-events","16","--max-bytes","144"};const auto so=parse(11,scan);check(so.first==2 && so.last==99 && so.max_events==16 && so.weak && so.bytes==144,"actual bounded scan parser");
    const char* reverse[]={"extract","scan","C:/capture.rdc","C:/fresh","9","8"};refuses([&]{parse(6,reverse);});
    const char* noend[]={"extract","scan","C:/capture.rdc","C:/fresh","9"};refuses([&]{parse(5,noend);});
    const char* over_events[]={"extract","scan","C:/capture.rdc","C:/fresh","1","9","--max-events","257"};refuses([&]{parse(8,over_events);});
    for(const auto* bad:{"0","-1","4294967296","17x"}){const char* a[]={"extract","event","C:/capture.rdc","C:/fresh",bad};refuses([&]{parse(5,a);});}
    const char* excess[]={"extract","event","C:/capture.rdc","C:/fresh","17","--max-bytes","65537"};refuses([&]{parse(7,excess);});
    const char* relative[]={"extract","inventory","relative.rdc","C:/fresh"};refuses([&]{parse(4,relative);});
    check(bounded_span(90,10,100,4)==4 && bounded_span(100,0,100,4)==0,"buffer prefix bounds");refuses([]{bounded_span(UINT64_MAX,10,100,4);});refuses([]{bounded_span(90,11,100,4);});refuses([]{bounded_span(0,1,1,0);});
    Budget budget;budget.reserve(MAX_TOTAL);refuses([&]{budget.reserve(1);});Budget files;files.files=MAX_FILES;refuses([&]{files.reserve(1);});Budget names;refuses([&]{names.text(std::string(4097,'x'));});
    const auto f=format(99,4,4,7,true,false);const auto escaped=Json{{"name","a\n\"b\\c"},{"format",f}};const auto decoded=Json::parse(escaped.dump());check(decoded==escaped && decoded["format"]["type_enum"]==99,"raw format fields and JSON escaping round trip");
    std::vector<int> order;struct Fake{std::vector<int>& order;int n;void Shutdown(){order.push_back(n);delete this;}};struct Global{std::vector<int>& order;~Global(){order.push_back(3);}};
    {Global global{order};Owned<Fake> capture{new Fake{order,2}};Owned<Fake> controller{new Fake{order,1}};}
    check(order==std::vector<int>({1,2,3}),"controller/capture shutdown precedes replay globals");
    std::cout<<"replay extractor CPU support PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
