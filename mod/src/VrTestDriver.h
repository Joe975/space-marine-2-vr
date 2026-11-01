#pragma once
#include "VrSession.h"
#include <sstream>
#include <set>
#include <cctype>

// Explicit opt-in test input. Pure timeline core; no native writes or runtime calls.
// Milliseconds, metres, degrees. Hand poses are relative to level current head;
// scripted head poses are relative to the level head captured when a run starts.
namespace vr_test {
using Clock=std::chrono::steady_clock;
using Pose=std::array<float,6>; // x, y, z, yaw, pitch, roll; XR -Z is forward.
struct Frame {
    unsigned ms{},buttons{};
    float x{},y{},turn{},fire{},aim{},right_grip{},left_grip{},utility_x{},utility_y{},utility_press{};
    Pose left{-.25f,-.35f,-.45f,0,0,0},right{.25f,-.35f,-.45f,0,0,0},head{};
};
struct Program {std::string id;bool scripted_head{};unsigned duration{};std::vector<Frame> frames;};
inline bool identifier(const std::string& id){return !id.empty() && id.size()<=64 && std::ranges::all_of(id,[](unsigned char c){return std::isalnum(c) || c=='_' || c=='-';});}
inline bool pose_valid(const Pose& p){for(size_t i=0;i<6;++i)if(!std::isfinite(p[i]) || std::abs(p[i])>(i<3?2.f:180.f))return false;return true;}
inline std::optional<Program> parse(const std::string& text,bool allow_head){
    if(text.size()>262144)return {};
    std::istringstream in{text};std::string magic,mode;unsigned version{},count{};Program p;
    if(!(in>>magic>>version>>p.id>>mode>>p.duration>>count) || magic!="SM2VR_TEST" || version!=1 || !identifier(p.id) ||
        (mode!="live_head" && mode!="scripted_head") || !p.duration || p.duration>120000 || !count || count>2048)return {};
    p.scripted_head=mode=="scripted_head";if(p.scripted_head && !allow_head)return {};
    for(unsigned n=0;n<count;++n){Frame f;
        if(!(in>>f.ms>>f.buttons>>f.x>>f.y>>f.turn>>f.fire>>f.aim>>f.right_grip>>f.left_grip>>f.utility_x>>f.utility_y>>f.utility_press))return {};
        for(auto* pose:{&f.left,&f.right,&f.head})for(auto& value:*pose)if(!(in>>value))return {};
        if(f.buttons>255 || f.ms>p.duration || (!n && f.ms) || (n && f.ms<=p.frames.back().ms))return {};
        for(auto value:{f.x,f.y,f.turn,f.utility_x,f.utility_y})if(!std::isfinite(value) || std::abs(value)>1)return {};
        for(auto value:{f.fire,f.aim,f.right_grip,f.left_grip,f.utility_press})if(!std::isfinite(value) || value<0 || value>1)return {};
        if(!pose_valid(f.left) || !pose_valid(f.right) || !pose_valid(f.head) || (!p.scripted_head && f.head!=Pose{}))return {};
        p.frames.push_back(f);
    }
    std::string extra;if(in>>extra)return {};return p;
}
inline vr_camera::Matrix matrix(const Pose& p){
    constexpr float radians=.0174532925199433f;
    auto yaw=vr_camera::IDENTITY,pitch=yaw,roll=yaw;
    auto c=std::cos(p[3]*radians),s=std::sin(p[3]*radians);yaw[0]=c;yaw[2]=-s;yaw[8]=s;yaw[10]=c;
    c=std::cos(p[4]*radians);s=std::sin(p[4]*radians);pitch[5]=c;pitch[6]=s;pitch[9]=-s;pitch[10]=c;
    c=std::cos(p[5]*radians);s=std::sin(p[5]*radians);roll[0]=c;roll[1]=s;roll[4]=-s;roll[5]=c;
    auto out=vr_camera::multiply(vr_camera::multiply(roll,pitch),yaw);for(size_t i=0;i<3;++i)out[12+i]=p[i];return out;
}
inline Pose interpolate(const Pose& a,const Pose& b,float t){Pose out{};for(size_t i=0;i<6;++i)out[i]=a[i]+(i<3?b[i]-a[i]:std::remainder(b[i]-a[i],360.f))*t;return out;}
inline VrControllerSample actions(const Frame& f){
    VrControllerSample s;s.move_x=f.x;s.move_y=f.y;s.turn_x=f.turn;s.fire=f.fire;s.aim=f.aim;s.melee=f.right_grip;s.parry=f.left_grip;
    s.utility_x=f.utility_x;s.utility_y=f.utility_y;s.utility_press=f.utility_press;
    s.confirm=f.buttons&1;s.interact=f.buttons&2;s.sprint=f.buttons&4;s.menu=f.buttons&8;s.recenter=f.buttons&16;s.finisher=f.buttons&32;
    s.fire_touch=f.buttons&64;s.fire_touch_valid=f.buttons&128;return s;
}
class Timeline {
public:
    bool start(Program p,Clock::time_point now,const vr_camera::Matrix& head){
        if(!vr_camera::level_anchor(head,m_origin))return false;
        m_program=std::move(p);m_virtual_head.reset();m_start=m_previous=now;m_state="preroll";m_elapsed=0;return true;
    }
    void cancel(const char* reason){if(active())m_state=reason;}
    bool active()const{return m_state=="preroll" || m_state=="running";}
    bool scripted_head()const{return m_program.scripted_head;}
    const std::string& state()const{return m_state;}
    const std::string& id()const{return m_program.id;}
    unsigned elapsed()const{return m_elapsed;}
    struct Output {VrBackend::Tracking tracking;VrControllerSample input;};
    Output sample(const VrBackend::Tracking& live,Clock::time_point now,bool allowed){
        if(!allowed)cancel("focus_lost");
        if(active() && (now<m_previous || now-m_previous>std::chrono::milliseconds(250)))cancel("clock_gap");
        m_previous=now;Frame f;
        if(active()){
            f.left=m_program.frames.front().left;f.right=m_program.frames.front().right;f.head=m_program.frames.front().head;
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(now-m_start).count();
            if(elapsed>=500){m_elapsed=unsigned(elapsed-500);
                if(m_elapsed>=m_program.duration){m_state="completed";f=Frame{};}
                else {m_state="running";const auto after=std::ranges::upper_bound(m_program.frames,m_elapsed,{},&Frame::ms);
                    f=*(after-1);if(after!=m_program.frames.end()){
                        const float blend=float(m_elapsed-f.ms)/float(after->ms-f.ms);
                        f.left=interpolate(f.left,after->left,blend);f.right=interpolate(f.right,after->right,blend);f.head=interpolate(f.head,after->head,blend);
                    }
                }
            }
        }
        Output out{live,{}};
        // Keep the last virtual head after completion/stop: no surprise snap back.
        if(active() && m_program.scripted_head)m_virtual_head=vr_camera::multiply(matrix(f.head),m_origin);
        if(m_program.scripted_head && m_virtual_head)out.tracking.head=*m_virtual_head;
        vr_camera::Matrix basis{};vr_camera::level_anchor(out.tracking.head,basis);
        out.tracking.controllers={vr_camera::multiply(matrix(f.left),basis),vr_camera::multiply(matrix(f.right),basis)};
        out.tracking.fingers={vr_finger::Curl{},vr_finger::Curl{}};
        if(allowed && m_state=="running")out.input=actions(f);
        return out;
    }
private:
    Program m_program;Clock::time_point m_start{},m_previous{};std::string m_state{"idle"};unsigned m_elapsed{};
    vr_camera::Matrix m_origin{vr_camera::IDENTITY};std::optional<vr_camera::Matrix> m_virtual_head;
};
} // namespace vr_test
