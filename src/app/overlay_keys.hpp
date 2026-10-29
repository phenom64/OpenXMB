#pragma once
#include <SDL2/SDL.h>

namespace app {
inline SDL_Keycode x11_overlay_key(unsigned symbol) {
    if(symbol>='A'&&symbol<='Z')return static_cast<SDL_Keycode>(symbol-'A'+'a');
    if(symbol>=0x20&&symbol<=0x7e)return static_cast<SDL_Keycode>(symbol);
    if(symbol>=0xffbe&&symbol<=0xffc9)return SDLK_F1+static_cast<int>(symbol-0xffbe);
    switch(symbol) {
    case 0xff51:return SDLK_LEFT;case 0xff53:return SDLK_RIGHT;
    case 0xff52:return SDLK_UP;case 0xff54:return SDLK_DOWN;
    case 0xff0d:return SDLK_RETURN;case 0xff8d:return SDLK_KP_ENTER;
    case 0xff1b:return SDLK_ESCAPE;case 0xff09:return SDLK_TAB;
    case 0xff08:return SDLK_BACKSPACE;case 0xffff:return SDLK_DELETE;
    case 0xff63:return SDLK_INSERT;case 0xff50:return SDLK_HOME;
    case 0xff57:return SDLK_END;case 0xff55:return SDLK_PAGEUP;
    case 0xff56:return SDLK_PAGEDOWN;case 0xffe5:return SDLK_CAPSLOCK;
    case 0xffe1:return SDLK_LSHIFT;case 0xffe2:return SDLK_RSHIFT;
    case 0xffe3:return SDLK_LCTRL;case 0xffe4:return SDLK_RCTRL;
    case 0xffe9:return SDLK_LALT;case 0xffea:return SDLK_RALT;
    default:return SDLK_UNKNOWN;
    }
}
inline SDL_Keymod x11_overlay_modifiers(unsigned modifiers) {
    unsigned result=KMOD_NONE;
    if(modifiers&1)result|=KMOD_SHIFT;
    if(modifiers&2)result|=KMOD_CAPS;
    if(modifiers&4)result|=KMOD_CTRL;
    if(modifiers&8)result|=KMOD_ALT;
    return static_cast<SDL_Keymod>(result);
}
}
