#include "GpuTestHost.h"
#include "GpuTextureReadback.h"
#include "VrNativeHud.h"

int main(){try{
    Host host;EyeTextureCapture capture;
    const auto world=host.color(0,0,1,512,DXGI_FORMAT_R8G8B8A8_UNORM,512),hud=host.color(0,0,0,128,DXGI_FORMAT_R8G8B8A8_UNORM,128);
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;check(host.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    const auto handle=heap->GetCPUDescriptorHandleForHeapStart();host.device->CreateRenderTargetView(hud.Get(),nullptr,handle);
    const auto paint=[&](float r,float g,float b){host.begin();auto before=barrier(hud.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&before);
        const float left[]{r,g,b,1},right[]{0,1,0,1},black[]{0,0,0,1};const D3D12_RECT a{0,0,64,128},c{64,0,128,128},gap{0,60,128,68};
        host.commands->ClearRenderTargetView(handle,left,1,&a);host.commands->ClearRenderTargetView(handle,right,1,&c);host.commands->ClearRenderTargetView(handle,black,1,&gap);
        auto after=barrier(hud.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();};
    paint(1,0,0);
    VrNativeHud crop;crop.values.values={0,0,.5f,1,.5f,0,.5f,1};require(crop.valid(),"two exact native source regions accepted");
    auto invalid=crop;invalid.values.values[0]=.8f;require(!invalid.valid(),"crop extending outside actual canvas is rejected");
    const auto path=std::filesystem::temp_directory_path()/("sm2vr-native-hud-"+std::to_string(GetCurrentProcessId())+".calibration");
    require(crop.save(path) && VrNativeHud::load(path)==crop,"source crop knobs round trip");std::filesystem::remove(path);
    std::array<EyeTextureCapture::Metadata,2> metadata;
    for(int eye=0;eye<2;++eye){auto& m=metadata[eye];m=meta(100+eye,eye,100+eye);m.tracked_stereo=m.native_hud=true;m.hud_crop=crop;
        require(vr_projection::build(eye?vr_projection::Tangents{-.8f,1.2f,-1,1}:vr_projection::Tangents{-1.2f,.8f,-1,1},.1f,100,m.projection),"asymmetric projection built");
        m.runtime_eye_to_head[12]=eye?.032f:-.032f;
        require(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),m)==E_INVALIDARG,"enabled HUD requires an actual leased native canvas");
        check(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),m,D3D12_RESOURCE_STATE_PRESENT,hud.Get()));
    }
    auto right=capture.latest(1);wait(right->fence.Get(),right->fence_value);auto pair=capture.pair();require(pair.has_value(),"both tracked eyes publish matching compact native HUD");
    std::vector<GpuTextureReadback::Pixels> images;const std::array<GpuTextureReadback::Source,2> sources{{{pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
    check(GpuTextureReadback::capture(host.queue.Get(),sources,images));
    for(int eye=0;eye<2;++eye){uint64_t red{},green{},blue{},red_x{},green_x{};const auto& rgb=images[eye].rgb;
        for(size_t p=0;p<rgb.size();p+=3){const auto x=(p/3)%512;
            if(rgb[p]>250 && rgb[p+1]<2 && rgb[p+2]<2){++red;red_x+=x;}
            if(rgb[p]<2 && rgb[p+1]>250 && rgb[p+2]<2){++green;green_x+=x;}
            if(rgb[p]<2 && rgb[p+1]<2 && rgb[p+2]>250)++blue;
        }
        require(red>200 && green>200 && blue>512*512*.96,"real GPU HUD contains both native regions while black canvas preserves world pixels");
        const auto& m=metadata[eye];const auto& s=m.hud_placement.values.values;
        const auto projected=[&](float x){return (1+m.projection[8]+m.projection[0]*(x-m.runtime_eye_to_head[12])/s[2])*256;};
        require(std::abs(double(red_x)/red+.5-projected(-s[0]/4))<1 && std::abs(double(green_x)/green+.5-projected(s[0]/4))<1,"both native HUD halves converge at the same physical plane in asymmetric eyes");
    }
    paint(.25f,.25f,.25f);require(capture.reset(true),"completed HUD leases drain on resize/reset");
    auto m=metadata[0];m.render_id=m.game_token=200;check(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),m,D3D12_RESOURCE_STATE_PRESENT,hud.Get()));
    auto changed=capture.latest(0);wait(changed->fence.Get(),changed->fence_value);
    const std::array<GpuTextureReadback::Source,1> changed_source{{{changed->texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};check(GpuTextureReadback::capture(host.queue.Get(),changed_source,images));
    size_t blended{};for(size_t p=0;p<images[0].rgb.size();p+=3)if(images[0].rgb[p]==64 && images[0].rgb[p+1]==64 && images[0].rgb[p+2]==255)++blended;
    require(blended>200,"changed native graphics reach actual eye pixels with correct premultiplied alpha");
    m.render_id=m.game_token=201;m.eye=1;m.native_hud=false;check(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),m));auto last=capture.latest(1);wait(last->fence.Get(),last->fence_value);
    require(!capture.pair(),"HUD enabled/disabled transitions cannot mix eye presentations");
    auto bad=metadata[0];bad.render_id=300;bad.hud_crop=invalid;require(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),bad,D3D12_RESOURCE_STATE_PRESENT,hud.Get())==E_INVALIDARG,"invalid live source parameters never enqueue GPU work");
    bad=metadata[0];bad.screen_menu=true;require(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),bad,D3D12_RESOURCE_STATE_PRESENT,hud.Get())==E_INVALIDARG,"native menus cannot become status graphics");
    bad=metadata[0];bad.tracked_stereo=false;require(capture.capture(host.device.Get(),host.queue.Get(),world.Get(),bad,D3D12_RESOURCE_STATE_PRESENT,hud.Get())==E_INVALIDARG,"native status requires tracked stereo");
    {   // Centre interaction prompt: a second plane with the native camera's angular mapping.
        VrNativePrompt prompt;require(prompt.valid() && prompt.enabled() && !VrNativePrompt::off().enabled(),"prompt crop on by default with the native HUD, off in bare metadata");
        const auto plane=prompt.plane(16.f/9);
        require(plane && std::abs(plane->centre_x)<1e-5f && std::abs(plane->centre_y)<1e-5f && std::abs(plane->width-2*1.5f*std::tan(55*.0174532925f))<1e-4f,
            "the whole canvas (default since 25 Oct: every world marker) sits straight ahead at its true native angular size");
        auto bad_prompt=prompt;bad_prompt.values.values[VrNativePrompt::X]=.9f;require(!bad_prompt.valid(),"prompt crop outside the canvas rejected");
        bad_prompt=prompt;bad_prompt.values.values[VrNativePrompt::ENABLED]=.5f;require(!bad_prompt.valid(),"enabled is 0 or 1");
        const auto prompt_path=std::filesystem::temp_directory_path()/("sm2vr-native-prompt-"+std::to_string(GetCurrentProcessId())+".calibration");
        require(prompt.save(prompt_path) && VrNativePrompt::load(prompt_path)==prompt,"prompt knobs round trip");std::filesystem::remove(prompt_path);
        // Canvas: black, a red marker exactly at the native screen centre and a green one at u=0.6.
        const auto canvas=host.color(0,0,0,128,DXGI_FORMAT_R8G8B8A8_UNORM,128);
        host.device->CreateRenderTargetView(canvas.Get(),nullptr,handle);host.begin();
        {auto before=barrier(canvas.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&before);
         const float black[]{0,0,0,1},red[]{1,0,0,1},green[]{0,1,0,1};const D3D12_RECT centre{62,62,66,66},side{75,62,79,66};
         host.commands->ClearRenderTargetView(handle,black,0,nullptr);host.commands->ClearRenderTargetView(handle,red,1,&centre);host.commands->ClearRenderTargetView(handle,green,1,&side);
         auto after=barrier(canvas.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();}
        EyeTextureCapture prompt_capture;std::array<EyeTextureCapture::Metadata,2> pm;
        VrNativeHud corners;corners.values.values={0,0,.1f,.1f,.9f,.9f,.1f,.1f};require(corners.valid(),"black corner status regions");
        for(int eye=0;eye<2;++eye){pm[eye]=metadata[eye];pm[eye].render_id=pm[eye].game_token=400+eye;pm[eye].hud_crop=corners;pm[eye].hud_prompt=prompt;
            check(prompt_capture.capture(host.device.Get(),host.queue.Get(),world.Get(),pm[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
        auto last_prompt=prompt_capture.latest(1);wait(last_prompt->fence.Get(),last_prompt->fence_value);
        const auto prompt_pair=prompt_capture.pair();require(prompt_pair.has_value(),"both eyes publish the matching prompt presentation");
        const std::array<GpuTextureReadback::Source,2> prompt_sources{{{prompt_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{prompt_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
        check(GpuTextureReadback::capture(host.queue.Get(),prompt_sources,images));
        const float tangent=std::tan(55*.0174532925f),d=prompt.values.values[VrNativePrompt::DISTANCE];
        for(int eye=0;eye<2;++eye){uint64_t red{},green{},red_x{},green_x{};const auto& rgb=images[eye].rgb;
            for(size_t p=0;p<rgb.size();p+=3){const auto x=(p/3)%512;
                if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40){++red;red_x+=x;}
                if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40){++green;green_x+=x;}}
            const auto& m=pm[eye];
            const auto projected=[&](float u){const float x=d*tangent*(2*u-1);return (1+m.projection[8]+m.projection[0]*(x-m.runtime_eye_to_head[12])/d)*256;};
            require(red>100 && green>100,"prompt markers reach the eye images");
            require(std::abs(double(red_x)/red+.5-projected(.5f))<1.5,"the native screen centre lands straight ahead in both eyes");
            require(std::abs(double(green_x)/green+.5-projected(77.f/128))<1.5,"an off-centre native marker keeps its native direction");
        }
        // 30 Oct: the canvas locked to the game's camera (hud_camera). The game camera aims 15 degrees left
        // of the eye; the native screen centre (the crosshair) must land on that aim line, not straight ahead.
        {EyeTextureCapture locked;std::array<EyeTextureCapture::Metadata,2> lm;
         const float yaw=-15*.0174532925f;VrHudCamera game;game.world={std::cos(yaw),0,-std::sin(yaw),0, 0,1,0,0, std::sin(yaw),0,std::cos(yaw),0, 0,0,0,1};
         game.left=game.bottom=-.7f;game.right=game.top=.7f;game.units_per_metre=.9f;
         for(int eye=0;eye<2;++eye){lm[eye]=pm[eye];lm[eye].render_id=lm[eye].game_token=700+eye;lm[eye].hud_camera=game;
             check(locked.capture(host.device.Get(),host.queue.Get(),world.Get(),lm[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
         auto last_locked=locked.latest(1);wait(last_locked->fence.Get(),last_locked->fence_value);
         const auto locked_pair=locked.pair();require(locked_pair.has_value(),"both eyes publish the camera-locked canvas");
         const std::array<GpuTextureReadback::Source,2> locked_sources{{{locked_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{locked_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
         check(GpuTextureReadback::capture(host.queue.Get(),locked_sources,images));
         for(int eye=0;eye<2;++eye){uint64_t red{},red_x{};const auto& rgb=images[eye].rgb;const auto& m=lm[eye];
             for(size_t p=0;p<rgb.size();p+=3)if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40){++red;red_x+=(p/3)%512;}
             // The aim line's direction in the eye: x/z = tan(yaw); its NDC is P0*x/z + P8.
             const double expected=(1+m.projection[0]*std::tan(yaw)+m.projection[8])*256;
             require(red>20,"the camera-locked crosshair reaches the eye image");
             require(std::abs(double(red_x)/red+.5-expected)<1.5,"the crosshair sits on the game camera's aim line, not straight ahead");}
         VrHudCamera behind=game;behind.world={-1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,0,0,1};
         EyeTextureCapture away;auto am=lm;for(int eye=0;eye<2;++eye){am[eye].render_id=am[eye].game_token=720+eye;am[eye].hud_camera=behind;
             check(away.capture(host.device.Get(),host.queue.Get(),world.Get(),am[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
         auto last_away=away.latest(1);wait(last_away->fence.Get(),last_away->fence_value);
         const auto away_pair=away.pair();require(away_pair.has_value(),"both eyes publish while looking away");
         const std::array<GpuTextureReadback::Source,2> away_sources{{{away_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{away_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
         check(GpuTextureReadback::capture(host.queue.Get(),away_sources,images));
         for(int eye=0;eye<2;++eye){uint64_t red{};const auto& rgb=images[eye].rgb;for(size_t p=0;p<rgb.size();p+=3)if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40)++red;
             require(red==0,"with the game camera behind the head the canvas is not drawn");}
         // 30 Oct: third person. The canvas stays head-locked, and the crosshair patch (VrHudReticle) moves
         // to the aim point 15 degrees left (clear of the green marker): the red centre marker leaves straight ahead and lands on the
         // hit point's direction; the green marker outside the patch stays where it was.
         {EyeTextureCapture moved;auto rm=pm;const float right=-15*.0174532925f;VrHudReticle reticle;reticle.values.values[VrHudReticle::WIDTH]=.08f;reticle.values.values[VrHudReticle::CENTRE_Y]=.5f; // Square: the green marker stays outside. On this canvas's marker.
          VrHudCamera level=game;level.world=vr_camera::IDENTITY;
          for(int eye=0;eye<2;++eye){rm[eye].render_id=rm[eye].game_token=740+eye;rm[eye].hud_camera=level;rm[eye].hud_camera_lock=false;rm[eye].hud_reticle=reticle;
              rm[eye].reticle_target=std::array<float,3>{10*std::tan(right),0,10};
              check(moved.capture(host.device.Get(),host.queue.Get(),world.Get(),rm[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
          auto last_moved=moved.latest(1);wait(last_moved->fence.Get(),last_moved->fence_value);
          const auto moved_pair=moved.pair();require(moved_pair.has_value(),"both eyes publish the moved crosshair");
          require(moved_pair->left.metadata.reticle_target && moved_pair->right.metadata.reticle_target,"image metadata records the drawn patch");
          const std::array<GpuTextureReadback::Source,2> moved_sources{{{moved_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{moved_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
          check(GpuTextureReadback::capture(host.queue.Get(),moved_sources,images));
          for(int eye=0;eye<2;++eye){uint64_t red{},red_x{},green{},green_x{};const auto& rgb=images[eye].rgb;const auto& m=rm[eye];
              for(size_t p=0;p<rgb.size();p+=3){const auto x=(p/3)%512;
                  if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40){++red;red_x+=x;}
                  if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40){++green;green_x+=x;}}
              const double aimed=(1+m.projection[0]*std::tan(right)+m.projection[8]-m.projection[0]*m.world[12]/10)*256;
              const auto projected=[&](float u){const float x=d*tangent*(2*u-1);return (1+m.projection[8]+m.projection[0]*(x-m.runtime_eye_to_head[12])/d)*256;};
              require(red>4,"the moved crosshair reaches the eye image");
              require(std::abs(double(red_x)/red+.5-aimed)<1.5,"the crosshair sits on the hit point's direction");
              require(std::abs(double(red_x)/red+.5-projected(.5f))>20,"and has left straight ahead (the canvas leaves the patch out)");
              require(green>100 && std::abs(double(green_x)/green+.5-projected(77.f/128))<1.5,"a marker outside the patch stays on the head-locked canvas");}}
         // No hit point: the crosshair stays on the canvas, straight ahead.
         {EyeTextureCapture kept;auto km=pm;for(int eye=0;eye<2;++eye){km[eye].render_id=km[eye].game_token=760+eye;km[eye].hud_camera=game;km[eye].hud_camera_lock=false;km[eye].hud_reticle=VrHudReticle{};
              check(kept.capture(host.device.Get(),host.queue.Get(),world.Get(),km[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
          auto last_kept=kept.latest(1);wait(last_kept->fence.Get(),last_kept->fence_value);const auto kept_pair=kept.pair();require(kept_pair.has_value(),"both eyes publish without a hit point");
          require(!kept_pair->left.metadata.reticle_target,"no patch drawn without a hit point");
          const std::array<GpuTextureReadback::Source,2> kept_sources{{{kept_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{kept_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
          check(GpuTextureReadback::capture(host.queue.Get(),kept_sources,images));
          for(int eye=0;eye<2;++eye){uint64_t red{},red_x{};const auto& rgb=images[eye].rgb;const auto& m=km[eye];
              for(size_t p=0;p<rgb.size();p+=3)if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40){++red;red_x+=(p/3)%512;}
              const double ahead=(1+m.projection[8]+m.projection[0]*(-m.runtime_eye_to_head[12])/d)*256;
              require(red>100 && std::abs(double(red_x)/red+.5-ahead)<1.5,"without a hit point the crosshair stays straight ahead, head-locked");}}
         // 31 Oct: first person with motion controls, all defaults. The canvas is locked to the game camera
         // (level, straight ahead); the crosshair (red) is where the game draws it, v 0.4225, and the gun aims
         // 20 degrees left. It lands on the hit point, level with the eye, not on the canvas above straight
         // ahead; a marker outside the patch (green) stays on the locked canvas.
         {const auto crosshair_canvas=host.color(0,0,0,128,DXGI_FORMAT_R8G8B8A8_UNORM,128);
          host.device->CreateRenderTargetView(crosshair_canvas.Get(),nullptr,handle);host.begin();
          {auto before=barrier(crosshair_canvas.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&before);
           const float black[]{0,0,0,1},red[]{1,0,0,1},green[]{0,1,0,1},yellow[]{1,1,0,1};const D3D12_RECT mark{62,52,66,56},side{94,52,98,56},ammo{62,62,66,66}; // Yellow: the magazine count, below the crosshair.
           host.commands->ClearRenderTargetView(handle,black,0,nullptr);host.commands->ClearRenderTargetView(handle,red,1,&mark);host.commands->ClearRenderTargetView(handle,green,1,&side);host.commands->ClearRenderTargetView(handle,yellow,1,&ammo);
           auto after=barrier(crosshair_canvas.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();}
          const float left=-20*.0174532925f;VrHudCamera level=game;level.world=vr_camera::IDENTITY;
          const auto markers=[&](EyeTextureCapture& target,std::array<EyeTextureCapture::Metadata,2>& frames,uint64_t id,const std::array<float,3>& hit,bool cut=true){
              for(int eye=0;eye<2;++eye){frames[eye]=pm[eye];frames[eye].render_id=frames[eye].game_token=id+eye;frames[eye].hud_camera=level;frames[eye].hud_reticle=VrHudReticle{};frames[eye].reticle_target=hit;frames[eye].reticle_cut=cut;
                  check(target.capture(host.device.Get(),host.queue.Get(),world.Get(),frames[eye],D3D12_RESOURCE_STATE_PRESENT,crosshair_canvas.Get()));}
              auto last_frame=target.latest(1);wait(last_frame->fence.Get(),last_frame->fence_value);};
          EyeTextureCapture aimed;std::array<EyeTextureCapture::Metadata,2> am;markers(aimed,am,780,{10*std::tan(left),0,10});
          const auto aimed_pair=aimed.pair();require(aimed_pair.has_value(),"both eyes publish the first-person crosshair");
          require(aimed_pair->left.metadata.reticle_target && aimed_pair->right.metadata.reticle_target,"image metadata records the drawn patch");
          const std::array<GpuTextureReadback::Source,2> aimed_sources{{{aimed_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{aimed_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
          check(GpuTextureReadback::capture(host.queue.Get(),aimed_sources,images));
          for(int eye=0;eye<2;++eye){uint64_t red{},red_x{},red_y{},green{},green_x{},yellow{},yellow_x{},yellow_y{};const auto& rgb=images[eye].rgb;const auto& m=am[eye];
              for(size_t p=0;p<rgb.size();p+=3){const auto x=(p/3)%512,y=(p/3)/512;
                  if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40){++red;red_x+=x;red_y+=y;}
                  if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40){++green;green_x+=x;}
                  if(rgb[p]>200 && rgb[p+1]>200 && rgb[p+2]<40){++yellow;yellow_x+=x;yellow_y+=y;}}
              // The magazine keeps its place under the crosshair: 0.0775 of the canvas (1.4 tangents across it),
              // scaled by the hit point's distance over its depth, as the patch is.
              const double under=256*(1+.0775*1.4/std::cos(left));
              require(yellow>4,"the magazine count reaches the eye image");
              require(std::abs(double(yellow_x)/yellow+.5-(1+m.projection[0]*std::tan(left)+m.projection[8])*256)<1.5 && std::abs(double(yellow_y)/yellow+.5-under)<1.5,"the magazine count stays under the crosshair at the hit point");
              const double on_gun=(1+m.projection[0]*std::tan(left)+m.projection[8])*256,on_canvas=(1+m.projection[8])*256;
              require(red>4,"the first-person crosshair reaches the eye image");
              require(std::abs(double(red_x)/red+.5-on_gun)<1.5 && std::abs(double(red_y)/red+.5-256)<1.5,"the first-person crosshair sits on the gun's hit point");
              require(std::abs(double(red_x)/red+.5-on_canvas)>20,"and has left the camera-locked canvas");
              const double side=(1+m.projection[0]*game.right*(2*96.f/128-1)+m.projection[8])*256;
              require(green>20 && std::abs(double(green_x)/green+.5-side)<1.5,"a marker outside the patch stays on the camera-locked canvas");}
          // The gun points past the eye: nothing to draw, and the crosshair does not snap back to the head.
          EyeTextureCapture past;std::array<EyeTextureCapture::Metadata,2> bm;markers(past,bm,800,{0,0,-10});
          const auto past_pair=past.pair();require(past_pair.has_value(),"both eyes publish with the hit point behind them");
          require(!past_pair->left.metadata.reticle_target && !past_pair->right.metadata.reticle_target,"image metadata says no patch was drawn");
          const std::array<GpuTextureReadback::Source,2> past_sources{{{past_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{past_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
          check(GpuTextureReadback::capture(host.queue.Get(),past_sources,images));
          for(int eye=0;eye<2;++eye){uint64_t red{},green{};const auto& rgb=images[eye].rgb;
              for(size_t p=0;p<rgb.size();p+=3){if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40)++red;if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40)++green;}
              require(red==0,"a hit point behind the eye: the crosshair leaves the canvas and is not drawn");
              require(green>20,"the rest of the canvas stays");}
          // Other UI beside the patch (reticle_cut false): the canvas stays whole, and the patch still goes to the aim point.
          EyeTextureCapture aside;std::array<EyeTextureCapture::Metadata,2> sm;markers(aside,sm,820,{10*std::tan(left),0,10},false);
          const auto aside_pair=aside.pair();require(aside_pair.has_value(),"both eyes publish with the canvas whole");
          const std::array<GpuTextureReadback::Source,2> aside_sources{{{aside_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{aside_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
          check(GpuTextureReadback::capture(host.queue.Get(),aside_sources,images));
          for(int eye=0;eye<2;++eye){uint64_t on_canvas{},on_gun{},ammo_on_canvas{};const auto& rgb=images[eye].rgb;const auto& m=sm[eye];
              const double gun_x=(1+m.projection[0]*std::tan(left)+m.projection[8])*256,canvas_x=(1+m.projection[8])*256;
              for(size_t p=0;p<rgb.size();p+=3){const double x=double((p/3)%512);const bool red=rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40,yellow=rgb[p]>200 && rgb[p+1]>200 && rgb[p+2]<40;
                  if(red && std::abs(x-canvas_x)<30)++on_canvas;if(red && std::abs(x-gun_x)<30)++on_gun;if(yellow && std::abs(x-canvas_x)<30)++ammo_on_canvas;}
              require(on_canvas>20 && ammo_on_canvas>20,"canvas whole: nothing cut from the canvas");
              require(on_gun>4,"canvas whole: the crosshair is still at the aim point");}
          // The patch sits behind the other panels: aimed at the green marker's own direction, green stays on top.
          EyeTextureCapture behind_panels;std::array<EyeTextureCapture::Metadata,2> om;markers(behind_panels,om,840,{3.5f,1.094f,10});
          const auto behind_pair=behind_panels.pair();require(behind_pair.has_value(),"both eyes publish with the patch under a marker");
          const std::array<GpuTextureReadback::Source,2> behind_sources{{{behind_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{behind_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
          check(GpuTextureReadback::capture(host.queue.Get(),behind_sources,images));
          for(int eye=0;eye<2;++eye){uint64_t green{};const auto& rgb=images[eye].rgb;
              for(size_t p=0;p<rgb.size();p+=3)if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40)++green;
              require(green>20,"another panel's pixels win over the patch");}}}
        // 27 Oct evening: health/inventory and the corners showed twice, in front and again head-locked
        // on this plane at the lens edge. A canvas area the strip or the corners panel draws is left out.
        VrNativeHud strip;strip.values.values={.55f,.45f,.1f,.1f,.9f,.9f,.1f,.1f}; // Health region over the green marker.
        const auto strip_only=VrNativeHudExcluded::of(strip,VrNativeHudCorners::off());
        require(VrNativeHudExcluded::inside(strip_only,77.f/128,.5f) && !VrNativeHudExcluded::inside(strip_only,.5f,.5f) &&
            strip_only[2]==std::array<float,4>{} && strip_only[3]==std::array<float,4>{},"the strip's regions are excluded; no corners panel, no corner rectangles");
        require(VrNativeHudExcluded::inside(VrNativeHudExcluded::of(strip,VrNativeHudCorners{}),.86f,.77f) && !VrNativeHudExcluded::inside(strip_only,.86f,.77f),
            "the corners are excluded only while their panel shows them");
        VrNativeHudCorners over_marker;over_marker.values.values[VrNativeHudCorners::FIRST_X]=.55f;over_marker.values.values[VrNativeHudCorners::FIRST_Y]=.45f;
        over_marker.values.values[VrNativeHudCorners::FIRST_WIDTH]=over_marker.values.values[VrNativeHudCorners::FIRST_HEIGHT]=.1f;
        require(over_marker.valid(),"fixture: a corners region over the green marker");
        int run=0;
        for(const auto& [shown_crop,shown_corners,why]:{std::tuple{strip,VrNativeHudCorners::off(),"a marker the strip shows is not shown again on the full canvas"},
                                                           std::tuple{corners,over_marker,"a marker the corners panel shows is not shown again on the full canvas"}}) {
            EyeTextureCapture once;std::array<EyeTextureCapture::Metadata,2> om;
            for(int eye=0;eye<2;++eye){om[eye]=pm[eye];om[eye].render_id=om[eye].game_token=800+10*run+eye;om[eye].hud_crop=shown_crop;om[eye].hud_corners=shown_corners;
                check(once.capture(host.device.Get(),host.queue.Get(),world.Get(),om[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
            ++run;
            auto last_once=once.latest(1);wait(last_once->fence.Get(),last_once->fence_value);
            const auto once_pair=once.pair();require(once_pair.has_value(),"both eyes publish the exclusion presentation");
            const std::array<GpuTextureReadback::Source,2> once_sources{{{once_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{once_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
            check(GpuTextureReadback::capture(host.queue.Get(),once_sources,images));
            for(int eye=0;eye<2;++eye){uint64_t red{},green{},green_on_plane{};const auto& rgb=images[eye].rgb;const auto& m=om[eye];
                const double marker_x=(1+m.projection[8]+m.projection[0]*(d*tangent*(2*77.f/128-1)-m.runtime_eye_to_head[12])/d)*256;
                for(size_t p=0;p<rgb.size();p+=3){const double x=double((p/3)%512)+.5,y=double((p/3)/512)+.5;
                    if(rgb[p]>200 && rgb[p+1]<40 && rgb[p+2]<40)++red;
                    if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40){++green;if(std::abs(x-marker_x)<8 && std::abs(y-256)<8)++green_on_plane;}}
                require(red>100,"a marker outside the excluded areas still shows on the full canvas");
                require(green>0 && green_on_plane==0,why);
            }
        }
        auto broken=pm[0];broken.render_id=broken.game_token=500;broken.hud_prompt=bad_prompt;
        require(prompt_capture.capture(host.device.Get(),host.queue.Get(),world.Get(),broken,D3D12_RESOURCE_STATE_PRESENT,canvas.Get())==E_INVALIDARG,"invalid prompt settings never enqueue GPU work");
    }
    {   // 25 Oct: the right-hand corners of the canvas (mission card with "Reject Mission", objective,
        // squad) on a low panel in front, where they can be read; on the full canvas they sit at the lens edge.
        VrNativeHudCorners corners;require(corners.valid() && corners.enabled() && !VrNativeHudCorners::off().enabled(),"corners panel on by default with the native HUD, off in bare metadata");
        auto bad_corners=corners;bad_corners.values.values[VrNativeHudCorners::SECOND_X]=.9f;require(!bad_corners.valid(),"a region outside the canvas is rejected");
        const auto corners_path=std::filesystem::temp_directory_path()/("sm2vr-native-hud-corners-"+std::to_string(GetCurrentProcessId())+".calibration");
        require(corners.save(corners_path) && VrNativeHudCorners::load(corners_path)==corners,"corners knobs round trip");std::filesystem::remove(corners_path);
        // Canvas: black with a green "mission card" at canvas uv (0.86, 0.77), inside the bottom-right region.
        const auto canvas=host.color(0,0,0,128,DXGI_FORMAT_R8G8B8A8_UNORM,128);
        host.device->CreateRenderTargetView(canvas.Get(),nullptr,handle);host.begin();
        {auto before=barrier(canvas.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&before);
         const float green[]{0,1,0,1};const D3D12_RECT card{106,94,114,103};host.commands->ClearRenderTargetView(handle,green,1,&card);
         auto after=barrier(canvas.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();}
        EyeTextureCapture corners_capture;std::array<EyeTextureCapture::Metadata,2> cm;
        VrNativeHud dark;dark.values.values={0,0,.1f,.1f,0,0,.1f,.1f}; // Status regions on black: only the corners panel shows green.
        for(int eye=0;eye<2;++eye){cm[eye]=metadata[eye];cm[eye].render_id=cm[eye].game_token=600+eye;cm[eye].hud_crop=dark;cm[eye].hud_corners=corners;
            check(corners_capture.capture(host.device.Get(),host.queue.Get(),world.Get(),cm[eye],D3D12_RESOURCE_STATE_PRESENT,canvas.Get()));}
        auto last_corner=corners_capture.latest(1);wait(last_corner->fence.Get(),last_corner->fence_value);
        const auto corner_pair=corners_capture.pair();require(corner_pair.has_value(),"both eyes publish the matching corners presentation");
        const std::array<GpuTextureReadback::Source,2> corner_sources{{{corner_pair->left.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},{corner_pair->right.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE}}};
        check(GpuTextureReadback::capture(host.queue.Get(),corner_sources,images));
        const auto& v=corners.values.values;const float d=v[VrNativeHudCorners::DISTANCE],w=v[VrNativeHudCorners::WIDTH];
        // Card centre in the bottom-right region -> right half of the panel.
        const float local_x=(110.f/128-v[VrNativeHudCorners::SECOND_X])/v[VrNativeHudCorners::SECOND_WIDTH],plane_x=(.5f+.5f*local_x-.5f)*w;
        const float local_y=(98.5f/128-v[VrNativeHudCorners::SECOND_Y])/v[VrNativeHudCorners::SECOND_HEIGHT],plane_y=v[VrNativeHudCorners::CENTRE_Y]+(.5f-local_y)*w/VrNativeHudCorners::ASPECT;
        for(int eye=0;eye<2;++eye){uint64_t green{},green_x{},green_y{};const auto& rgb=images[eye].rgb;
            for(size_t p=0;p<rgb.size();p+=3)if(rgb[p]<40 && rgb[p+1]>200 && rgb[p+2]<40){++green;green_x+=(p/3)%512;green_y+=(p/3)/512;}
            const auto& m=cm[eye];
            const double x=(1+m.projection[8]+m.projection[0]*(plane_x-m.runtime_eye_to_head[12])/d)*256,y=(1-m.projection[5]*plane_y/d)*256;
            require(green>20,"the mission card reaches the eye images on the corners panel");
            require(std::abs(double(green_x)/green+.5-x)<2 && std::abs(double(green_y)/green+.5-y)<2,"the card sits in the panel's right half, low in front, at the same plane in both eyes");
            require(y>300,"the corners panel is below the line of sight");
        }
        auto broken=cm[0];broken.render_id=broken.game_token=700;broken.hud_corners=bad_corners;
        require(corners_capture.capture(host.device.Get(),host.queue.Get(),world.Get(),broken,D3D12_RESOURCE_STATE_PRESENT,canvas.Get())==E_INVALIDARG,"invalid corners settings never enqueue GPU work");
    }
    {
        VrNativeHudRefusals refusals;
        require(refusals.record(false),"one transient capture refusal keeps the HUD (22 Oct: it used to disable it for the run)");
        require(refusals.record(true) && refusals.consecutive==0 && refusals.total==1,"a composed frame clears the refusal run");
        for(uint32_t i=1;i<VrNativeHudRefusals::LIMIT;++i)require(refusals.record(false),"refusals short of the limit keep the HUD");
        require(refusals.record(true),"a composed frame just short of the limit still keeps it");
        bool kept=true;for(uint32_t i=0;i<VrNativeHudRefusals::LIMIT;++i)kept=refusals.record(false);
        require(!kept && refusals.consecutive==VrNativeHudRefusals::LIMIT,"a run of refusals with no composed frame disables the HUD");
    }
    {   // 23 Oct: the back buffer holds the flat scene on mono frames; never compose it as HUD.
        require(VrNativeHudSource::stereo(101,100,101,true) && VrNativeHudSource::stereo(102,100,103,true),"steady stereo composes the HUD");
        require(!VrNativeHudSource::stereo(104,100,104,true),"the first pair after a mono gap (previous=dispatch-4) skips the HUD");
        require(!VrNativeHudSource::stereo(101,100,102,false),"a presented mono frame skips the HUD");
        require(!VrNativeHudSource::stereo(101,100,std::nullopt,true) && !VrNativeHudSource::stereo(101,0,101,true),"unknown frames skip the HUD");
        require(!VrNativeHudSource::stereo(101,100,105,true) && !VrNativeHudSource::stereo(101,100,100,true),"a presented frame far from the pair skips the HUD");
    }
    std::cout<<"native HUD crops, centre prompt, actual stereo pixels, black alpha, live changes, reset and refusal run PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
