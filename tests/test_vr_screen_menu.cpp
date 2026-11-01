#include "GpuTestHost.h"
#include "UiCoverageProbe.h"
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
using std::chrono::milliseconds;

void coverage_cells() {
    require(ui_coverage::covered({})==0,"no cells, no coverage");
    const std::vector<uint8_t> black(64*36,0);
    require(ui_coverage::covered(black)==0,"the stereo back buffer behind the HUD is black: nothing covered");
    std::vector<uint8_t> cells(100,0);for(int i=0;i<25;++i)cells[i]=200;for(int i=25;i<50;++i)cells[i]=2;
    require(ui_coverage::covered(cells)==.25f,"cells at 2 of 255 (dither) are not UI; bright cells are");
    cells.assign(100,ui_coverage::COVERED);require(ui_coverage::covered(cells)==1,"a dark menu background still covers its cells");
}

void detector() {
    const auto t0=Clock::time_point{std::chrono::seconds(100)};int step=0;
    VrScreenMenuDetector d;
    const auto at=[&]{return t0+milliseconds(100*++step);};
    for(int i=0;i<20;++i)require(!d.update(.07f,at()),"walking the barge (7% HUD) never shows the panel");
    require(!d.update(.88f,at()) && !d.update(.88f,at()),"two samples of a full-screen menu are not enough (a flash)");
    require(d.update(.88f,at()) && d.active(t0+milliseconds(100*step)),"the third in a row shows the panel");
    require(d.update(.5f,at()) && d.update(.4f,at()),"in between holds the state");
    require(d.update(.2f,at()),"one low sample (a menu transition) keeps it");
    require(!d.update(.06f,at()),"the second low sample in a row closes it");
    for(int i=0;i<2;++i)d.update(.9f,at());
    require(!d.update(.2f,at()) && !d.update(.9f,at()) && !d.update(.9f,at()) && d.update(.9f,at()),"a low sample restarts the count");
    require(d.update(std::nullopt,at()),"a mono frame (not counted) changes nothing");
    step+=11;require(!d.update(std::nullopt,at()) && !d.active(t0+milliseconds(100*step)),"no stereo sample for over a second: off");
    {   // 26 Oct: the battle simulator loadout menu (0.53-0.59) shows on the panel; play (at most 0.27 seen) never does.
        VrScreenMenuDetector sim;int n=0;const auto when=[&]{return t0+milliseconds(100*++n);};
        for(int i=0;i<50;++i)require(!sim.update(i%2?.27f:.15f,when()),"the busiest play HUD seen (0.27) never shows the panel");
        require(!sim.update(.53f,when()) && !sim.update(.57f,when()) && sim.update(.59f,when()),"the battle simulator loadout menu shows on the panel");
        require(sim.update(.33f,when()) && sim.update(.31f,when()),"a dip inside the menu keeps it");}
    VrScreenMenuDetector mono;for(int i=0;i<30;++i)require(!mono.update(std::nullopt,t0+milliseconds(100*i)),"mono frames alone never show the panel");
    {   // 31 Oct 18:46: the panel takes three samples, and until 18:46:43.15 the gaze camera's right stick
        // was the open loadout menu's scroll. It holds off from the first sample at ON.
        VrScreenMenuDetector open;auto time=t0;const auto sample=[&](std::optional<float> coverage,int ms=100){time+=milliseconds(ms);open.update(coverage,time);};
        sample(.27f);require(!open.opening(time),"the busiest play HUD isn't a menu opening");
        sample(.57f);require(open.opening(time) && !open.active(time),"the first sample at ON: a menu may be opening, no panel yet");
        sample(.59f);require(open.opening(time) && !open.active(time),"still opening on the second");
        sample(.53f);require(!open.opening(time) && open.active(time),"the third shows the panel: opening is over");
        for(const float low:{.2f,.1f})sample(low);require(!open.opening(time) && !open.active(time),"closed again");
        sample(.57f);require(open.opening(time),"a flash also holds the gaze camera off for its one sample");
        sample(.2f);require(!open.opening(time),"and the next low sample lets it steer");
        sample(.57f);sample(.4f);require(!open.opening(time),"a sample in between isn't opening either");
        sample(.57f);require(open.opening(time) && !open.opening(time+milliseconds(1100)),"no stereo sample for over a second: not opening");
    }
}

