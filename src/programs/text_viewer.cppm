/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
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

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <span>
#include <utility>

#if __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

export module openxmb.app:text_viewer;

import dreamrender;
import glm;
import spdlog;
import vulkan_hpp;
import openxmb.utils;
import :component;
import :programs;

namespace programs {

using namespace app;

#if __linux__
class mapped_memory {
    public:
        mapped_memory(const std::filesystem::path& path) {
            m_fd = open(path.c_str(), O_RDONLY);
            if(m_fd < 0) {
                spdlog::error("Failed to open file {}: {}", path.string(), strerror(errno));
                throw std::runtime_error("Failed to open file");
            }
            m_size = lseek(m_fd, 0, SEEK_END);
            if(m_size < 0) {
                spdlog::error("Failed to get size of file {}: {}", path.string(), strerror(errno));
                close(m_fd);
                throw std::runtime_error("Failed to get file size");
            }
            m_data = mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
            if(m_data == MAP_FAILED) {
                spdlog::error("Failed to map file {}: {}", path.string(), strerror(errno));
                close(m_fd);
                throw std::runtime_error("Failed to map file");
            }
        }
        mapped_memory(const mapped_memory&) = delete;
        mapped_memory(mapped_memory&& other) noexcept :
            m_fd(std::exchange(other.m_fd, -1)), m_size(std::exchange(other.m_size, 0)), m_data(std::exchange(other.m_data, nullptr)) {}
        ~mapped_memory() {
            if(m_data != nullptr) {
                munmap(m_data, m_size);
            }
            if(m_fd >= 0) {
                close(m_fd);
            }
        }

        mapped_memory& operator=(const mapped_memory&) = delete;
        mapped_memory& operator=(mapped_memory&& other) noexcept {
            if(this != &other) {
                m_fd = std::exchange(other.m_fd, -1);
                m_size = std::exchange(other.m_size, 0);
                m_data = std::exchange(other.m_data, nullptr);
            }
            return *this;
        }

        template<typename T>
        const T* data() {
            return static_cast<T*>(m_data);
        }

        std::size_t size() const {
            return m_size;
        }

        template<typename T>
        operator std::span<const T>() const {
            return std::span<const T>(static_cast<const T*>(m_data), m_size / sizeof(T));
        }
    private:
        int m_fd = -1;
        std::size_t m_size = 0;
        void* m_data = nullptr;
};
#else
class mapped_memory {
    public:
        mapped_memory(const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if(!file.is_open()) {
                spdlog::error("Failed to open file {}: {}", path.string(), strerror(errno));
                throw std::runtime_error("Failed to open file");
            }
            std::streamsize size = -1;
            if(size < 0) {
                spdlog::error("Failed to get size of file {}: {}", path.string(), strerror(errno));
                throw std::runtime_error("Failed to get file size");
            }
            file.seekg(0, std::ios::beg);

            m_data.resize(static_cast<std::size_t>(size));
            if(!file.read(m_data.data(), size)) {
                spdlog::error("Failed to read file {}: {}", path.string(), strerror(errno));
                throw std::runtime_error("Failed to read file");
            }
        }
        mapped_memory(const mapped_memory&) = delete;
        mapped_memory(mapped_memory&& other) noexcept : m_data(std::move(other.m_data)) {
        }
        ~mapped_memory() = default;

        mapped_memory& operator=(const mapped_memory&) = delete;
        mapped_memory& operator=(mapped_memory&& other) noexcept {
            if(this != &other) {
                m_data = std::move(other.m_data);
            }
            return *this;
        }

        template<typename T>
        const T* data() const {
            return reinterpret_cast<const T*>(m_data.data());
        }

        std::size_t size() const {
            return m_data.size();
        }

        template<typename T>
        operator std::span<const T>() const {
            return std::span<const T>(reinterpret_cast<const T*>(m_data.data()), m_data.size() / sizeof(T));
        }
    private:
        std::string m_data;
};
#endif

struct hyperlink {
    std::size_t pos{};
    std::size_t len{};
    std::size_t line{};
    std::size_t col{};
    std::string_view dest{};
    mutable glm::vec2 screen_pos{};
    mutable glm::vec2 screen_size{};
    bool hovered = false;
};

namespace {
    std::string shell_quote(std::string_view value)
    {
        std::string result{"'"};
        for(char c : value) {
            if(c == '\'') {
                result += "'\\''";
            } else {
                result += c;
            }
        }
        result += "'";
        return result;
    }

