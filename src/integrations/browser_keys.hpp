#pragma once
#include <string>
namespace openxmb::integrations::detail {
inline unsigned browser_virtual_key(int scan) {
  if(scan>=4&&scan<=29) return 0x41+scan-4;
  if(scan>=30&&scan<=38) return 0x31+scan-30;
  if(scan>=58&&scan<=69) return 0x70+scan-58;
  if(scan>=104&&scan<=115) return 0x7c+scan-104;
  if(scan>=89&&scan<=97) return 0x61+scan-89;
  switch(scan) {
  case 39:return 0x30;case 40:return 0x0d;case 41:return 0x1b;case 42:return 0x08;case 43:return 0x09;case 44:return 0x20;
  case 73:return 0x2d;case 74:return 0x24;case 75:return 0x21;case 76:return 0x2e;case 77:return 0x23;case 78:return 0x22;
  case 79:return 0x27;case 80:return 0x25;case 81:return 0x28;case 82:return 0x26;
  case 84:return 0x6f;case 85:return 0x6a;case 86:return 0x6d;case 87:return 0x6b;case 88:return 0x0d;case 98:return 0x60;case 99:return 0x6e;
  default:return 0;
  }
}
// SDL scancodes use USB HID positions, as do DOM KeyboardEvent.code names.
inline std::string browser_key_code(int scan) {
  if(scan>=4&&scan<=29) return "Key"+std::string(1,static_cast<char>('A'+scan-4));
  if(scan>=30&&scan<=38) return "Digit"+std::to_string(scan-29);
  if(scan>=58&&scan<=69) return "F"+std::to_string(scan-57);
  if(scan>=104&&scan<=115) return "F"+std::to_string(scan-91);
  if(scan>=89&&scan<=97) return "Numpad"+std::to_string(scan-88);
  switch(scan) {
  case 39:return "Digit0";case 40:return "Enter";case 41:return "Escape";
  case 42:return "Backspace";case 43:return "Tab";case 44:return "Space";
  case 45:return "Minus";case 46:return "Equal";case 47:return "BracketLeft";
  case 48:return "BracketRight";case 49:return "Backslash";case 51:return "Semicolon";
  case 52:return "Quote";case 53:return "Backquote";case 54:return "Comma";
  case 55:return "Period";case 56:return "Slash";case 57:return "CapsLock";
  case 73:return "Insert";case 74:return "Home";case 75:return "PageUp";
  case 76:return "Delete";case 77:return "End";case 78:return "PageDown";
  case 79:return "ArrowRight";case 80:return "ArrowLeft";case 81:return "ArrowDown";case 82:return "ArrowUp";
  case 83:return "NumLock";case 84:return "NumpadDivide";case 85:return "NumpadMultiply";
  case 86:return "NumpadSubtract";case 87:return "NumpadAdd";case 88:return "NumpadEnter";
  case 98:return "Numpad0";case 99:return "NumpadDecimal";case 100:return "IntlBackslash";
  case 101:return "ContextMenu";case 103:return "NumpadEqual";
  case 224:return "ControlLeft";case 225:return "ShiftLeft";case 226:return "AltLeft";case 227:return "MetaLeft";
  case 228:return "ControlRight";case 229:return "ShiftRight";case 230:return "AltRight";case 231:return "MetaRight";
  default:return {};
  }
}
}
