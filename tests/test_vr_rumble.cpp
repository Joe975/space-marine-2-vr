#include "VrRumble.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error{message}; }
bool close(float a,float b) { return std::abs(a-b)<1e-4f; }
using namespace std::chrono;
using vr_rumble::Command;
}
int main() {
    try {
        {   // Motor -> hand mapping.
            const vr_rumble::Settings both{},split{1,0},half{.5f,1},loud{2,1};
            auto a=vr_rumble::amplitudes({65535,0},both);
            require(close(a[0],1) && close(a[1],1),"cross 1: the heavy motor alone drives both hands");
            a=vr_rumble::amplitudes({65535,32768},split);
            require(close(a[0],1) && close(a[1],.5f),"cross 0: left hand = low motor, right hand = high motor");
            a=vr_rumble::amplitudes({0,65535},half);
            require(close(a[0],.5f) && close(a[1],.5f),"gain scales both hands");
            a=vr_rumble::amplitudes({40000,0},loud);
            require(close(a[0],1) && close(a[1],1),"gain above 1 clamps to full amplitude");
            require(both.valid() && !vr_rumble::Settings{2.5f,1}.valid() && !vr_rumble::Settings{1,-.1f}.valid() &&
                !vr_rumble::Settings{NAN,1}.valid(),"settings range");
        }
        {   // Mailbox round trip.
            vr_rumble::Mailbox box;const auto t=steady_clock::time_point{seconds(5)};
            require(box.read().calls==0,"empty mailbox");
            box.publish({1234,65535},t);const auto r=box.read();
            require(r.calls==1 && r.motors==vr_rumble::Motors{1234,65535} && r.at==t,"published speeds and time read back");
        }
        {   // Driver: when commands go to the runtime.
            vr_rumble::Driver d;vr_rumble::Mailbox box;const vr_rumble::Settings s{1,0};
            const auto t0=steady_clock::time_point{seconds(10)};
            auto c=d.update(box.read(),true,t0,s);
            require(c[0].op==Command::Op::none && c[1].op==Command::Op::none && !d.playing(),"no game call yet: nothing to play");
            box.publish({32768,0},t0);c=d.update(box.read(),true,t0,s);
            require(c[0].op==Command::Op::apply && close(c[0].amplitude,.5f) && c[1].op==Command::Op::none,"a new rumble is sent at once");
            c=d.update(box.read(),true,t0+milliseconds(20),s);
            require(c[0].op==Command::Op::none,"an unchanged rumble is not re-sent inside the refresh");
            c=d.update(box.read(),true,t0+milliseconds(50),s);
            require(c[0].op==Command::Op::apply,"a held rumble is re-sent before the 100 ms pulse runs out");
            box.publish({34000,0},t0+milliseconds(60));c=d.update(box.read(),true,t0+milliseconds(60),s);
            require(c[0].op==Command::Op::none,"a change under the step waits for the refresh");
            box.publish({50000,0},t0+milliseconds(70));c=d.update(box.read(),true,t0+milliseconds(70),s);
            require(c[0].op==Command::Op::apply && close(c[0].amplitude,50000/65535.f),"a larger change is sent at once");
            box.publish({0,0},t0+milliseconds(80));c=d.update(box.read(),true,t0+milliseconds(80),s);
            require(c[0].op==Command::Op::stop && !d.playing(),"the game's stop stops the controller");
            c=d.update(box.read(),true,t0+milliseconds(90),s);
            require(c[0].op==Command::Op::none,"a stop is sent once");
            box.publish({500,0},t0+milliseconds(100));c=d.update(box.read(),true,t0+milliseconds(100),s);
            require(c[0].op==Command::Op::none,"speeds under the floor are silence");
            const auto t1=t0+seconds(1);
            box.publish({65535,65535},t1);c=d.update(box.read(),true,t1,s);
            require(c[0].op==Command::Op::apply && c[1].op==Command::Op::apply,"both motors play on both hands");
            c=d.update(box.read(),true,t1+milliseconds(1001),s);
            require(c[0].op==Command::Op::stop && c[1].op==Command::Op::stop,"a speed not re-sent for a second is treated as stopped");
            box.publish({65535,0},t1+seconds(2));d.update(box.read(),true,t1+seconds(2),s);
            c=d.update(box.read(),false,t1+seconds(2)+milliseconds(5),s);
            require(c[0].op==Command::Op::stop,"disallowing (focus, pause of the session) stops at once");
            c=d.update(box.read(),true,t1+seconds(2)+milliseconds(10),s);
            require(c[0].op==Command::Op::apply,"allowed again: the still-fresh rumble resumes");
            require(d.applies()==7 && d.stops()==4 && close(d.peak(),1),"counters");
        }
        {   // Amplitude bins: what the game's rumble felt like over a stretch of play (the peak is all-time).
            using vr_rumble::Driver;vr_rumble::Mailbox box;Driver d;vr_rumble::Settings s;s.cross=0;const auto t0=steady_clock::now();
            box.publish({3277,0},t0);d.update(box.read(),true,t0,s);            // 0.05 left
            box.publish({9830,0},t0+milliseconds(10));d.update(box.read(),true,t0+milliseconds(10),s);   // 0.15
            box.publish({26214,0},t0+milliseconds(20));d.update(box.read(),true,t0+milliseconds(20),s);  // 0.40
            box.publish({65535,0},t0+milliseconds(30));d.update(box.read(),true,t0+milliseconds(30),s);  // 1.0
            require((d.bins()==std::array<uint64_t,4>{1,1,1,1}),"each applied pulse is counted in its amplitude band");
            require(Driver::bin(.0999f)==0 && Driver::bin(.1f)==1 && Driver::bin(.5f)==3,"band edges are lower-inclusive");
        }
        std::cout<<"rumble mapping, mailbox and driver passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