    void open_uri(std::string_view uri)
    {
#if defined(__APPLE__)
        std::string command = "open " + shell_quote(uri);
#elif defined(_WIN32)
        std::string command = "start \"\" \"" + std::string(uri) + "\"";
#else
        std::string command = "xdg-open " + shell_quote(uri) + " >/dev/null 2>&1 &";
#endif
        if(std::system(command.c_str()) != 0) {
            spdlog::warn("Failed to open URI: {}", uri);
        }
    }
}

export class text_viewer : public component, public action_receiver, public event_receiver {
    public:
        text_viewer(const std::filesystem::path& path, dreamrender::resource_loader& loader) : title(path.string()), src(mapped_memory(path)) {
            text = std::string_view{std::span<const char>{std::get<mapped_memory>(src)}};
            lines = std::ranges::count(text, '\n') + 1;
            update();
        }
        text_viewer(std::string title, std::string data) : title(std::move(title)), src(std::move(data)) {
            text = std::get<std::string>(src);
            lines = std::ranges::count(text, '\n') + 1;
            update();
        }
        text_viewer(std::string title, std::string_view data) : title(std::move(title)), src(data) {
            text = std::get<std::string_view>(src);
            lines = std::ranges::count(text, '\n') + 1;
            update();
        }

        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override {
            constexpr float x = (1.0f - width) / 2;
            constexpr float y = (1.0f - height) / 2;
            const double offset_x = 0.01;
            const double offset_y = 0.01 * renderer.aspect_ratio;

            render_controller_buttons(xmb, renderer, 0.5f, 0.95f, std::array{
                std::pair{action::ok, std::string_view{"Open Link"}},
                std::pair{action::up, std::string_view{"Scroll Up"}},
                std::pair{action::down, std::string_view{"Scroll Down"}},
                std::pair{action::cancel, std::string_view{"Close"}}
            });

            renderer.draw_rect(glm::vec2{x - offset_x, y - offset_y}, glm::vec2{width + 2*offset_x, height + 2*offset_y},
                glm::vec4{0.1f, 0.1f, 0.1f, 0.5f});

            if(!title.empty()) {
                renderer.draw_text(title, x, y - font_size, 1.25*font_size);
            }
            renderer.set_clip(x, y, width, height);
            if(!text.empty()) {
                std::string_view part = text.substr(begin_offset, end_offset - begin_offset);
                renderer.draw_text(part, x, y, font_size);
                for(const auto& h : hyperlinks) {
                    const float hy = y + (h.line + 1) * font_size/2.0f;
                    const float hx = x + renderer.measure_text(part.substr(h.pos - h.col, h.col), font_size).x;
                    const float hl = renderer.measure_text(h.dest, font_size).x;
                    const float underline_height = (h.hovered ? 3.0f : 1.0f) / static_cast<float>(renderer.frame_size.height);
                    renderer.draw_rect(glm::vec2{hx, hy}, glm::vec2{hl, underline_height},
                        h.hovered ? glm::vec4{0.75f, 0.9f, 1.0f, 1.0f} : glm::vec4{1.0f, 1.0f, 1.0f, 0.8f});
                    h.screen_pos = glm::vec2{hx, hy - font_size/2.0f};
                    h.screen_size = glm::vec2{hl, font_size/2.0f};
                }
            }
            renderer.reset_clip();
        }

        result tick(shell*) override {
            if(line_movement != 0) {
                const int max_line = std::max(0, static_cast<int>(lines) - rendered_lines);
                current_line = static_cast<unsigned int>(
                    std::clamp(static_cast<int>(current_line) + line_movement, 0, max_line)
                );
                update();
            }
            return result::success;
        }

        result on_action(action action) override {
            if(action == action::cancel) {
                return result::close;
            } else if(action == action::up) {
                if(current_line > 0) {
                    current_line -= 1;
                    update();
                }
                return result::success;
            } else if(action == action::down) {
                if(current_line + rendered_lines < lines) {
                    current_line += 1;
                    update();
                }
                return result::success;
            }
            return result::failure;
        };

