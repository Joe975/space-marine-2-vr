#include "NativeHzbCulling.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
using namespace native_hzb_culling;
void check(bool ok,const char* label){if(!ok){std::cerr<<label<<'\n';std::exit(1);}}
int main(){
    uint8_t memory=1;unsigned writes{};
    const auto read=[&](uint8_t& v){v=memory;return true;};
    const auto write=[&](uint8_t v){++writes;memory=v;return true;};
    check(apply(IMAGE_TIMESTAMP,false,read,write)==Result::disabled && memory==0 && writes==1,"native enabled switch is cleared once");
    check(apply(IMAGE_TIMESTAMP,false,read,write)==Result::already_disabled && writes==1,"already cleared switch is not rewritten");
    memory=1;check(apply(IMAGE_TIMESTAMP,true,read,write)==Result::kept_native && memory==1 && writes==1,"opt-out keeps native HZB culling untouched");
    check(apply(IMAGE_TIMESTAMP+1,false,read,write)==Result::unsupported_build && memory==1 && writes==1,"other builds are never written");
    memory=7;check(apply(IMAGE_TIMESTAMP,false,read,write)==Result::unexpected_value && memory==7 && writes==1,"a non-boolean byte is not a known switch and is left alone");
    memory=1;check(apply(IMAGE_TIMESTAMP,false,[](uint8_t&){return false;},write)==Result::unexpected_value && writes==1,"unreadable switch refuses before writing");
    check(apply(IMAGE_TIMESTAMP,false,read,[](uint8_t){return true;})==Result::write_failed && memory==1,"a write that does not stick is reported");
    check(parse_mode("off\r\n")==Mode::off && parse_mode("native\n")==Mode::native && parse_mode("left_only")==Mode::left_only,"control values parse");
    check(!parse_mode("") && !parse_mode("left") && !parse_mode("LEFT_ONLY"),"anything else keeps the current mode");
    check(!switch_value(Mode::off) && switch_value(Mode::native) && switch_value(Mode::left_only),"only off clears the switch");
    check(parse_mode("shared")==Mode::shared && switch_value(Mode::shared),"shared turns the game's culling on");
    check(second_eye_frustum_only(Mode::left_only) && second_eye_frustum_only(Mode::shared) && !second_eye_frustum_only(Mode::native) && !second_eye_frustum_only(Mode::off),"native keeps slot 1 in the HZB test, the others take it out");
    check(parse_mode("shared_objects")==Mode::shared_objects,"objects-only parses");
    check(objects_inherit(Mode::shared) && objects_inherit(Mode::shared_objects) && !objects_inherit(Mode::left_only),"both shared modes inherit objects");
    check(lwi_inherits(Mode::shared) && !lwi_inherits(Mode::shared_objects) && !lwi_inherits(Mode::left_only) && !lwi_inherits(Mode::native),"only shared touches rocks and foliage");
    check(second_eye_frustum_only(Mode::shared_objects),"objects-only also takes slot 1 out of the HZB test");
    // mod-authored: the instructions the mod writes, not the game's.
    {constexpr LwiKey want{0xc6,0x45,0x10,0x01,0x75,0x11,0x83,0xff,0x01,0x77,0x0c,0x80,0x3d,0x78,0x56,0x34,0x12,0x00,0x0f,0x94,0x45,0x10,0x90};
     check(lwi_key_patch(0x12345678)==want,"the key patch is exactly these instructions");
     check(lwi_key_patch(-2)[13]==0xfe && lwi_key_patch(-2)[16]==0xff,"a negative displacement encodes little endian");
     check(lwi_switch_disp(0x1000,0x2000)==0x2000-0x1012,"the displacement counts from the end of the cmp, past its immediate");
     check(0x06+5==LWI_KEY_SIZE-0x17+0x0b && 0x0b+7==LWI_SWITCH_END,"both skips land on the first byte after the site");}
    check(inherit(0b1110,true)==0b1100,"hidden from the left eye inside its view: the right eye skips it");
    check(inherit(0b0010,true)==0,"an object left with no view is dropped");
    check(inherit(0b1110,false)==0b1110,"outside the left eye's view the left bit says nothing: the right eye keeps it");
    check(inherit(0b1111,true)==0b1111,"seen by both: untouched");
    check(inherit(0b1101,true)==0b1101,"seen by the left eye only: untouched");
    check(inherit(0b1100,true)==0b1100,"shadow camera bits are never changed");
    check(!inherit_candidate(0b0011) && inherit_candidate(0b1010) && !inherit_candidate(0),"only right-without-left needs the frustum test");
    {   // A row-vector matrix with clip x = x, clip w = w: points through it land where the widened view puts them.
        float m[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};widen_view(m,1.5f);
        const auto ndc=[&](float x){return (x*m[0]+m[12]*1)/(x*m[3]+m[15]*1);};
        const auto equal=[](float a,float b){return a-b<1e-5f && b-a<1e-5f;};
        check(equal(ndc(-1),-1),"the left eye's far side stays at the HZB's edge");
        check(equal(ndc(1.5f),1),"the widened edge reaches the HZB's other edge");
        check(ndc(1)<0.7f,"the left eye's own edge is now well inside the HZB, so a box crossing it isn't clamped there");
        check(equal(m[5],1) && equal(m[10],1) && equal(m[15],1) && equal(m[3],0),"y, z and w are untouched");
        float rows[16]{2,0,0,1, 0,3,0,0, 0,0,4,5, 6,0,0,7};widen_view(rows,1.5f);
        check(equal(rows[0],2*0.8f-0.2f) && equal(rows[12],6*0.8f-7*0.2f) && equal(rows[3],1) && equal(rows[15],7),"every row: x takes w, nothing else moves");}
    {   // Row-vector projections looking down +z, with the tangents of each edge. Canted turns the view about y.
        const auto eye=[](float left_tan,float right_tan,float canted=0){
            std::array<float,16> m{};const float c=std::cos(canted),s=std::sin(canted),width=right_tan-left_tan;
            const float x=2/width,z=-(right_tan+left_tan)/width;
            // view x = c*px + s*pz, view z = -s*px + c*pz; clip x = x*view x + z*view z, clip w = view z.
            m[0]=x*c-z*s;m[8]=x*s+z*c;m[5]=1;m[3]=-s;m[11]=c;m[15]=0;return m;};
        const auto equal=[](float a,float b){return std::abs(a-b)<1e-3f;};
        const auto narrow=eye(-1,1);
        check(equal(wide_edge(narrow.data(),narrow.data()),1+WIDE_MARGIN),"parallel, matching views reach only the margin past the left eye's edge");
        const auto left=eye(-1.39f,.87f),right=eye(-.87f,1.39f);
        check(equal(wide_edge(left.data(),right.data()),(2*1.39f+1.39f-.87f)/(1.39f+.87f)+WIDE_MARGIN),"a wider outer edge is read from the matrices");
        const auto canted=eye(-.87f,1.39f,-.1f);
        check(wide_edge(left.data(),canted.data())>wide_edge(left.data(),right.data()),"a right eye turned outward reaches further");
        const auto vast=eye(-.87f,12,-.3f);
        check(wide_edge(left.data(),vast.data())==0,"too wide for one view: no sharing");
        const std::array<float,16> zero{};
        check(wide_edge(left.data(),zero.data())==0 && wide_edge(zero.data(),right.data())==0,"an unreadable matrix: no sharing");}
    {   // An entry's two matrices through one widened frame.
        std::array<uint8_t,ENTRY_RELATIVE_VIEW_PROJECTION+64> entry{};
        const auto matrix=[&](size_t offset){WideView::Matrix m;std::memcpy(m.data(),entry.data()+offset,sizeof(m));return m;};
        const auto set=[&](size_t offset,const WideView::Matrix& m){std::memcpy(entry.data()+offset,m.data(),sizeof(m));};
        const WideView::Matrix game{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},fresh{2,0,0,0, 0,2,0,0, 0,0,2,0, 0,0,0,1};
        set(ENTRY_VIEW_PROJECTION,game);set(ENTRY_RELATIVE_VIEW_PROJECTION,game);
        WideView view;view.settle();check(matrix(ENTRY_VIEW_PROJECTION)==game,"nothing to end");
        view.widen(entry.data(),1.5f);
        check(matrix(ENTRY_VIEW_PROJECTION)!=game && matrix(ENTRY_RELATIVE_VIEW_PROJECTION)!=game,"both matrices widen");
        view.settle();check(matrix(ENTRY_VIEW_PROJECTION)==game && matrix(ENTRY_RELATIVE_VIEW_PROJECTION)==game,"the LWI pass puts them back");
        view.settle();check(matrix(ENTRY_VIEW_PROJECTION)==game,"once");
        // No LWI pass this frame, and the game writes the next frame's matrices over the widened ones.
        view.widen(entry.data(),1.5f);set(ENTRY_VIEW_PROJECTION,fresh);set(ENTRY_RELATIVE_VIEW_PROJECTION,fresh);
        view.settle();check(matrix(ENTRY_VIEW_PROJECTION)==fresh && matrix(ENTRY_RELATIVE_VIEW_PROJECTION)==fresh,"a fresh matrix never gets last frame's back");
        // No LWI pass and no new matrices: widening again must not stack on the widened view.
        view.widen(entry.data(),1.5f);const auto once=matrix(ENTRY_VIEW_PROJECTION);view.widen(entry.data(),1.5f);
        check(matrix(ENTRY_VIEW_PROJECTION)==once,"a second widen starts from the game's matrix");
        view.settle();check(matrix(ENTRY_VIEW_PROJECTION)==fresh,"and still ends there");}
    {   // One plane x >= 10 and one z <= 100. Corner indices name the far corner along each normal.
        struct Plane {float n[3];float d;uint8_t pad,x,y,z;};static_assert(sizeof(Plane)==PLANE_SIZE);
        struct {int32_t count;Plane planes[2];} frustum{2,{{{1,0,0},10,0,3,1,2},{{0,0,-1},-100,0,0,1,2}}};
        const auto* raw=reinterpret_cast<const uint8_t*>(&frustum);
        const float inside_box[BOX_FLOATS]{20,0,0,30,5,50};
        check(wholly_inside(raw,inside_box),"a box past both planes is wholly inside");
        const float straddle[BOX_FLOATS]{5,0,0,30,5,50};
        check(!wholly_inside(raw,straddle),"a box across a plane is not, though a touch test would pass it");
        const float beyond[BOX_FLOATS]{20,0,0,30,5,150};
        check(!wholly_inside(raw,beyond),"a box across the second plane is not");
        const float outside[BOX_FLOATS]{-20,0,0,-10,5,50};
        check(!wholly_inside(raw,outside),"a box behind a plane is not");
        const float exact[BOX_FLOATS]{10,0,0,30,5,100};
        check(wholly_inside(raw,exact),"touching the plane from inside counts");
        frustum.count=0;check(!wholly_inside(raw,inside_box),"no planes: not provably inside");
        frustum.count=13;check(!wholly_inside(raw,inside_box),"a bad plane count refuses");
        frustum.count=2;frustum.planes[1].y=9;check(!wholly_inside(raw,inside_box),"a bad corner index refuses");
        const float nan_box[BOX_FLOATS]{std::nanf(""),0,0,30,5,50};frustum.planes[1].y=1;
        check(!wholly_inside(raw,nan_box),"bad bounds keep the right eye drawing");
        check(opposite_corner(0)==3 && opposite_corner(5)==2,"corners pair across the box");}
    check(DEFAULT_MODE==Mode::shared,"both eyes cull unless a launch opts out");
    {   const float left[3]{1,2,3},play[3]{1.09f,2,3},limit[3]{1.2999f,2,3},tabletop[3]{1,2,3.5f},diagonal[3]{1.2f,2.2f,3.2f};
        check(eyes_close(left,play),"play spacing shares");check(eyes_close(left,left),"same pose shares");
        check(eyes_close(left,limit),"just inside the limit shares");
        check(!eyes_close(left,tabletop),"camera mode spacing drops to left_only");
        check(!eyes_close(left,diagonal),"the gap counts all three axes");}
    constexpr uint32_t eye=0x7f7c013;
    check(hzb_camera(eye),"the eyes' live flags are in the rebuild loop");
    check(stereo_pair(2,eye,0,eye,1) && stereo_pair(10,eye,0,eye,1),"the two eyes are found, with shadow cameras after them");
    check(!stereo_pair(1,eye,0,eye,1),"mono has no second eye");
    check(!stereo_pair(65,eye,0,eye,1) && !stereo_pair(-1,eye,0,eye,1),"a bad camera count is refused");
    check(!stereo_pair(2,eye,0,eye,2) && !stereo_pair(2,eye,1,eye,0),"slots must be 0 and 1");
    check(!stereo_pair(2,eye,0,eye|FLAG_NO_HZB,1),"a camera already out of HZB is left alone");
    check(!stereo_pair(2,eye,0,eye&~FLAG_MAIN,1),"a shadow camera in slot 1 is not the second eye");
    check(!stereo_pair(2,eye,0,eye|0x4,1),"a view with flag 4 is not rebuilt, so it is not an eye");
    uint32_t right=eye;
    {ScopedSecondEye hold{&right};check(right==(eye|FLAG_NO_HZB),"the bit is set for the native call");check(!hzb_camera(right),"slot 1 leaves the rebuild loop");}
    check(right==eye,"the bit is gone after the native call, so validators never see it");
    {ScopedSecondEye none{nullptr};}
    std::cout<<"Native HZB culling tests passed\n";
}
