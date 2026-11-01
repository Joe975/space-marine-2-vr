#include "VrCompactHud.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    using namespace std::chrono_literals;const auto now=VrCompactHud::Clock::time_point{100s};VrCompactHud hud;
    VrCompactHud::Data d{42,3,now,.7f,.5f,24,180,3};
    require(!hud.publish(d) && !hud.data(now) && !hud.allows(42,3),"unowned status cannot reach headset");hud.allow(42,3);
    require(hud.allows(42,3) && !hud.allows(41,3) && !hud.allows(42,4),"native graphic source requires exact actor and epoch permission");
    require(hud.publish(d) && hud.data(now).has_value(),"exact owner and epoch status accepted");
    require(!hud.data(now-1ms) && !hud.data(now+1001ms),"future or stale status disappears");
    hud.allow(99,3);require(!hud.data(now),"actor change hides previous player's HUD");hud.allow(42,4);require(!hud.publish(d),"epoch change refuses old stats");hud.allow(42,3);
    auto bad=d;bad.health=NAN;require(!hud.publish(bad),"invalid health does not render fabricated bars");
    require(hud.publish(d),"status recovers");auto pixels=VrCompactHud::render(d);auto changed=d;changed.health=.2f;changed.grenades=2;
    require(pixels!=VrCompactHud::render(changed),"health and grenade changes reach actual rendered texture");
    auto settings=hud.settings();settings.values.values[0]=.8f;require(hud.set_settings(settings) && hud.settings().values.values[0]==.8f,"bounded HUD placement consumed by overlay settings");
    settings.values.values[0]=10;require(!hud.set_settings(settings),"HUD size is bounded");
    hud.allow(0,0);require(!hud.data(now) && !hud.allows(42,3),"focus/runtime release hides HUD immediately");
    if(argc==2){std::ofstream out{argv[1],std::ios::binary};out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());require(bool(out),"preview pixels saved");}
    std::cout<<"compact HUD ownership, freshness, real-value rendering and placement passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
