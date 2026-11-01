#pragma once
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

// Optional extra completion polls on the existing Present thread. Never waits,
// captures a new HUD source, or moves runtime submission onto a worker thread.
class PresentHandoff {
public:
    enum class Point { present, post_frame, post_present, dispatch_complete };
    void configure(std::filesystem::path path){if(path.is_absolute())m_control=std::move(path);}
    bool mode(const std::string& value){
        if(value=="present")m_mode=0;
        else if(value=="opportunistic")m_mode=1;
        else if(value=="queued")m_mode=2;
        else if(value=="isolated")m_mode=3;
        else return false;
        return true;
    }
    void poll(double now){
        if(m_control.empty() || now<m_next_poll)return;
        m_next_poll=now+1000;std::ifstream input{m_control};std::string value;
        if(std::getline(input,value)){if(!value.empty() && value.back()=='\r')value.pop_back();mode(value);}
    }
    template<class F> bool visit(Point point,F&& action,DWORD thread=GetCurrentThreadId()){
        if(!m_owner && point==Point::present)m_owner=thread;
        const bool permitted=point==Point::present || (point==Point::dispatch_complete?queued():extra());
        if(!m_owner || thread!=m_owner || !permitted || m_running)return false;
        struct Guard{bool& flag;Guard(bool& value):flag(value){flag=true;}~Guard(){flag=false;}} guard{m_running};
        action();return true;
    }
    bool extra()const{return m_mode==1;}
    bool queued()const{return m_mode==2;}
    bool isolated()const{return m_mode==3;}
    unsigned code()const{return m_mode;}
    bool capture_here(bool after_dispatch,bool native_hud)const{return !queued() || native_hud || after_dispatch;}
    void reset(){m_owner=0;}
private:
    std::filesystem::path m_control;double m_next_poll{};std::atomic<DWORD> m_owner{};std::atomic<unsigned> m_mode{};bool m_running{};
};
