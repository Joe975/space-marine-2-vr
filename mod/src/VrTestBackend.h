#pragma once
#include "VrTestDriver.h"
#include <functional>

// Decorates real OpenVR: real optics/submission and live HMD by default. Controller
// ownership is exclusive for the entire opt-in launch; idle means virtual neutral.
class VrTestBackend final : public VrBackend {
public:
    using Clock=vr_test::Clock;
    VrTestBackend(std::unique_ptr<VrBackend> backend,std::filesystem::path directory,bool head,
        std::function<bool()> foreground,std::function<Clock::time_point()> clock=Clock::now)
        :m_real(std::move(backend)),m_dir(std::move(directory)),m_allow_head(head),m_foreground(std::move(foreground)),m_clock(std::move(clock)){}
    bool available()override{return m_real->available();}
    bool prepare_graphics(ID3D12CommandQueue* q)override{return m_real->prepare_graphics(q);}
    bool start()override{return m_real->start();}
    void tick()override{m_real->tick();}
    bool test_input_available()const override{return m_real->test_input_available();}
    std::optional<Tracking> tracking()override{
        const auto now=m_clock();auto live=m_real->tracking();
        const bool allowed=m_foreground() && m_real->test_input_available();
        if(!live && m_allow_head && !m_last_tracking && allowed && m_real->test_tracking_fallback_available())m_last_tracking=m_real->test_tracking_seed();
        const bool cached=!live && m_allow_head && m_last_tracking && allowed && m_real->test_tracking_fallback_available();
        if(live)m_last_tracking=live;
        if(!live && !cached){m_timeline.cancel("tracking_lost");m_input.reset();m_last_tracking.reset();m_using_cached=false;report(now);return {};}
        const auto& source=live?*live:*m_last_tracking;
        // The mailbox is bounded and atomically replaced by the CLI. Polling only
        // occurs at this one tracking/action boundary, never from native XInput.
        if(now>=m_next_poll){m_next_poll=now+std::chrono::milliseconds(50);poll(now,source,allowed,cached);}
        m_using_cached=cached && m_timeline.scripted_head();
        if(cached && !m_using_cached){m_timeline.cancel("tracking_lost");m_input.reset();report(now);return {};}
        const auto out=m_timeline.sample(source,now,allowed);
        // Missing input after finish/stop resets the real gesture state machines
        // as well as releasing native buttons (neutral alone could still swing).
        m_input=allowed && m_timeline.active()?std::optional{out.input}:std::nullopt;
        report(now);return out.tracking;
    }
    std::optional<VrControllerSample> controllers()override{return m_input;}
    bool rumble(const vr_rumble::Commands& c)override{return m_real->rumble(c);}
    bool submit(const EyeTextureCapture::Pair& p,ID3D12CommandQueue* q)override{return m_real->submit(p,q);}
    Transfer transfer(const EyeTextureCapture::Pair& p,ID3D12CommandQueue* q)override{return m_real->transfer(p,q);}
    // Before 28 Oct this fell back to the caller's queue: with the OpenXR own queue, test-driver runs
    // ended eye-image leases on the game queue while the runtime's copies could still be reading them.
    ID3D12CommandQueue* completion_queue(ID3D12CommandQueue* q)override{return m_real->completion_queue(q);}
    bool separate_frame_thread()const override{return m_real->separate_frame_thread();}
    void stop()override{m_timeline.cancel("stopped");m_input.reset();m_last_tracking.reset();m_using_cached=false;m_real->stop();}
    std::string status()const override{return m_using_cached?"test scripted head; cached native optics":m_real->status();}
    std::string input_status()const override{return "test driver: "+m_timeline.state()+" "+m_timeline.id();}
    std::string input_bindings()const override{return m_real->input_bindings();}
    std::string finger_status()const override{return "test driver: neutral curls";}
private:
    void poll(Clock::time_point now,const Tracking& live,bool allowed,bool cached){
        try{
            const auto path=m_dir/"request.txt";std::error_code ec;const auto size=std::filesystem::file_size(path,ec);if(ec)return;
            const auto stamp=std::filesystem::last_write_time(path,ec);if(ec || stamp==m_stamp)return;m_stamp=stamp;
            if(size>262144){m_error="request_too_large";m_timeline.cancel("rejected");return;}
            std::ifstream in{path,std::ios::binary};std::string text((std::istreambuf_iterator<char>(in)),{});
            std::istringstream header{text};std::string command,id;header>>command;
            if(command=="STOP"){std::string extra;if(!(header>>id) || !vr_test::identifier(id) || header>>extra){m_error="invalid_stop";m_timeline.cancel("rejected");return;}
                m_timeline.cancel("stopped");m_error.clear();return;}
            auto program=vr_test::parse(text,m_allow_head);
            if(!program){m_error="invalid_program";m_timeline.cancel("rejected");return;}
            if(cached && !program->scripted_head){m_error="live_head_requires_tracking";m_timeline.cancel("rejected");return;}
            if(m_seen.contains(program->id))return;
            if(m_seen.size()>=1024){m_error="session_request_limit";return;}
            m_seen.insert(program->id);
            if(!allowed){m_error="input_focus_unavailable";m_timeline.cancel("rejected");return;}
            if(m_timeline.active()){m_error="busy_stop_first";return;}
            m_error.clear();m_timeline.start(std::move(*program),now,live.head);
        }catch(...){m_error="mailbox_error";m_timeline.cancel("rejected");}
    }
    void report(Clock::time_point now){
        const auto key=m_timeline.id()+" "+m_timeline.state()+" "+m_error;
        if(key!=m_reported){vr_trace::Recorder::get().note("test_driver",key,now);m_reported=key;}
        if(now<m_next_status)return;m_next_status=now+std::chrono::milliseconds(250);
        try{vr_trace::Json j;j.open().key("id").string(m_timeline.id()).key("state").string(m_timeline.state()).key("elapsed_ms").integer(m_timeline.elapsed())
            .key("error").string(m_error).key("scripted_head_allowed").boolean(m_allow_head).key("cached_native_optics").boolean(m_using_cached).close();
            const auto temporary=m_dir/"status.tmp",target=m_dir/"status.json";
            {std::ofstream out{temporary};out<<j.text()<<'\n';if(!out)return;}
            MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING);
        }catch(...){}
    }
    std::unique_ptr<VrBackend> m_real;std::filesystem::path m_dir;bool m_allow_head{};
    std::function<bool()> m_foreground;std::function<Clock::time_point()> m_clock;vr_test::Timeline m_timeline;
    std::optional<VrControllerSample> m_input;Clock::time_point m_next_poll{},m_next_status{};
    std::optional<Tracking> m_last_tracking;bool m_using_cached{};
    std::filesystem::file_time_type m_stamp{};std::set<std::string> m_seen;std::string m_error,m_reported;
};
