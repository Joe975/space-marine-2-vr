#include "VrPhoto.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace vr_photo;
void require(bool ok,const std::string& why) {if(!ok) throw std::runtime_error{why};}

void one_at_a_time() {
    using namespace std::chrono;
    const auto t=Clock::time_point{seconds(100)};
    Flight f;
    require(f.idle() && f.request(t),"a click starts a picture");
    require(!f.request(t+milliseconds(5)) && !f.request(t+seconds(9)),"clicks during it are refused, however many");
    require(f.clicked()==t,"a refused click doesn't move the deadline");
    f.preparing();f.armed();f.copying();
    require(f.stage()==Flight::Stage::copying && !f.request(t+seconds(9)),"still one in flight while the GPU copies");
    f.finishing();require(!f.request(t+seconds(9)),"still one in flight while the file is written");
    f.finish();require(f.idle() && f.request(t+seconds(10)) && f.clicked()==t+seconds(10),"the next click starts the next picture");
}

void no_eye_image_times_out() {
    using namespace std::chrono;
    const auto t=Clock::time_point{seconds(100)};
    Flight f;f.request(t);
    require(!f.expired(t+EYE_WAIT-milliseconds(1)),"still waiting for an eye");
    require(f.expired(t+EYE_WAIT),"no eye image in time");
    f.preparing();require(f.expired(t+EYE_WAIT),"the buffer ready but no eye still times out");
    f.armed();require(f.expired(t+EYE_WAIT),"armed with no eye still times out");
    f.copying();require(!f.expired(t+EYE_WAIT+seconds(5)),"a copy on its way is the save thread's to end");
    f.finishing();require(!f.expired(t+seconds(60)),"so is a file being written");
    Flight idle;require(!idle.expired(t+seconds(60)),"nothing to time out");
    Flight out_of_order;out_of_order.armed();out_of_order.copying();require(out_of_order.idle(),"no stage without a click");
}

void names() {
    const Stamp t{2026,11,2,14,3,7};
    require(file_name(t)=="SM2VR_2026-11-02_14-03-07.png","a timestamped name");
    require(file_name(t,1)==file_name(t) && file_name(t,2)=="SM2VR_2026-11-02_14-03-07_2.png" && file_name(t,12)=="SM2VR_2026-11-02_14-03-07_12.png","a second picture in the same second");
    require(file_name({2026,12,31,23,59,59})>file_name({2026,12,31,23,59,58}) && file_name({2027,1,1,0,0,0})>file_name({2026,12,31,23,59,59}),"names sort by time");
}

void pixels() {
    // Two rows with a wider pitch, as a readback footprint has.
    std::vector<uint8_t> mapped(16*2,0xee),rows;
    const uint8_t first[]{10,20,30,0, 40,50,60,255},second[]{70,80,90,7, 100,110,120,0};
    std::memcpy(mapped.data(),first,8);std::memcpy(mapped.data()+16,second,8);
    require(bgr_rows(mapped.data(),16,2,2,true,rows),"BGRA converts");
    require(rows==std::vector<uint8_t>{10,20,30,40,50,60,70,80,90,100,110,120},"BGRA keeps its order, drops alpha, skips the padding, top row first");
    require(bgr_rows(mapped.data(),16,2,2,false,rows),"RGBA converts");
    require(rows==std::vector<uint8_t>{30,20,10,60,50,40,90,80,70,120,110,100},"RGBA swaps red and blue");
    require(!bgr_rows(nullptr,16,2,2,true,rows) && !bgr_rows(mapped.data(),7,2,2,true,rows) && !bgr_rows(mapped.data(),16,0,2,true,rows) && !bgr_rows(mapped.data(),16,2,0,true,rows),"refuses a bad image");
}

// 2 Nov, live: 3840x2160 holding the Index's view (0.92) came out 1.9 times too wide.
void true_shape() {
    const float index=view_aspect(.770f,.708f);
    require(std::abs(index-.9195f)<.001f,"the view's width over its height, from the projection");
    require(view_aspect(0,1)==0,"no projection, no shape");
    const auto size=picture_size(3840,2160,index);
    require(std::abs(float(size.width)/size.height-index)<.001f,"the view's shape");
    const double pixels=double(size.width)*size.height;
    require(std::abs(pixels/(3840.0*2160)-1)<.002,"the same pixel count");
    require(size.height>2160 && size.width<3840,"narrower and taller than the texture");
    const Size eye{3840,2160};
    require(picture_size(3840,2160,3840.f/2160)==eye && picture_size(3840,2160,1.77f)==eye,"already the right shape: untouched");
    for(const float bad:{0.f,-1.f,.1f,9.f,NAN,INFINITY}) require(picture_size(3840,2160,bad)==eye,"a shape not to trust leaves the image alone");
    require(picture_size(0,2160,1)==Size{0,2160},"no width");
}

void shutter_pulse() {
    const auto c=pulse(160);
    require(c[0].op==vr_rumble::Command::Op::none,"the left hand stays quiet");
    require(c[1].op==vr_rumble::Command::Op::apply && c[1].frequency==160 && c[1].duration==PULSE && c[1].amplitude>0 && c[1].amplitude<=1,"a short pulse on the right hand, at the session's frequency");
    require(PULSE<=std::chrono::milliseconds(100),"short");
}
}

int main() {
    try {one_at_a_time();no_eye_image_times_out();names();pixels();true_shape();shutter_pulse();}
    catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
    std::cout<<"vr_photo tests passed\n";return 0;
}
