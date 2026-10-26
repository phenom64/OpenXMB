/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

module;
#include "openxmb/integrations/desktop.hpp"

export module openxmb.modules.browser;

export namespace modules::browser {
  using Session = openxmb::integrations::BrowserSession;
  inline openxmb::integrations::Result open(std::string_view url, Session& session) {
    return openxmb::integrations::open_browser(url, session);
  }
  inline openxmb::integrations::Result close(Session& session) {
    return openxmb::integrations::close_browser(session);
  }
  inline bool init() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return openxmb::integrations::find_executable("chromium").has_value() ||
      openxmb::integrations::find_executable("firefox").has_value() ||
      openxmb::integrations::find_executable("xdg-open").has_value();
#endif
  }
}
