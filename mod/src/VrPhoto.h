#pragma once
#include "VrRumble.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Camera mode photos: the rules with no graphics or Windows in them (docs/VIEWER_MODE.md). EyePhoto
// does the GPU copy and the file; VrSession turns a right stick click into a request.
namespace vr_photo {
using Clock=std::chrono::steady_clock;

// A click with no eye image to take within this is refused: the game isn't drawing the mod's own
// stereo (a flat upscaler, the menu presses), so a save would have nothing to save.
inline constexpr auto EYE_WAIT=std::chrono::milliseconds(1000);
// The GPU copy normally ends with its frame. Past this the device is in trouble: give up.
inline constexpr auto GPU_WAIT=std::chrono::milliseconds(3000);
// Worst case for the save thread to wait for its copy to be recorded before it gives up alone.
inline constexpr auto TAP_WAIT=std::chrono::milliseconds(5000);

// busy: one is in flight, so this click takes nothing. Every other reason ends a click that started.
enum class Outcome : uint8_t {saved,busy,no_eye_image,bad_format,no_thread,no_memory,gpu,no_folder,write_failed};
inline const char* name(Outcome value) {
    switch(value) {case Outcome::saved:return "saved";case Outcome::busy:return "busy";case Outcome::no_eye_image:return "no_eye_image";
        case Outcome::bad_format:return "bad_format";case Outcome::no_thread:return "no_thread";case Outcome::no_memory:return "no_memory";
        case Outcome::gpu:return "gpu";case Outcome::no_folder:return "no_folder";case Outcome::write_failed:return "write_failed";}
    return "unknown";
}

// One click, one picture, one at a time, so clicks can't pile up work: a click during a save is
// refused rather than queued, and at most one readback buffer and one save thread exist.
class Flight {
public:
    enum class Stage : uint8_t {idle,wanted,preparing,armed,copying,finishing};
    Stage stage() const {return m_stage;}
    bool idle() const {return m_stage==Stage::idle;}
    bool request(Clock::time_point now) {
        if(m_stage!=Stage::idle) return false;
        m_stage=Stage::wanted;m_clicked=now;return true;
    }
    Clock::time_point clicked() const {return m_clicked;}
    void preparing() {if(m_stage==Stage::wanted) m_stage=Stage::preparing;}
    void armed() {if(m_stage==Stage::preparing) m_stage=Stage::armed;}
    void copying() {if(m_stage==Stage::armed) m_stage=Stage::copying;}
    void finishing() {m_stage=Stage::finishing;}
    void finish() {m_stage=Stage::idle;}
    // Still looking for an eye image past EYE_WAIT. Once the copy is on its way this no longer applies.
    bool expired(Clock::time_point now) const {
        return (m_stage==Stage::wanted || m_stage==Stage::preparing || m_stage==Stage::armed) && now>=m_clicked+EYE_WAIT;
    }
private:
    Stage m_stage{Stage::idle};
    Clock::time_point m_clicked{};
};

// Local wall clock at the click.
struct Stamp {uint16_t year{},month{},day{},hour{},minute{},second{};};
// SM2VR_2026-11-02_14-03-21.png, then _2, _3 for more in the same second. Sorts by time in Explorer.
inline std::string file_name(const Stamp& t,unsigned attempt=1) {
    char text[64]{};
    if(attempt<=1) std::snprintf(text,sizeof(text),"SM2VR_%04u-%02u-%02u_%02u-%02u-%02u.png",t.year,t.month,t.day,t.hour,t.minute,t.second);
    else std::snprintf(text,sizeof(text),"SM2VR_%04u-%02u-%02u_%02u-%02u-%02u_%u.png",t.year,t.month,t.day,t.hour,t.minute,t.second,attempt);
    return text;
}

// The eye texture has the game's render shape (3840x2160) but holds the headset's field of view,
// which is near square (Index: 0.92 wide per high, from the projection's P[5]/P[0]). The runtime maps
// one onto the other, so the headset looks right, and the raw texture is 1.9 times too wide: seen
// live on 2 Nov, a squat, wide Marine. A picture takes the view's true shape at the eye's pixel count.
struct Size {
    uint32_t width{},height{};
    bool operator==(const Size&) const=default;
};
inline float view_aspect(float projection_x,float projection_y) {return projection_x>0?projection_y/projection_x:0;}
inline Size picture_size(uint32_t width,uint32_t height,float aspect) {
    const Size eye{width,height};
    if(!width || !height || !std::isfinite(aspect) || aspect<.25f || aspect>4) return eye; // Not a shape to trust: leave it alone.
    if(std::abs(double(width)/height/aspect-1)<.01) return eye; // Already right: no resample.
    const double tall=std::sqrt(double(width)*height/aspect);
    return {static_cast<uint32_t>(std::lround(tall*aspect)),static_cast<uint32_t>(std::lround(tall))};
}

// A mapped RGBA or BGRA image as tightly packed BGR rows, top row first, the bytes unchanged: the
// eye images already hold the colours the headset shows. Alpha is dropped: it means nothing here and
// is often zero, which would make the picture transparent.
inline bool bgr_rows(const uint8_t* mapped,size_t pitch,uint32_t width,uint32_t height,bool bgra,std::vector<uint8_t>& out) {
    if(!mapped || !width || !height || pitch<size_t(width)*4) return false;
    out.resize(size_t(width)*height*3);
    for(uint32_t y=0;y<height;++y) {
        const auto* source=mapped+pitch*y;auto* target=out.data()+size_t(width)*3*y;
        for(uint32_t x=0;x<width;++x,source+=4,target+=3) {target[0]=source[bgra?0:2];target[1]=source[1];target[2]=source[bgra?2:0];}
    }
    return true;
}

// The shutter: a short tick on the hand that clicked, once the file is on disk.
inline constexpr auto PULSE=std::chrono::milliseconds(60);
inline constexpr float PULSE_AMPLITUDE=.7f;
inline vr_rumble::Commands pulse(float frequency) {
    vr_rumble::Commands out{};
    out[1]={vr_rumble::Command::Op::apply,PULSE_AMPLITUDE,frequency,PULSE};
    return out;
}
} // namespace vr_photo
