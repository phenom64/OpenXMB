#pragma once
#include "openxmb/integrations/desktop.hpp"
#include <memory>

namespace openxmb::integrations {
// UI-thread-owned passive shortcut listener. It reports input only and never
// requests focus. Windows retains its shell RegisterHotKey implementation.
class SessionHotkey {
public:
  SessionHotkey();
  ~SessionHotkey();
  SessionHotkey(const SessionHotkey&)=delete;
  SessionHotkey& operator=(const SessionHotkey&)=delete;
  // Ctrl+Shift+F1..F4; lock modifiers are accepted. Reconfiguration releases
  // the prior shortcut. Conflicts/unsupported session policies fail closed.
  [[nodiscard]] Result configure(unsigned function_key_1_to_4);
  void stop() noexcept;
  // Consumes at most 256 native events per call. Held/autorepeat presses do
  // not repeat; genuine rapid press/release pairs retain their separate edges.
  [[nodiscard]] unsigned take_presses();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
