#include "../../src/app/overlay_keys.hpp"
#include <iostream>
#include <stdexcept>
int main() try {
    const auto check=[](bool value){if(!value)throw std::runtime_error("X11 overlay key translation failed");};
    check(app::x11_overlay_key(0xff51)==SDLK_LEFT);
    check(app::x11_overlay_key(0xff53)==SDLK_RIGHT);
    check(app::x11_overlay_key(0xff52)==SDLK_UP);
    check(app::x11_overlay_key(0xff54)==SDLK_DOWN);
    check(app::x11_overlay_key(0xff0d)==SDLK_RETURN);
    check(app::x11_overlay_key(0xff8d)==SDLK_KP_ENTER);
    check(app::x11_overlay_key(0xff1b)==SDLK_ESCAPE);
    check(app::x11_overlay_key(0xffbe)==SDLK_F1&&app::x11_overlay_key(0xffc9)==SDLK_F12);
    check(app::x11_overlay_key('Q')==SDLK_q&&app::x11_overlay_key('q')==SDLK_q);
    check(app::x11_overlay_key(0xffffff)==SDLK_UNKNOWN);
    check((app::x11_overlay_modifiers(5)&(KMOD_SHIFT|KMOD_CTRL))==(KMOD_SHIFT|KMOD_CTRL));
    check((app::x11_overlay_modifiers(10)&(KMOD_CAPS|KMOD_ALT))==(KMOD_CAPS|KMOD_ALT));
    std::cout<<"X11 overlay controls, remappable key symbols and modifiers passed\n";
    return 0;
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
