#include "GpuTestHost.h"
#include "EyeDumpRequest.h"
#include <fstream>
#include <filesystem>

int main() {
    try {
        Host host;
        auto red = host.color(1,0,0), blue = host.color(0,0,1);
        EyeTextureCapture capture;
        ComPtr<ID3D12Fence> gate; check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
        check(host.queue->Wait(gate.Get(),1)); // Deterministically stop the GPU before capture commands.
        require(capture.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(1,0,100)) == S_OK,"left capture must enqueue");
        require(capture.capture(host.device.Get(),host.queue.Get(),blue.Get(),meta(2,1,101)) == S_OK,"right capture must enqueue");
        auto first_left = capture.latest(0), first_right = capture.latest(1);
        require(first_left && first_right && !first_left->ready() && !capture.pair(false),"pending copies must not be offered as complete images");
        require(!capture.reset(),"reset must not release in-flight resources");
        require(capture.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(2,0,101)) == S_FALSE,"duplicate render identity must not relabel an image");
        require(capture.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(3,0,102)) == S_OK,"second left slot must accept next frame");
        require(capture.capture(host.device.Get(),host.queue.Get(),blue.Get(),meta(4,1,103)) == S_OK,"second right slot must accept next frame");
        require(capture.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(5,0,104)) == E_PENDING,"busy or leased slots must not be reused");
        check(gate->Signal(1));
        const auto current_right = capture.latest(1); wait(current_right->fence.Get(),current_right->fence_value);
        capture.poll();
        {   // The ordinary bridge holds more images per eye: the submit thread keeps up to three.
            EyeTextureCapture deep;deep.slots(EyeTextureCapture::MAX_SLOTS);
            ComPtr<ID3D12Fence> hold; check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&hold)));check(host.queue->Wait(hold.Get(),1));
            for(uint32_t n=0;n<EyeTextureCapture::MAX_SLOTS;++n) require(deep.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(10+n,0,200+n))==S_OK,"each configured slot accepts a frame while the others are in flight");
            require(deep.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(20,0,210))==E_PENDING,"one more than the configured slots is still refused");
            EyeTextureCapture clamped;clamped.slots(9);require(clamped.slots()==EyeTextureCapture::MAX_SLOTS,"slot count is capped");clamped.slots(1);require(clamped.slots()==2,"never fewer than two slots");
            check(hold->Signal(1));const auto last=deep.latest(0);wait(last->fence.Get(),last->fence_value);
        }
        EyeDumpRequest request;
        require(!request.take("../escape",true) && !request.take("a/b",true) && !request.take("x y",true) && !request.take(std::string(41,'x'),true),"dump ids must not escape or exceed the bounded token");
        require(!request.take("first",false) && request.take("first\r\n",true) && request.id()=="first" && !request.take("first",true),"dump requests wait for a complete pair and consume once");
        EyeDumpRequest interleaved;
        require(interleaved.take("legacy",true) && interleaved.take("joined",true) && !interleaved.take("legacy",true),"interleaved joined snapshots cannot revive an old legacy request");
        for(unsigned n=0;n<EyeDumpRequest::LIMIT-1;++n) require(request.take(std::to_string(n),true),"bounded dump requests must be accepted");
        require(EyeDumpRequest::LIMIT==24,"a four-station, four-variant parity sweep fits in one launch");
        require(!request.take("overflow",true),"dump requests have a hard per-process limit");
        const auto dump=std::filesystem::temp_directory_path()/("sm2vr-eye-test-"+std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(dump);
        VrMenuPanel panel;panel.width_degrees=60;
        require(panel.active(true,0x908,0x908,false) && !panel.active(true,0,0,false) && !panel.active(true,0x800,0x800,false) && !panel.active(true,0xc00,0xc00,false) && !panel.active(true,0x1800,0x1800,false) &&
            !panel.active(true,0x400,0x400,false) && !panel.active(true,0x108,0x108,false) && !panel.active(true,0x908,0x908,true) && !panel.active(true,0x810,0x810,true) &&
            !panel.active(false,0x800,0x800,false),"menu gate: the pause menu only; never gameplay (0, 0x108), the Battle Barge (0x800) or cutscenes (0x810)");
        const auto settings_file=dump.string()+".calibration";
        require(panel.save(settings_file) && VrMenuPanel::load(settings_file)==panel,"menu knobs must round trip through the live file format");
        {std::ofstream invalid{settings_file};invalid<<"version=1\nenabled=1\nwidth_degrees=nan\ndistance_metres=2\n";}
        require(!VrMenuPanel::load(settings_file),"nonfinite menu settings must fail closed");std::filesystem::remove(settings_file);
        EyeTextureCapture menu_capture;
        const auto square=host.color(1,0,0,128,DXGI_FORMAT_R8G8B8A8_UNORM,128);
        auto menu_left=meta(20,0,20),menu_right=meta(21,1,21);
        vr_projection::build({-1.2f,.8f,-1,1},.1f,100,menu_left.projection);
        vr_projection::build({-.8f,1.2f,-1,1},.1f,100,menu_right.projection);
        menu_left.runtime_eye_to_head[12]=-.064f;menu_right.runtime_eye_to_head[12]=.064f;
        menu_left.screen_menu=menu_right.screen_menu=true;menu_left.menu_panel=menu_right.menu_panel=panel;
        const auto left_uv=panel.transform(menu_left.projection,menu_left.runtime_eye_to_head,1);
        auto wider=panel;wider.width_degrees=90;auto further=panel;further.distance_metres=4;
        require(left_uv && wider.transform(menu_left.projection,menu_left.runtime_eye_to_head,1)!=left_uv &&
            further.transform(menu_left.projection,menu_left.runtime_eye_to_head,1)!=left_uv,"both width and distance knobs must change actual shader constants");
        {   // World-locked panel: the image column that shows the canvas centre, at mid height.
            const auto canvas_u=[](const VrMenuPanel::Homography& h,float x,float y){const float w=h[8]*x+h[9]*y+h[10];return w>0?std::optional{(h[0]*x+h[1]*y+h[2])/w}:std::nullopt;};
            const auto centre_column=[&](const VrMenuPanel& p,const vr_camera::Matrix& head)->std::optional<float>{
                const auto h=p.transform(menu_left.projection,menu_left.runtime_eye_to_head,1,head);if(!h)return std::nullopt;
                std::optional<float> found;float best=1;
                for(int i=0;i<=1000;++i){const float x=i/1000.f;if(const auto u=canvas_u(*h,x,.5f);u && std::abs(*u-.5f)<best){best=std::abs(*u-.5f);found=x;}}
                return best<.01f?found:std::nullopt;};
            const auto yaw=[](float degrees){auto m=vr_camera::IDENTITY;const float r=degrees*3.14159265f/180;m[0]=std::cos(r);m[2]=-std::sin(r);m[8]=std::sin(r);m[10]=std::cos(r);return m;};
            const auto still=centre_column(panel,vr_camera::IDENTITY),follows=centre_column(panel,yaw(15));
            require(still && follows && std::abs(*still-*follows)<.002f,"without an anchor the panel follows the head");
            VrMenuAnchor anchor;const auto t0=std::chrono::steady_clock::now();
            const auto fixed=anchor.apply(panel,vr_camera::IDENTITY,t0);
            require(fixed.anchor && centre_column(fixed,vr_camera::IDENTITY) && std::abs(*centre_column(fixed,vr_camera::IDENTITY)-*still)<.002f,"an anchor at the current head shows the panel where the head-locked one was");
            const auto turned=anchor.apply(panel,yaw(15),t0+std::chrono::milliseconds(100));
            require(turned.anchor==fixed.anchor,"the anchor holds while the panel stays up");
            const auto moved=centre_column(turned,yaw(15));
            require(moved && std::abs(*moved-*still)>.05f,"turning the head with an anchor moves the panel across the eye image");
            const auto back=centre_column(turned,yaw(-15));
            require(back && (*moved-*still)*(*back-*still)<0,"turning the other way moves it the other way");
            const auto h=turned.transform(menu_left.projection,menu_left.runtime_eye_to_head,1,yaw(180));
            require(h && !canvas_u(*h,.5f,.5f),"facing away from the anchor shows no panel");
            const auto later=anchor.apply(panel,yaw(15),t0+std::chrono::milliseconds(100)+VrMenuAnchor::GAP+std::chrono::milliseconds(1));
            require(later.anchor && *later.anchor!=*fixed.anchor,"a panel that went away re-anchors where the head now faces");
            auto pitched=vr_camera::IDENTITY;pitched[5]=std::cos(.5f);pitched[6]=std::sin(.5f);pitched[9]=-std::sin(.5f);pitched[10]=std::cos(.5f);
            VrMenuAnchor level;const auto upright=level.apply(panel,pitched,t0);
            require(upright.anchor && std::abs((*upright.anchor)[9])<1e-5f && std::abs((*upright.anchor)[6])<1e-5f,"the anchor keeps heading only, so a tilted head does not tilt the panel");
        }
        check(menu_capture.capture(host.device.Get(),host.queue.Get(),square.Get(),menu_left));
        check(menu_capture.capture(host.device.Get(),host.queue.Get(),square.Get(),menu_right));
        auto menu_complete=menu_capture.latest(1);wait(menu_complete->fence.Get(),menu_complete->fence_value);
        check(menu_capture.dump_pair(dump,false));
        for(int eye=0;eye<2;++eye) {
            std::ifstream file{dump/(eye?"right.ppm":"left.ppm"),std::ios::binary};std::string contents{std::istreambuf_iterator<char>{file},{}};
            const auto header=std::string{"P6\n128 128\n255\n"};require(contents.starts_with(header),"menu composition must preserve output dimensions");
            unsigned left=128,right=0;
            for(unsigned x=0;x<128;++x) if(static_cast<unsigned char>(contents[header.size()+(64*128+x)*3])>250) {left=std::min(left,x);right=std::max(right,x);}
            const float centre=(left+right+1)/256.f;
            const auto& eye_meta=eye?menu_right:menu_left;
            const float expected=(1+eye_meta.projection[8])/2-eye_meta.projection[0]*eye_meta.runtime_eye_to_head[12]/(2*panel.distance_metres);
            require(std::abs(centre-expected)<1/128.f && left>0 && right<127,"GPU menu panel must converge on the same head-relative plane in asymmetric eyes, with black margins");
        }
        std::filesystem::remove_all(dump);
        menu_left.render_id=22;menu_left.game_token=22;menu_right.render_id=23;menu_right.game_token=23;menu_right.screen_menu=false;
        check(menu_capture.capture(host.device.Get(),host.queue.Get(),square.Get(),menu_left));
        check(menu_capture.capture(host.device.Get(),host.queue.Get(),square.Get(),menu_right));
        menu_complete=menu_capture.latest(1);wait(menu_complete->fence.Get(),menu_complete->fence_value);
        require(!menu_capture.pair(false),"menu transitions must not submit one composed eye paired with one gameplay eye");
        require(capture.dump_pair("relative",false)==E_INVALIDARG && capture.dump_pair(dump)==E_INVALIDARG,"dump requires absolute path and tracked pair unless explicitly diagnostic");
        check(capture.dump_pair(dump,false));
        const auto ppm=[&](const char* name,const std::string& rgb) {
            std::ifstream file{dump/name,std::ios::binary};std::string contents{std::istreambuf_iterator<char>{file},{}};
            require(contents==std::string("P6\n2 2\n255\n")+rgb+rgb+rgb+rgb,"readback must strip GPU row padding and preserve eye colours");
        };
        ppm("left.ppm",std::string{"\xff\0\0",3});ppm("right.ppm",std::string{"\0\0\xff",3});
        require(capture.dump_pair(dump,false)==HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),"dump must refuse overwriting existing evidence");
        std::ifstream metadata{dump/"pair.json"};std::string metadata_text{std::istreambuf_iterator<char>{metadata},{}};
        require(metadata_text.find("\"render_id\":3")!=std::string::npos && metadata_text.find("\"render_id\":4")!=std::string::npos && metadata_text.find("\"tracked_stereo\":false")!=std::string::npos,"dump metadata must describe the actual retained pair");
        metadata.close();std::filesystem::remove_all(dump);
        require(capture.pair(false) && !capture.pair(),"diagnostic mono images must not qualify as tracked stereo");
        require(first_left->texture() != first_right->texture(),"eyes must retain independent textures");
        host.pixels(*first_left,255,0,0); host.pixels(*first_right,0,0,255);
        require(capture.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(5,0,104)) == E_PENDING,"completed GPU copies still cannot overwrite consumer-held images");
        first_left.reset();
        require(capture.capture(host.device.Get(),host.queue.Get(),red.Get(),meta(5,0,104)) == S_OK,"released completed slot may be reused");
        auto newest_left = capture.latest(0); wait(newest_left->fence.Get(),newest_left->fence_value); host.pixels(*newest_left,255,0,0);
        require(capture.pair(false) && capture.pair(false)->left.metadata.render_id == 3 && capture.pair(false)->right.metadata.render_id == 4,
            "completed pair must survive arrival of the next left image until a new right image forms a pair");
        const auto old_generation = newest_left->generation;
        require(capture.reset(),"completed capture can reset while independent image leases survive");
        host.pixels(*first_right,0,0,255);
        auto resized = host.color(0,1,0,4);
        auto tracked_left = meta(6,0,0xffffffffu), tracked_right = meta(7,1,0);
        tracked_left.tracked_stereo = tracked_right.tracked_stereo = true;
        require(capture.capture(host.device.Get(),host.queue.Get(),resized.Get(),tracked_left) == E_INVALIDARG,"tracked metadata needs an actual positive-forward eye projection");
        require(vr_projection::build({-1,1,-1,1},0.1f,1000,tracked_left.projection),"tracked fixture projection must build");
        tracked_right.projection = tracked_left.projection;
        require(capture.capture(host.device.Get(),host.queue.Get(),resized.Get(),tracked_left) == S_OK,"new size and epoch must configure");
        require(capture.capture(host.device.Get(),host.queue.Get(),resized.Get(),tracked_right) == S_OK,"rollover right frame must capture");
        const auto new_right = capture.latest(1); wait(new_right->fence.Get(),new_right->fence_value);
        require(capture.pair() && new_right->generation > old_generation && new_right->texture()->GetDesc().Width == 4,
            "pair must respect tracked metadata, token rollover and new resource generation");
        host.pixels(*new_right,0,255,0);
        EyeTextureCapture bgra_capture;
        auto bgra=host.color(1,0,0,2,DXGI_FORMAT_B8G8R8A8_UNORM);
        check(bgra_capture.capture(host.device.Get(),host.queue.Get(),bgra.Get(),meta(10,0,10)));
        check(bgra_capture.capture(host.device.Get(),host.queue.Get(),bgra.Get(),meta(11,1,11)));
        auto bgra_right=bgra_capture.latest(1);wait(bgra_right->fence.Get(),bgra_right->fence_value);
        check(bgra_capture.dump_pair(dump,false));
        ppm("left.ppm",std::string{"\xff\0\0",3});ppm("right.ppm",std::string{"\xff\0\0",3});
        std::filesystem::remove_all(dump);
        require(capture.capture(nullptr,host.queue.Get(),red.Get(),meta(8,0,1)) == E_INVALIDARG,"invalid device must fail without a copy");
        check(host.queue->Signal(host.fence.Get(),++host.tick)); wait(host.fence.Get(),host.tick);
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(host.device.As(&info))) {
            for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
                SIZE_T size{}; info->GetMessage(index,nullptr,&size); auto memory = std::make_unique<uint8_t[]>(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(memory.get()); check(info->GetMessage(index,message,&size));
                require(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,"D3D12 validation must have no errors");
            }
        }
        // Ordinary-path pause: a separate (differently sized) menu image replaces the scene with the panel.
        // The back buffer can be BGRA while the eye is RGBA: the panel must still read blue as blue.
        for(const auto menu_format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
            auto scene=host.color(1,0,0,64,DXGI_FORMAT_R8G8B8A8_UNORM,64),menu=host.color(0,0,1,48,menu_format,27);
            EyeTextureCapture menu_capture;auto metadata=meta(1,0,600);metadata.tracked_stereo=true;metadata.screen_menu=true;
            require(vr_projection::build({-1,1,-1,1},.1f,100,metadata.projection),"menu test projection");
            require(menu_capture.capture(host.device.Get(),host.queue.Get(),scene.Get(),metadata,D3D12_RESOURCE_STATE_COMMON,nullptr,scene.Get())==E_INVALIDARG,"the scene cannot also be its own menu image");
            auto plain=metadata;plain.screen_menu=false;
            require(menu_capture.capture(host.device.Get(),host.queue.Get(),scene.Get(),plain,D3D12_RESOURCE_STATE_COMMON,nullptr,menu.Get())==E_INVALIDARG,"a menu image requires screen-menu metadata");
            require(menu_capture.capture(host.device.Get(),host.queue.Get(),scene.Get(),metadata,D3D12_RESOURCE_STATE_COMMON,nullptr,menu.Get())==S_OK,"menu panel capture enqueues");
            const auto image=menu_capture.latest(0);wait(image->fence.Get(),image->fence_value);
            const auto desc=image->texture()->GetDesc();require(desc.Width==64 && desc.Height==64,"eye image keeps the eye resolution");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size{};host.device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&size);
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
            host.begin();auto before=barrier(image->texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);host.commands->ResourceBarrier(1,&before);
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=image->texture();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=layout;
            host.commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);auto after=barrier(image->texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);host.commands->ResourceBarrier(1,&after);host.execute();
            void* mapped{};D3D12_RANGE range{0,size_t(size)};check(readback->Map(0,&range,&mapped));
            const auto rgb=[&](unsigned px,unsigned py){const auto* p=static_cast<uint8_t*>(mapped)+layout.Offset+py*layout.Footprint.RowPitch+px*4;return std::array<int,3>{p[0],p[1],p[2]};};
            bool any_scene=false;for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)any_scene|=rgb(x,y)[0]>0;
            const auto centre=rgb(32,32),corner=rgb(1,1);D3D12_RANGE none{};readback->Unmap(0,&none);
            require(centre[2]==255 && centre[0]==0 && corner==std::array<int,3>{0,0,0} && !any_scene,"menu image fills the centred panel, black outside, no frozen scene");
        }
        {   // Guard guide: its quads land in the eye where VrGuardGuide::clip puts them, in the state's colour.
            auto grey=host.color(.5f,.5f,.5f,128,DXGI_FORMAT_R8G8B8A8_UNORM,128);
            const auto read=[&](const EyeTextureCapture::Image& image){
                const auto desc=image.texture()->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size{};host.device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&size);
                D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=size;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(host.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
                host.begin();auto before=barrier(image.texture(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);host.commands->ResourceBarrier(1,&before);
                D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=image.texture();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=layout;
                host.commands->CopyTextureRegion(&target,0,0,0,&source,nullptr);auto after=barrier(image.texture(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);host.commands->ResourceBarrier(1,&after);host.execute();
                void* mapped{};D3D12_RANGE range{0,size_t(size)};check(readback->Map(0,&range,&mapped));
                std::vector<std::array<int,3>> out(128*128);
                for(unsigned y=0;y<128;++y)for(unsigned x=0;x<128;++x){const auto* p=static_cast<uint8_t*>(mapped)+layout.Offset+y*layout.Footprint.RowPitch+x*4;out[y*128+x]={p[0],p[1],p[2]};}
                D3D12_RANGE none{};readback->Unmap(0,&none);return out;
            };
            const VrGuard::Settings guard;const VrGuardGuide::Settings look;
            auto eye=meta(1,0,700);eye.tracked_stereo=true;require(vr_projection::build({-1,1,-1,1},.05f,100,eye.projection),"guide test projection");
            eye.runtime_head[13]=1.7f;eye.runtime_eye_to_head[12]=-.032f;
            // Pixel of the grip marker's centre, from the same projection the renderer uses.
            const auto pixel=[&](const EyeTextureCapture::Metadata& m){
                const auto c=VrGuardGuide::clip(VrGuardGuide::draw(VrGuardGuide::State::idle,guard,look)->quads[0],m.runtime_head,m.runtime_eye_to_head,m.projection);
                float x=0,y=0;for(int k=0;k<4;++k){x+=(*c)[k*4]/(*c)[k*4+3]/4;y+=(*c)[k*4+1]/(*c)[k*4+3]/4;}
                return std::array<unsigned,2>{unsigned((x+1)/2*128),unsigned((1-y)/2*128)};
            };
            EyeTextureCapture guide_capture;uint64_t id=700;
            const auto shot=[&](std::optional<VrGuardGuide::State> state,bool menu=false){
                auto m=eye;m.render_id=m.game_token=uint32_t(++id);
                if(state) m.guard_guide=VrGuardGuide::draw(*state,guard,look);
                m.screen_menu=menu;m.menu_panel.width_degrees=60;
                check(guide_capture.capture(host.device.Get(),host.queue.Get(),grey.Get(),m,D3D12_RESOURCE_STATE_COMMON));
                const auto image=guide_capture.latest(0);wait(image->fence.Get(),image->fence_value);guide_capture.poll();
                return std::pair{*image,read(*image)};
            };
            const auto at=pixel(eye);require(at[0]>8 && at[0]<120 && at[1]>64 && at[1]<120,"the grip marker sits below the centre of view");
            const auto base=shot(std::nullopt).second[2*128+2];
            require(std::abs(base[0]-128)<=1 && base[0]==base[1] && base[1]==base[2],"fixture scene is mid grey");
            const auto expected=[&](VrGuardGuide::State s){const auto c=VrGuardGuide::colour(s,look[VrGuardGuide::OPACITY]);
                std::array<int,3> rgb{};for(int k=0;k<3;++k)rgb[k]=int(std::lround((c[k]*c[3]+(base[k]/255.f)*(1-c[3]))*255));return rgb;};
            const auto close_to=[](std::array<int,3> a,std::array<int,3> b){for(int k=0;k<3;++k)if(std::abs(a[k]-b[k])>3)return false;return true;};
            for(auto s:{VrGuardGuide::State::idle,VrGuardGuide::State::turn,VrGuardGuide::State::blocked,VrGuardGuide::State::active}) {
                const auto [image,rgb]=shot(s);
                require(close_to(rgb[at[1]*128+at[0]],expected(s)),"the grip marker is drawn in the state's colour, alpha blended over the scene");
                require(rgb[2*128+2]==base && rgb[10*128+64]==base,"the rest of the eye is the scene, untouched");
                require(image.metadata.guard_guide && image.metadata.guard_guide->state==s,"image metadata records the drawn guide");
                // The bar continues to the right of the grip marker on the same row.
                bool right_bar=false,left_bar=false;
                for(unsigned x=at[0]+12;x<128 && !right_bar;++x) right_bar=close_to(rgb[at[1]*128+x],expected(s));
                for(int x=int(at[0])-12;x>=0 && !left_bar;--x) left_bar=close_to(rgb[at[1]*128+unsigned(x)],expected(s));
                require(right_bar && left_bar,"the bar extends both sides of the grip marker");
            }
            {const auto [image,rgb]=shot(std::nullopt);
             require(rgb[at[1]*128+at[0]]==base && !image.metadata.guard_guide,"no guide (no sword): the eye is the scene");}
            {const auto [image,rgb]=shot(VrGuardGuide::State::active,true);
             require(!image.metadata.guard_guide,"the menu panel never carries the guide");}
            auto right=eye;right.runtime_eye_to_head[12]=.032f;
            require(pixel(right)[0]<at[0],"the right eye sees the near guide further left: real stereo depth");
            {   // Aim dot: a round dot where VrAimDot::clip puts the game-world hit point, its colour in the
                // middle, a dark ring, the scene outside the disc (the square's corners included).
                EyeTextureCapture dot_capture;auto d=eye;require(vr_projection::build({-.2f,.2f,-.2f,.2f},.05f,100,d.projection),"aim dot projection");
                VrAimDot settings;settings.values.values[VrAimDot::ENABLED]=1;settings.values.values[VrAimDot::SIZE]=4;
                const auto draw=settings.draw({1,-.5f,10});require(draw.has_value(),"aim dot draw");
                const auto dot_shot=[&](bool with_dot,bool menu=false){
                    auto m=d;m.render_id=m.game_token=uint32_t(++id);if(with_dot)m.aim_dot=draw;m.screen_menu=menu;m.menu_panel.width_degrees=60;
                    check(dot_capture.capture(host.device.Get(),host.queue.Get(),grey.Get(),m,D3D12_RESOURCE_STATE_COMMON));
                    const auto image=dot_capture.latest(0);wait(image->fence.Get(),image->fence_value);dot_capture.poll();
                    return std::pair{*image,read(*image)};
                };
                const auto centre=VrAimDot::pixel(*draw,d.world,d.projection,128,128);require(centre && std::abs((*centre)[0]-96)<.5f && std::abs((*centre)[1]-80)<.5f,"the dot's centre where the point projects");
                const unsigned cx=unsigned((*centre)[0]),cy=unsigned((*centre)[1]);
                const auto c=draw->colour;
                std::array<int,3> middle{},ring{};for(int k=0;k<3;++k){middle[k]=int(std::lround((c[k]*c[3]+(base[k]/255.f)*(1-c[3]))*255));ring[k]=int(std::lround((base[k]/255.f)*(1-c[3])*255));}
                {const auto [image,rgb]=dot_shot(true); // Scoped: the capture has two slots and holds the latest.
                 require(close_to(rgb[cy*128+cx],middle),"the middle of the dot is its colour, blended over the scene");
                 require(close_to(rgb[cy*128+cx+9],ring) && close_to(rgb[cy*128+cx-9],ring),"a dark ring around it");
                 require(rgb[cy*128+cx+16]==base && rgb[(cy+9)*128+cx+9]==base && rgb[(cy-9)*128+cx-9]==base,"round: the scene outside the disc and in the square's corners");
                 require(rgb[2*128+2]==base,"the rest of the eye is the scene");
                 require(image.metadata.aim_dot && *image.metadata.aim_dot==*draw,"image metadata records the drawn dot");}
                {const auto [none,plain]=dot_shot(false);require(plain[cy*128+cx]==base && !none.metadata.aim_dot,"no dot: the eye is the scene");}
                {const auto [menu,panel]=dot_shot(true,true);require(!menu.metadata.aim_dot,"the menu panel never carries the dot");}
            }
            // Comfort vignette (riding along with the jump pack): black at the edge of the eye, the
            // centre of view untouched, every pixel as vr_vignette::alpha says, in an asymmetric eye.
            EyeTextureCapture vignette_capture;auto v=eye;require(vr_projection::build({-1.2f,.8f,-1,1},.05f,100,v.projection),"vignette test projection");
            const auto vignette_shot=[&](float strength,bool tracked=true,bool menu=false){
                auto m=v;m.render_id=m.game_token=uint32_t(++id);m.comfort_vignette=strength;m.tracked_stereo=tracked;m.screen_menu=menu;m.menu_panel.width_degrees=60;
                check(vignette_capture.capture(host.device.Get(),host.queue.Get(),grey.Get(),m,D3D12_RESOURCE_STATE_COMMON));
                const auto image=vignette_capture.latest(0);wait(image->fence.Get(),image->fence_value);vignette_capture.poll();
                return std::pair{*image,read(*image)};
            };
            const auto ndc=[](unsigned x,unsigned y){return std::array<float,2>{(x+.5f)/64-1,1-(y+.5f)/64};};
            for(float strength:{1.f,.4f}) {
                const auto d=*vr_vignette::draw(strength,v.projection);
                const auto [image,rgb]=vignette_shot(strength);
                require(image.metadata.comfort_vignette==strength,"image metadata records the vignette drawn");
                int worst=0;
                for(unsigned y=0;y<128;y+=3)for(unsigned x=0;x<128;x+=3){const auto n=ndc(x,y);const int want=int(std::lround(base[0]*(1-vr_vignette::alpha(d,n[0],n[1]))));worst=std::max(worst,std::abs(rgb[y*128+x][0]-want));}
                require(worst<=3,("the vignette matches vr_vignette::alpha across the eye (worst "+std::to_string(worst)+")").c_str());
                const unsigned cx=unsigned((1+v.projection[8])*64),cy=unsigned((1-v.projection[9])*64);
                require(rgb[cy*128+cx]==base,"the eye's own forward axis is untouched");
                require(rgb[0][0]<base[0]/2,"the outer corner is dark");
                if(strength==1) require(rgb[64*128+10][0]<rgb[64*128+120][0],"the wider (outer) side of the eye darkens first: it is centred on the eye's axis, not the image");
            }
            require(vignette_shot(1).second[64*128+20][0]<vignette_shot(.4f).second[64*128+20][0],"a stronger vignette is darker at the same point");
            for(const auto& [strength,tracked,menu]:{std::tuple{0.f,true,false},std::tuple{1.f,false,false},std::tuple{1.f,true,true}}) {
                const auto [image,rgb]=vignette_shot(strength,tracked,menu);
                require(image.metadata.comfort_vignette==0,"strength 0, an untracked eye or the menu panel: no vignette, and the metadata says so");
                if(!menu) require(rgb[0]==base && rgb[127*128+127]==base,"and the eye is the scene");
            }
        }
        std::cout << "WARP retained-eye pixel, fence, lease, pair and resize checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
