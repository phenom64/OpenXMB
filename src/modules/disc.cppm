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
#include "openxmb/integrations/disc.hpp"

export module openxmb.modules.disc;

export namespace modules::disc {
  inline auto scan() { return openxmb::integrations::discover_discs(); }
  inline auto probe(const std::filesystem::path& path) { return openxmb::integrations::probe_disc(path); }
  inline auto play(const openxmb::integrations::DiscInfo& disc) { return openxmb::integrations::play_disc(disc); }
  inline auto init() { return scan(); }
}
