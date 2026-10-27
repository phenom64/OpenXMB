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

#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace openxmb::documents {
enum class Kind { pdf, comic_archive };
struct ArchivePage { std::string name; std::size_t entry{}; std::uintmax_t bytes{}; };
struct Workspace;
struct Document {
    std::filesystem::path path;
    Kind kind{Kind::pdf};
    std::size_t pages{};
    std::vector<ArchivePage> archive_pages;
    std::shared_ptr<Workspace> workspace;
};
std::shared_ptr<Document> open(const std::filesystem::path& path, std::stop_token stop = {});
std::filesystem::path render_page(const Document& document, std::size_t page,
                                  std::stop_token stop = {});
std::string page_label(const Document& document, std::size_t page);
void print(const Document& document, std::optional<std::size_t> page = std::nullopt,
           std::stop_token stop = {});
bool printing_available();
std::string information(const Document& document);
}