        result on_event(const event& event) override {
            if(auto* d = event.get<events::joystick_axis>()) {
                if(d->index == events::logical_joystick_index::left) {
                    constexpr float scroll_threshold = 10000.0f / 32767.0f;
                    if(std::abs(d->y) > scroll_threshold) {
                        line_movement = static_cast<int>(d->y * 2.0f);
                    } else {
                        line_movement = 0;
                    }
                    return result::success;
                }
            } else if(auto* d = event.get<events::cursor_move>()) {
                for(auto& h : hyperlinks) {
                    h.hovered = d->x >= h.screen_pos.x && d->x <= h.screen_pos.x + h.screen_size.x &&
                                d->y >= h.screen_pos.y && d->y <= h.screen_pos.y + h.screen_size.y;
                }
                return result::success;
            } else if(event.action == action::ok ||
                event.test<events::mouse_button_up>(
                    [](const events::mouse_button_up& d){ return d.button == events::logical_mouse_button::left; }
                ))
            {
                for(const auto& h : hyperlinks) {
                    if(h.hovered) {
                        spdlog::debug("Opening hyperlink: {}", h.dest);
                        open_uri(h.dest);
                        break;
                    }
                }
                return result::success;
            }
            return on_action(event.action);
        }

        [[nodiscard]] bool enable_cursor() const override {
            return true;
        }
    private:
        static constexpr float width = 0.6f;
        static constexpr float height = 0.6f;
        static constexpr float font_size = 0.05f;
        static constexpr int rendered_lines = 2*height / font_size;

        std::string title;
        std::variant<mapped_memory, std::string, std::string_view> src;
        std::string_view text;
        unsigned int current_line = 0;
        unsigned int lines = 0;
        int line_movement = 0;
        std::vector<hyperlink> hyperlinks;

        unsigned int begin_offset = 0;
        unsigned int end_offset = 0;

        void update() {
            calculate_offsets();
            find_hyperlinks();
        }

        void calculate_offsets() {
            begin_offset = 0;
            end_offset = 0;

            unsigned int l = 0;
            for(unsigned int i=0; i<text.size(); ++i) {
                if(text[i] == '\n') {
                    l += 1;
                    if(l == current_line) {
                        begin_offset = i+1;
                    }
                    if(l == current_line + rendered_lines) {
                        end_offset = i;
                        break;
                    }
                }
            }
            if(end_offset <= begin_offset) {
                end_offset = text.size();
            }
        }

        void find_hyperlinks() {
            hyperlinks.clear();
            if(text.empty()) {
                return;
            }

            std::string_view part = text.substr(begin_offset, end_offset - begin_offset);
            std::size_t line = 0;
            std::size_t col = 0;
            constexpr std::size_t min_link_len = std::char_traits<char>::length("http://");
            for(std::size_t i = 0; i + min_link_len <= part.size(); ++i) {
                char c = part[i];
                if(c == '\n') {
                    line++;
                    col = 0;
                    continue;
                }

                std::string_view atpos = part.substr(i);
                if(atpos.starts_with("http://") || atpos.starts_with("https://")) {
                    auto& h = hyperlinks.emplace_back();
                    h.pos = i;
                    h.line = line;
                    h.col = col;

                    auto end = std::ranges::find_if_not(atpos, [](char c){
                        return std::isalnum(static_cast<unsigned char>(c)) || std::string_view{"-._~:/?'[]@!$&'*+,;%="}.contains(c);
                    });
                    h.len = static_cast<std::size_t>(end - atpos.begin());
                    h.dest = atpos.substr(0, h.len);
                    spdlog::trace("Found hyperlink: \"{}\" beginning at position {} in line {} and column {}", h.dest, h.pos, h.line, h.col);
                }

                col++;
            }
        }
};

namespace {
const inline register_program<text_viewer> text_viewer_program{
    "text_viewer",
    {
        "text/plain", "application/json", "text/x-shellscript", "text/x-c++"
    },
    {
        ".txt", ".md", ".json", ".sh", ".cpp", ".h"
    }
};
}

}
