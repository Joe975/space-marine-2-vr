#pragma once
#include "VrBodyFrame.h"
#include <chrono>
#include <mutex>
#include <cstdint>

// Compact HUD renderer and owned data boundary. A native status adapter must publish
// actual values: unknown/stale values are never inferred from button presses.
class VrCompactHud {
public:
    using Clock=std::chrono::steady_clock;
    struct Data {
        uintptr_t actor{};uint64_t epoch{};Clock::time_point captured{};
        std::optional<float> health,armour; // Normalised native status.
        std::optional<uint32_t> magazine,reserve,grenades;
    };
    using Values=vr_body::Parameters<3>;
    inline static constexpr std::array<Values::Parameter,3> PARAMETERS{{
        {"width_metres",.6f,.2f,1},{"height_metres",-.41f,-.7f,0},{"distance_metres",1.2f,.6f,3}}};
    struct Settings {
        Values values=Values::defaults(PARAMETERS);
        static std::optional<Settings> load(const std::filesystem::path& path){std::ifstream in{path};auto v=Values::read(in,PARAMETERS);return v?std::optional{Settings{*v}}:std::nullopt;}
        bool save(const std::filesystem::path& path) const {if(!path.is_absolute() || !values.in_range(PARAMETERS))return false;std::ofstream out{path};return values.write(out,PARAMETERS);}
    };
    inline static constexpr uint32_t WIDTH=640,HEIGHT=96;
    using Pixels=std::array<uint8_t,WIDTH*HEIGHT*4>;
    static VrCompactHud& get(){static VrCompactHud hud;return hud;}
    void allow(uintptr_t actor,uint64_t epoch) {
        std::lock_guard lock{m_mutex};if(actor!=m_actor || epoch!=m_epoch || !actor)m_data.reset();m_actor=actor;m_epoch=epoch;
    }
    bool allows(uintptr_t actor,uint64_t epoch)const{std::lock_guard lock{m_mutex};return actor && actor==m_actor && epoch && epoch==m_epoch;}
    bool publish(const Data& data) {
        std::lock_guard lock{m_mutex};
        const auto ratio=[](auto v){return !v || (std::isfinite(*v) && *v>=0 && *v<=1);};
        if(!m_actor || data.actor!=m_actor || data.epoch!=m_epoch || data.captured==Clock::time_point{} || (!data.health && !data.armour && !data.magazine && !data.reserve && !data.grenades) || !ratio(data.health) || !ratio(data.armour) ||
           (data.magazine && *data.magazine>99999) || (data.reserve && *data.reserve>99999) || (data.grenades && *data.grenades>99)) {m_data.reset();return false;}
        m_data=data;return true;
    }
    std::optional<Data> data(Clock::time_point now=Clock::now()) const {
        std::lock_guard lock{m_mutex};
        return m_actor && m_data && now>=m_data->captured && now-m_data->captured<=std::chrono::seconds(1)?m_data:std::nullopt;
    }
    bool set_settings(const Settings& s){if(!s.values.in_range(PARAMETERS))return false;std::lock_guard lock{m_mutex};m_settings=s;return true;}
    Settings settings() const {std::lock_guard lock{m_mutex};return m_settings;}
    static Pixels render(const Data& d) {
        Pixels out{};
        const auto rect=[&](int x,int y,int w,int h,std::array<uint8_t,4> colour){
            for(int yy=std::max(0,y);yy<std::min(int(HEIGHT),y+h);++yy)for(int xx=std::max(0,x);xx<std::min(int(WIDTH),x+w);++xx)
                for(int c=0;c<4;++c)out[(yy*WIDTH+xx)*4+c]=colour[c];
        };
        constexpr std::array<uint16_t,10> digits{0b111101101101111,0b010110010010111,0b111001111100111,0b111001111001111,0b101101111001001,
            0b111100111001111,0b111100111101111,0b111001001001001,0b111101111101111,0b111101111001111};
        const auto number=[&](std::optional<uint32_t> value,int x,int y){
            if(!value){rect(x,y+12,18,4,{180,180,180,255});return;}
            const auto text=std::to_string(*value);
            for(char ch:text){auto bits=digits[ch-'0'];for(int i=0;i<15;++i)if(bits&(1<<(14-i)))rect(x+(i%3)*5,y+(i/3)*5,4,4,{240,245,250,255});x+=20;}
        };
        rect(0,0,WIDTH,HEIGHT,{12,17,24,175});
        // Health cross, armour plates, ammunition cartridge and grenade icon.
        rect(16,23,24,8,{245,100,90,255});rect(24,15,8,24,{245,100,90,255});
        rect(16,60,24,16,{95,175,245,255});
        for(int i=0;i<2;++i){const auto value=i==0?d.health:d.armour;const int y=i?60:18;
            rect(52,y,220,18,{55,65,78,255});
            if(value && std::isfinite(*value))rect(52,y,int(220*std::clamp(*value,0.f,1.f)),18,i?std::array<uint8_t,4>{95,175,245,255}:std::array<uint8_t,4>{245,100,90,255});
            else rect(148,y+7,28,4,{180,180,180,255});
        }
        rect(302,27,10,37,{230,215,155,255});rect(304,21,6,6,{230,215,155,255});
        number(d.magazine,326,20);rect(326,52,100,2,{85,95,110,255});number(d.reserve,326,60);
        rect(507,38,20,24,{165,210,155,255});rect(512,30,10,8,{165,210,155,255});number(d.grenades,546,38);
        return out;
    }
private:
    mutable std::mutex m_mutex;uintptr_t m_actor{};uint64_t m_epoch{};std::optional<Data> m_data;Settings m_settings;
};
