#include "SceneMode.h"
#include "VrMenuPanel.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
using namespace scene_mode;
Scene of(uint32_t mask,bool scripted=false,bool screen=false,bool available=true){return classify({available,mask,0,scripted,screen});}

// Every mask the logs have shown (config/known_run_states.json; tests/test_run_report.py checks the
// two lists agree). scripted is what EngineCinematicState reports with it: the cinematic bit, kept
// across a pause inside a cutscene.
struct Seen {uint32_t mask;bool scripted;Mode mode;const char* what;};
constexpr std::array<Seen,18> SEEN{{
    {0x0,false,Mode::gameplay,"plain play"},
    {0x10,true,Mode::cutscene,"cutscene"},
    {0x108,false,Mode::photo,"photo mode"},
    {0x400,false,Mode::paused,"paused, no menu"},
    {0x800,false,Mode::hub,"the walkable Battle Barge (24 Oct 19:52: NOT a menu)"},
    {0x810,true,Mode::cutscene,"cutscene in the barge"},
    {0x908,false,Mode::pause_menu,"pause menu"},
    {0x918,true,Mode::cutscene,"paused inside a cutscene stays a cutscene, as before"},
    {0xc00,false,Mode::hub,"front-end menu"},
    {0x1000,false,Mode::title,"startup screens"},
    {0x1400,false,Mode::title,"title"},
    {0x1800,false,Mode::title,"Game files modified dialog"},
    {0x1c00,false,Mode::title,"quitting to the title"},
    {0x4010,true,Mode::loading,"loading into a mission (25 Oct 18:57: loading wins over the cutscene bit)"},
    {0x4410,true,Mode::loading,"loading into a mission"},
    {0x4c10,true,Mode::loading,"loading into a mission"},
    {0x5410,true,Mode::loading,"loading into a mission"},
    {0xc010,true,Mode::loading,"loading into a mission (0x8000: a bit no rule reads)"},
}};

void seen_masks(){
    for(const auto& s:SEEN){const auto scene=of(s.mask,s.scripted);
        require(scene.mode==s.mode,std::string{"mask "}+std::to_string(s.mask)+" is "+name(s.mode)+" ("+s.what+"), got "+name(scene.mode));
        require(scene.mode!=Mode::unknown,"every seen mask has a rule");}
    require(of(0xc010,true).unknown_bits==0x8000 && of(0xc010,true).novel(),"a bit no rule reads is reported");
    require(!of(0x908).novel() && !of(0x800).novel(),"known masks are not novel");
}

// The rules each consumer had before 26 Oct, verbatim. classify() must agree with every one of them
// over every combination of the known bits, scripted, and the coverage detector; the only intended
// differences are listed (a loading bit is always a loading screen, and a menu over the world is no
// longer "gameplay" for the gaze stick).
void same_as_before(){
    constexpr std::array<uint32_t,7> bits{CINEMATIC,0x8,PHOTO,PAUSED,HUB,TITLE,LOADING};
    unsigned checked{};
    for(uint32_t combo=0;combo<(1u<<bits.size());++combo){uint32_t mask{};for(size_t i=0;i<bits.size();++i)if(combo&(1u<<i))mask|=bits[i];
        for(bool available:{false,true})for(bool scripted:{false,true})for(bool screen:{false,true}){
            const auto scene=of(mask,scripted,screen,available);const bool loading_bit=(mask&LOADING)!=0;++checked;
            const auto at=[&](const char* what){return std::string{what}+" differs at mask "+std::to_string(mask)+" scripted="+std::to_string(scripted)+" screen="+std::to_string(screen)+" available="+std::to_string(available);};
            // OwnedStereoReadiness (Sm2Probe readiness lambda): loading, then cutscene.
            require(scene.loading()==(available && loading_bit),at("readiness loading"));
            require(scene.cutscene()==(available && scripted && !loading_bit),at("readiness cutscene"));
            // VrMenuPanel::active before: available && !scripted && (mask&0x908)==0x908.
            const bool old_pause=available && !scripted && (mask&0x908)==0x908;
            require(scene.pause_menu()==(old_pause && !loading_bit),at("pause gate"));
            // The screen-menu gate: !pause && available && !scripted && detector.
            require(scene.screen_menu()==(!old_pause && available && !scripted && screen && !loading_bit),at("screen menu gate"));
            // Native HUD and guard guide: available && !scripted && !paused.
            require(scene.hud()==(available && !scripted && !(mask&PAUSED) && !loading_bit),at("HUD gate"));
            // Plain play: available && !scripted && mask==0; now also no menu over the world.
            require(scene.gameplay()==(available && !scripted && mask==0 && !screen),at("plain play"));
            // Gaze stick: plain play, and the hub exactly (31 Oct 18:39 playtest).
            require(scene.gaze_camera()==(available && !scripted && (mask==0 || mask==HUB) && !screen),at("gaze stick"));
        }}
    require(checked==128*8,"swept every combination");
    // A bit no rule reads changes nothing but gameplay and the gaze stick (it stays centred, as before: mask!=0).
    for(const auto& s:SEEN){const auto plain=of(s.mask,s.scripted),extra=of(s.mask|0x20000,s.scripted);
        require(extra.mode==plain.mode && extra.unknown_bits==(0x20000|plain.unknown_bits) && extra.novel(),"an unknown bit keeps the mode and is reported (mask "+std::to_string(s.mask)+")");
        require(!extra.gameplay() && !extra.gaze_camera(),"an unknown bit keeps the gaze stick centred");}
}

