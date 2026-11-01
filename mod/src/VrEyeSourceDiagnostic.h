#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

// Explicit stereo diagnostic only (default native). Chooses which runtime eye's
// eye-to-head offset and FOV feeds each ordinary camera slot, so a slot-dependent
// rendering fault can be separated from a pose-dependent one:
//   swap   slot0 renders the right eye's pose, slot1 the left eye's.
//   left   both slots render the left eye's pose; right   both the right eye's.
// Collapsed modes add COLLAPSE_SEPARATION metres along head X to slot1 so the
// existing nonzero-separation stereo checks still hold. Submitted images are then
// deliberately wrong for a person wearing the headset; use with the test driver.
class VrEyeSourceDiagnostic {
public:
    enum class Mode { Native,Swap,Left,Right };
    static constexpr float COLLAPSE_SEPARATION=0.0005f;
    static constexpr auto POLL_INTERVAL=std::chrono::milliseconds(500);
    static const char* name(Mode m){switch(m){case Mode::Swap:return "swap";case Mode::Left:return "left";case Mode::Right:return "right";default:return "native";}}
    static std::optional<Mode> parse(std::string value){
        while(!value.empty() && (value.back()=='\n' || value.back()=='\r' || value.back()==' '))value.pop_back();
        for(auto m:{Mode::Native,Mode::Swap,Mode::Left,Mode::Right})if(value==name(m))return m;
        return {};
    }
    static int source(Mode m,int slot){
        switch(m){case Mode::Swap:return 1-slot;case Mode::Left:return 0;case Mode::Right:return 1;default:return slot;}
    }
    static float lateral_offset(Mode m,int slot){return (m==Mode::Left || m==Mode::Right) && slot==1?COLLAPSE_SEPARATION:0.f;}

    void set_control(std::filesystem::path path){m_path=std::move(path);m_next_poll={};}
    bool enabled()const{return !m_path.empty();}
    Mode mode()const{return m_mode;}
    // Rereads the control file at most every POLL_INTERVAL. Missing or invalid
    // content keeps the last valid mode. Returns true when the mode changed.
    bool poll(std::chrono::steady_clock::time_point now){
        if(m_path.empty() || now<m_next_poll)return false;
        m_next_poll=now+POLL_INTERVAL;
        std::ifstream in{m_path,std::ios::binary};if(!in)return false;
        std::string text(64,'\0');in.read(text.data(),text.size());text.resize(size_t(in.gcount()));
        const auto parsed=parse(text);if(!parsed || *parsed==m_mode)return false;
        m_mode=*parsed;return true;
    }
private:
    std::filesystem::path m_path;
    Mode m_mode{Mode::Native};
    std::chrono::steady_clock::time_point m_next_poll{};
};
