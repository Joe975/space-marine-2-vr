#include "OrdinaryToneSource.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
using T=OrdinaryToneSource;

void parse(){
    require(T::parse("native\n")==0u && T::parse("")==0u,"native or empty control keeps both eyes on their own inputs");
    require(T::parse("bloom")==2u && T::parse("scene")==1u && T::parse("luminance")==4u,"single inputs map to native argument order");
    require(T::parse("scene+luminance\r\n")==5u && T::parse(" bloom , scene ")==3u && T::parse("all")==7u,"combinations and whitespace");
    require(!T::parse("blom") && !T::parse("bloom,x"),"unknown names are rejected, not ignored");
}
void substitutes_only_the_right_eye(){
    T t;t.mask(2);
    require(t.apply(10,0,{1,2,3})==T::Inputs{1,2,3},"left eye is never changed");
    require(t.apply(10,1,{4,5,6})==T::Inputs{4,2,6},"right eye reads the left eye's bloom of the same dispatch");
    require(t.stats().substituted==1 && t.stats().missing==0,"counted");
    require(t.apply(11,1,{4,5,6})==T::Inputs{4,5,6} && t.stats().missing==1,"no left call this dispatch: right keeps its own inputs");
    require(t.apply(12,2,{7,8,9})==T::Inputs{7,8,9},"other slots pass through");
    t.mask(0);t.apply(13,0,{1,2,3});
    require(t.apply(13,1,{1,5,6})==T::Inputs{1,5,6} && t.stats().shared==std::array<uint64_t,3>{1,0,0},"native mode counts wrappers both eyes share without swapping");
    t.mask(7);t.apply(14,0,{1,0,3});
    require(t.apply(14,1,{4,5,6})==T::Inputs{1,5,3},"a null left input is never passed to the right eye");
}
}

int main(){
    try{parse();substitutes_only_the_right_eye();}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
    std::cout<<"ordinary_tone_source: all passed\n";return 0;
}