// Where the gaze camera steers. The Battle Barge sits at 0x800 from arrival until the first pause or Use
// and at 0x0 after, so a rule of 0x0 alone left the game camera at its spawn heading for the first walk to
// the battle simulator or mission select, and every prompt behind the player (31 Oct 18:44:17, 18:53:22).
void gaze_camera_scenes(){
    require(of(0x0).gaze_camera(),"plain play steers");
    require(of(0x800).gaze_camera(),"the walkable Battle Barge steers");
    require(!of(0x800).gameplay(),"the hub is still not plain play: the HUD canvas stays head-locked there");
    for(const uint32_t mask:{0xc00u,0x108u,0x400u,0x908u,0x1000u,0x1400u,0x1800u,0x1c00u,0x4800u})
        require(!of(mask).gaze_camera(),"no steering at mask "+std::to_string(mask));
    require(!of(0x800,false,true).gaze_camera(),"mission select over the hub keeps the stick for the menu");
    require(!of(0x810,true).gaze_camera(),"a cutscene in the barge");
    require(!of(0x800,false,false,false).gaze_camera(),"unreadable flags");
}

void menu_panel_delegates(){
    VrMenuPanel panel;
    require(panel.active(true,0x908,0,false) && !panel.active(true,0x800,0,false) && !panel.active(true,0x908,0,true) && !panel.active(false,0x908,0,false),"the panel's pause gate is the classifier's");
    panel.enabled=false;require(!panel.active(true,0x908,0,false),"a disabled panel is never active");
}

void watch(){
    Watch w;
    auto e=w.update(of(0x1400));require(e && e->changed && e->first_mask && e->mode==Mode::title,"first scene is logged");
    require(!w.update(of(0x1400)),"the same scene again is quiet");
    e=w.update(of(0x1800));require(e && !e->changed && e->first_mask,"a new mask in the same mode is logged once");
    require(!w.update(of(0x1800)) && !w.update(of(0x1400)),"masks seen before are quiet");
    e=w.update(of(0x800));require(e && e->changed && e->previous==Mode::title && e->mode==Mode::hub,"a mode change names both modes");
    e=w.update(of(0x800,false,true));require(e && e->changed && !e->first_mask && e->mode==Mode::screen_menu,"the coverage detector changes the mode, not the mask");
    require(w.changes()==3 && w.masks()==3,"counters");
    e=w.update(of(0x800,false,true,false));require(e && e->changed && e->mode==Mode::unavailable && !e->first_mask && w.masks()==3,"unreadable flags: a mode change, no mask recorded");
    require(std::string_view(name(Mode::screen_menu))=="screen_menu" && std::string_view(name(Mode::paused))=="paused","names");
}
}
int main(){try{seen_masks();same_as_before();gaze_camera_scenes();menu_panel_delegates();watch();std::cout<<"scene mode: every seen mask, same decisions as the old per-consumer rules, watch PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
