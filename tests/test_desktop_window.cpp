#include "DesktopWindow.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using desktop_window::Decision;
using desktop_window::Size;

void parsing() {
    const auto ok = desktop_window::parse("960x540");
    require(ok && ok->width == 960 && ok->height == 540, "plain WxH parses");
    require(desktop_window::parse("320x180") && desktop_window::parse("3840x2160"), "inclusive bounds");
    for (const char* bad : {"", "x", "960", "960x", "x540", "960X540", " 960x540", "960x540 ", "0960x540", "-960x540",
                            "960x540x1", "319x540", "960x179", "3841x2160", "3840x2161", "99999x540", "96ax540"}) {
        require(!desktop_window::parse(bad), bad);
    }
}

void decisions() {
    const Size output{1944, 2160}, client{1944, 2160};
    require(desktop_window::decide({960, 1066}, client, output, false, true, false) == Decision::apply, "shrinks a plain window");
    require(desktop_window::decide({960, 1066}, {960, 1066}, output, false, true, false) == Decision::already, "no-op when already that size");
    require(desktop_window::decide({960, 1066}, client, output, true, true, false) == Decision::fullscreen, "never touches fullscreen");
    require(desktop_window::decide({960, 1066}, client, output, false, true, true) == Decision::maximized, "never touches a maximized window");
    require(desktop_window::decide({2560, 1440}, client, output, false, true, false) == Decision::larger_than_output, "never wider than the output");
    require(desktop_window::decide({960, 2200}, client, {3840, 2160}, false, true, false) == Decision::larger_than_output, "never taller than the output");
    require(desktop_window::decide({960, 1066}, client, output, false, false, false) == Decision::borderless, "never touches a borderless window");
    require(std::string_view{desktop_window::name(Decision::borderless)} == "refused_borderless", "names");
}
}

int main() {
    try {
        parsing();
        decisions();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "desktop_window tests passed\n";
    return 0;
}
