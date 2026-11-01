#include "ValidatedQueue.h"
#include <stdexcept>
#include <iostream>

struct FakeQueue {
    unsigned references{1};
    unsigned long AddRef() { return ++references; }
    unsigned long Release() { return --references; }
};
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        FakeQueue first,second; unsigned checks{};
        {
            ValidatedQueue<FakeQueue> cache;
            auto accept=[&](FakeQueue*) { ++checks; return true; };
            auto reject=[&](FakeQueue*) { ++checks; return false; };
            require(!cache.accepts(nullptr,accept) && checks==0,"null queue must never pass");
            require(cache.accepts(&first,accept) && checks==1,"first queue must be validated");
            for(unsigned frame=0;frame<1000;++frame) require(cache.accepts(&first,reject),"validated queue must be reused");
            require(checks==1 && first.references==2,"hot path must skip validation and retain queue identity");
            require(!cache.accepts(&second,reject) && checks==2 && first.references==1,"replacement must be rechecked and old identity released");
            require(!cache.accepts(&second,reject) && checks==3 && second.references==1,"failed validation must never be cached");
            require(cache.accepts(&second,accept) && checks==4 && second.references==2,"replacement may recover");
            cache.reset();
            require(second.references==1,"runtime reset must release queue");
            require(cache.accepts(&second,accept) && checks==5,"same queue must be revalidated after runtime reset");
            require(cache.accepts(&first,accept) && checks==6 && second.references==1,"another queue requires another check");
        }
        require(first.references==1 && second.references==1,"cache destruction must release retained identity");
        std::cout<<"validated queue passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
