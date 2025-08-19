module;

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <variant>
#include <span>
#include <utility>

#if __linux__
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

export module xmbshell.app:text_viewer;

import dreamrender;
import glm;
import spdlog;
import vulkan_hpp;
import xmbshell.utils;
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

export class text_viewer : public component, public action_receiver, public joystick_receiver, public mouse_receiver {
    public:
        text_viewer(const std::filesystem::path& path, dreamrender::resource_loader& loader) : title(path.string()), src(mapped_memory(path)) {
            text = std::string_view{std::span<const char>{std::get<mapped_memory>(src)}};
            lines = std::ranges::count(text, '\n') + 1;
            calculate_offsets();
        }
        text_viewer(std::string title, std::string data) : title(std::move(title)), src(std::move(data)) {
            text = std::get<std::string>(src);
            lines = std::ranges::count(text, '\n') + 1;
            calculate_offsets();
        }
        text_viewer(std::string title, std::string_view data) : title(std::move(title)), src(data) {
            text = std::get<std::string_view>(src);
            lines = std::ranges::count(text, '\n') + 1;
            calculate_offsets();
        }

        void render(dreamrender::gui_renderer& renderer, class xmbshell* xmb) override {
            constexpr float x = (1.0f - width) / 2;
            constexpr float y = (1.0f - height) / 2;
            const double offset_x = 0.01;
            const double offset_y = 0.01 * renderer.aspect_ratio;

            renderer.draw_rect(glm::vec2{x - offset_x, y - offset_y}, glm::vec2{width + 2*offset_x, height + 2*offset_y},
                glm::vec4{0.1f, 0.1f, 0.1f, 0.5f});

            if(!title.empty()) {
                renderer.draw_text(title, x, y - font_size, 1.25*font_size);
            }
            renderer.set_clip(x, y, width, height);
            if(!text.empty()) {
                std::string_view part = text.substr(begin_offset, end_offset - begin_offset);
                renderer.draw_text(part, x, y, font_size);
            }
            renderer.reset_clip();
        }

        result on_action(action action) override {
            if(action == action::cancel) {
                return result::close;
            } else if(action == action::up) {
                if(current_line > 0) {
                    current_line -= 1;
                    calculate_offsets();
                }
                return result::success;
            } else if(action == action::down) {
                if(current_line + rendered_lines < lines) {
                    current_line += 1;
                    calculate_offsets();
                }
                return result::success;
            }
            return result::failure;
        };
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

        unsigned int begin_offset = 0;
        unsigned int end_offset = 0;

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
