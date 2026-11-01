// docs/BODY_SCALE.md: eye-height readings, the clamped factor, the calibration field, and
// holster zones fitting a shorter or taller player once their hand is body-scaled.
#include "VrBodyScale.h"
#include "VrCalibration.h"
#include "VrHolster.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
bool close(float a,float b) { return std::abs(a-b)<1e-5f; }
using Reading=VrBodyScale::Reading;

void readings() {
    VrBodyScale b;
    require(!b.height() && !b.last() && b.factor(1.64f)==1,"unmeasured: factor 1");
    require(b.measure(0.9f)==Reading::implausible && b.measure(2.2f)==Reading::implausible && b.measure(NAN)==Reading::implausible && !b.height(),
        "outside 1.0-2.1 m is not a standing eye height");
    require(b.measure(1.45f)==Reading::accepted && b.height()==1.45f,"first plausible reading is held");
    require(b.measure(1.31f)==Reading::accepted && b.height()==1.31f,"a reading within 0.15 m of the highest is accepted, even lower");
    require(b.measure(1.29f)==Reading::crouched && b.height()==1.31f && b.last()==Reading::crouched,"more than 0.15 m below the highest is a crouch");
    require(b.measure(1.70f)==Reading::accepted && b.height()==1.70f,"taller is accepted");
    require(b.measure(1.50f)==Reading::crouched && b.height()==1.70f,"the limit follows the highest reading");
    require(b.measure(1.56f)==Reading::accepted,"just within 0.15 m below is still standing");
}
void factors() {
    require(close(VrBodyScale::factor(1.64f,1.64f),1),"Joe: 1");
    require(close(VrBodyScale::factor(1.64f,1.40f),1.64f/1.40f),"shorter player: larger scale");
    require(close(VrBodyScale::factor(1.64f,1.90f),1.64f/1.90f),"taller player: smaller scale");
    require(close(VrBodyScale::factor(2.0f,1.0f),1.35f) && close(VrBodyScale::factor(1.0f,2.0f),0.75f),"clamped to 0.75-1.35");
    require(VrBodyScale::factor(std::nullopt,1.4f)==1 && VrBodyScale::factor(1.64f,std::nullopt)==1 && VrBodyScale::factor(0.f,1.4f)==1,"missing or bad heights: 1");
}
void calibration() {
    std::istringstream old{"units_per_metre=0.9\nheight_offset_metres=0.05\nhead_forward_offset_metres=-0.2\n"};
    const auto a=VrCalibration::read(old);
    require(a && !a->reference_eye_height_metres,"older presets load with no reference (no body scale)");
    std::istringstream with{"units_per_metre=0.9\nheight_offset_metres=0.05\nreference_eye_height_metres=1.644\n"};
    const auto b=VrCalibration::read(with);
    require(b && b->reference_eye_height_metres==1.644f,"reference eye height parses");
    std::ostringstream out;require(b->write(out),"writes");
    std::istringstream back{out.str()};require(VrCalibration::read(back)==b,"round-trips");
    std::ostringstream plain;require(a->write(plain) && plain.str().find("reference_eye_height")==std::string::npos,"absent field is not written");
    for(const char* bad:{"units_per_metre=0.9\nheight_offset_metres=0\nreference_eye_height_metres=0.5\n",
                         "units_per_metre=0.9\nheight_offset_metres=0\nreference_eye_height_metres=2.5\n",
                         "units_per_metre=0.9\nheight_offset_metres=0\nreference_eye_height_metres=1.6\nreference_eye_height_metres=1.6\n"}) {
        std::istringstream in{bad};require(!VrCalibration::read(in),"out-of-range or duplicate reference rejects the file");
    }
    auto c=*b;c.reference_eye_height_metres=1.5f;require(!c.same_placement(*b),"changing the reference recenters");
    std::istringstream scaled{"units_per_metre=0.9\nheight_offset_metres=0.1\nfirst_person_units_per_metre=1.45\n"};
    const auto s=VrCalibration::read(scaled);
    require(s && s->first_person_units_per_metre==1.45f && close(s->first_person_factor(),1.45f/.9f),"first-person scale parses");
    std::ostringstream scaled_out;require(s->write(scaled_out),"writes the first-person scale");
    std::istringstream scaled_back{scaled_out.str()};require(VrCalibration::read(scaled_back)==s,"first-person scale round-trips");
    require(!a->first_person_units_per_metre && a->first_person_factor()==1 && plain.str().find("first_person")==std::string::npos,"older presets: the calibration's own scale, nothing written");
    for(const char* bad:{"units_per_metre=0.9\nheight_offset_metres=0\nfirst_person_units_per_metre=0.4\n",
                         "units_per_metre=0.9\nheight_offset_metres=0\nfirst_person_units_per_metre=3.5\n",
                         "units_per_metre=0.9\nheight_offset_metres=0\nfirst_person_units_per_metre=1.4\nfirst_person_units_per_metre=1.4\n"}) {
        std::istringstream in{bad};require(!VrCalibration::read(in),"out-of-range or duplicate first-person scale rejects the file");
    }
    auto d=*s;d.first_person_units_per_metre=1.3f;require(!d.same_placement(*s),"a new first-person scale recenters, which is how it applies live");
}
// Head at eye height h, hand at an offset given in Joe's metres, scaled to a body of height h.
void holsters() {
    constexpr float JOE=1.644f;
    struct Draw { float lateral,height,forward; VrHolster::Slot slot; };
    // Joe's recorded draws (VrHolster.h): shoulder about 0.1 m below, 0.2 m right, 0.2 m behind;
    // hip about 0.7 m below, 0.25 m right.
    const Draw draws[]{{0.2f,-0.1f,-0.2f,VrHolster::Slot::primary},{0.25f,-0.7f,0.f,VrHolster::Slot::secondary},{0.25f,-0.5f,0.f,VrHolster::Slot::secondary},
        {0.2f,0.5f,0.f,VrHolster::Slot::salute}};
    for(const float height:{1.30f,1.45f,JOE,1.85f,2.0f}) {
        const float body=height/JOE,factor=VrBodyScale::factor(JOE,height);
        auto head=vr_camera::IDENTITY;head[13]=height;
        for(const auto& d:draws) {
            auto hand=vr_camera::IDENTITY;hand[12]=d.lateral*body;hand[13]=height+d.height*body;hand[14]=-d.forward*body;
            VrHolster h;
            require(h.classify(head,vr_body::scaled_from_head(head,hand,factor))==d.slot,"a proportional draw hits the same zone for any player in the clamp range");
        }
    }
    // Unscaled, a 1.30 m player's high hip draw (0.4 m below the eyes) lands in the shoulder zone.
    auto head=vr_camera::IDENTITY;head[13]=1.30f;auto hand=vr_camera::IDENTITY;hand[12]=.25f*1.3f/JOE;hand[13]=1.30f-.5f*1.3f/JOE;
    VrHolster h;require(h.classify(head,hand)==VrHolster::Slot::primary,"without body scale a short player's hip draw selects the wrong weapon (why this exists)");
    require(vr_body::scaled_from_head(head,hand,1)==hand,"factor 1 leaves the hand alone");
}
} // namespace

int main() {
    try { readings();factors();calibration();holsters(); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
    std::cout<<"vr_body_scale tests passed\n";return 0;
}