// A back buffer stand-in in PRESENT (COMMON) state: black, with rects cleared to a colour.
ComPtr<ID3D12Resource> canvas(Host& host,DXGI_FORMAT format,std::initializer_list<D3D12_RECT> rects,float value) {
    auto texture=host.color(0,0,0,256,format,144);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> rtv;check(host.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&rtv)));
    const auto handle=rtv->GetCPUDescriptorHandleForHeapStart();host.device->CreateRenderTargetView(texture.Get(),nullptr,handle);
    host.begin();auto before=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&before);
    const float rgba[]{value,value,value,1};const std::vector<D3D12_RECT> list(rects);
    if(!list.empty())host.commands->ClearRenderTargetView(handle,rgba,UINT(list.size()),list.data());
    auto after=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();
    return texture;
}

UiCoverageProbe::Sample measure(Host& host,UiCoverageProbe& probe,ID3D12Resource* source,bool counts=true) {
    check(probe.begin(host.queue.Get(),source,counts));
    require(probe.busy() && probe.begin(host.queue.Get(),source,counts)==E_INVALIDARG,"one sample in flight at a time");
    check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
    const auto sample=probe.take();require(sample.has_value() && !probe.busy(),"a completed sample is taken once");
    require(!probe.take(),"and only once");
    return *sample;
}

void gpu_probe() {
    Host host;UiCoverageProbe probe;
    const auto black=canvas(host,DXGI_FORMAT_R8G8B8A8_UNORM,{},0);
    require(measure(host,probe,black.Get()).coverage==0,"GPU: a black back buffer is uncovered");
    // Left half lit with a dark menu background (10 of 255): half the 64x36 cells.
    const auto half=canvas(host,DXGI_FORMAT_B8G8R8A8_UNORM,{D3D12_RECT{0,0,128,144}},10.f/255);
    const auto sample=measure(host,probe,half.Get(),false);
    require(sample.coverage==.5f && !sample.counts,"GPU: the left half of a BGRA back buffer covers half the cells; counts is kept");
    require(probe.cells()[0]>=ui_coverage::COVERED && probe.cells()[63]==0,"cells are laid out left to right");
    // A single 2x2 HUD element lights only its own cell: 8x8 samples per cell find it.
    const auto dot=canvas(host,DXGI_FORMAT_R8G8B8A8_UNORM,{D3D12_RECT{201,101,203,103}},1);
    require(measure(host,probe,dot.Get()).coverage==1.f/(64*36),"GPU: a small bright element lights one cell");
    const auto dim=canvas(host,DXGI_FORMAT_R8G8B8A8_UNORM,{D3D12_RECT{0,0,256,144}},1.f/255);
    require(measure(host,probe,dim.Get()).coverage==0,"GPU: 1 of 255 everywhere is still black");
    // Restored state: the source is usable as a render target again (debug layer checks states).
    host.begin();auto before=barrier(half.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);host.commands->ResourceBarrier(1,&before);
    auto after=barrier(half.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&after);host.execute();
    const auto tiny=host.color(1,1,1,32,DXGI_FORMAT_R8G8B8A8_UNORM,16);
    require(probe.begin(host.queue.Get(),tiny.Get(),true)==E_INVALIDARG && !probe.busy(),"a source smaller than the grid is refused");
    require(probe.begin(nullptr,half.Get(),true)==E_INVALIDARG && probe.begin(host.queue.Get(),nullptr,true)==E_INVALIDARG,"null queue or source refused");
    probe.release();require(measure(host,probe,half.Get()).coverage==.5f,"recreated after release");
}
}

int main() {
    try {
        coverage_cells();detector();gpu_probe();
        std::cout<<"VR screen menu tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
