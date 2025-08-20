module;

#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#ifdef __GNUG__
#include <cxxabi.h>
#endif

#if __linux__
#include <fcntl.h>
#include <sys/mman.h>
#endif

module shell.utils;

import spdlog;

namespace utils {
#if __linux__
    class GtkIconCache {
        public:
            GtkIconCache(const std::filesystem::path& path) : m_fd(-1), m_data(nullptr) {
                m_fd = open(path.c_str(), O_RDONLY);
                if (m_fd < 0) {
                    spdlog::error("Failed to open icon cache file: {}", path.string());
                    throw std::runtime_error("Failed to open icon cache file");
                }
                m_size = lseek(m_fd, 0, SEEK_END);
                if (m_size <= 0) {
                    spdlog::error("Failed to get size of icon cache file at {}: {}", path.string(), strerror(errno));
                    cleanup();
                    throw std::runtime_error("Failed to get size of icon cache file");
                }
                m_data = reinterpret_cast<const char*>(mmap(nullptr, m_size, PROT_READ, MAP_PRIVATE, m_fd, 0));
                if (m_data == MAP_FAILED) {
                    spdlog::error("Failed to mmap icon cache file at {}: {}", path.string(), strerror(errno));
                    cleanup();
                    throw std::runtime_error("Failed to mmap icon cache file");
                }

                uint16_t major_version = read_card16(0);
                uint16_t minor_version = read_card16(2);
                if(major_version != 1 || minor_version != 0) {
                    cleanup();
                    spdlog::error("invalid version {}.{}, we only support 1.0", major_version, minor_version);
                    throw std::runtime_error("Invalid version");
                }

                m_hash_offset = read_card32(4);
                m_directory_offset = read_card32(8);
                m_bucket_count = read_card32(m_hash_offset);
            }
            GtkIconCache(const GtkIconCache&) = delete;
            GtkIconCache(GtkIconCache&& other) noexcept :
                m_fd(std::exchange(other.m_fd, -1)), m_data(std::exchange(other.m_data, nullptr)),
                m_size(std::exchange(other.m_size, 0)), m_bucket_count(std::exchange(other.m_bucket_count, 0)),
                m_hash_offset(std::exchange(other.m_hash_offset, 0)), m_directory_offset(std::exchange(other.m_directory_offset, 0)) {}
            GtkIconCache& operator=(const GtkIconCache&) = delete;
            GtkIconCache& operator=(GtkIconCache&& other) noexcept {
                if(this != &other) {
                    cleanup();
                    m_fd = std::exchange(other.m_fd, -1);
                    m_data = std::exchange(other.m_data, nullptr);
                    m_size = std::exchange(other.m_size, 0);
                    m_bucket_count = std::exchange(other.m_bucket_count, 0);
                    m_hash_offset = std::exchange(other.m_hash_offset, 0);
                    m_directory_offset = std::exchange(other.m_directory_offset, 0);
                }
                return *this;
            }

            ~GtkIconCache() {
                cleanup();
            }

            std::optional<std::vector<std::string>> lookup(std::string_view name) const {
                auto hash = icon_hash(name);
                auto bucket = hash % m_bucket_count;

                for(uint32_t bucket_offset = read_card32(m_hash_offset + 4 + bucket*4); bucket_offset; bucket_offset = read_card32(bucket_offset)) {
                    uint32_t name_offset = read_card32(bucket_offset + 4);
                    if(!name_offset) {
                        break;
                    }
                    std::string_view key{m_data + name_offset};
                    if(name == key) {
                        auto list_offset = read_card32(bucket_offset + 8);
                        auto list_len = read_card32(list_offset);

                        std::vector<std::string> entries;
                        entries.reserve(list_len);
                        for(std::size_t i = 0; i < list_len; i++) {
                            auto index = read_card16(list_offset + 4 + i * 8);
                            auto flags = read_card16(list_offset + 4 + i * 8 + 2);
                            auto offset = read_card32(m_directory_offset + 4 + index * 4);

                            std::string_view dir{m_data + offset};
                            std::string str = std::string{dir} + "/" + std::string{name};
                            if(flags & 0x1) {
                                str += ".xpm";
                            } else if(flags & 0x4) {
                                str += ".png";
                            } else if(flags & 0x2) {
                                str += ".svg";
                            } else {
                                spdlog::warn("Unknown icon type for icon \"{}\" in cache", name);
                                continue;
                            }
                            entries.push_back(std::move(str));
                        }
                        return entries;
                    }
                }

                return std::nullopt;
            }
        private:
            void cleanup() {
                if (m_data) {
                    munmap(const_cast<void*>(static_cast<const void*>(m_data)), 0);
                }
                if (m_fd >= 0) {
                    close(m_fd);
                }
            }
            static unsigned int icon_hash(std::string_view key) {
                if(key.empty()) {
                    return 0;
                }
                unsigned int h = key[0];
                for(std::size_t i = 1; i<key.size(); i++) {
                    h = (h << 5) - h + key[i];
                }
                return h;
            }

            uint16_t read_card16(std::size_t offset) const {
                if (offset + sizeof(uint16_t) > m_size) {
                    throw std::out_of_range("Offset out of bounds");
                }
                return static_cast<uint16_t>(static_cast<uint8_t>(m_data[offset])) << 8 |
                       static_cast<uint16_t>(static_cast<uint8_t>(m_data[offset + 1]));
            }
            uint32_t read_card32(std::size_t offset) const {
                if (offset + sizeof(uint32_t) > m_size) {
                    throw std::out_of_range("Offset out of bounds");
                }
                return static_cast<uint32_t>(static_cast<uint8_t>(m_data[offset])) << 24 |
                       static_cast<uint32_t>(static_cast<uint8_t>(m_data[offset + 1])) << 16 |
                       static_cast<uint32_t>(static_cast<uint8_t>(m_data[offset + 2])) << 8 |
                       static_cast<uint32_t>(static_cast<uint8_t>(m_data[offset + 3]));
            }
            int m_fd;
            std::size_t m_size;
            const char* m_data;

            std::size_t m_hash_offset;
            std::size_t m_bucket_count;
            std::size_t m_directory_offset;
    };
    inline std::vector<std::tuple<std::filesystem::path, GtkIconCache>> gtkIconCaches;
#endif
    inline std::once_flag iconInitFlag;

    static void initialize_icons(){
#if __linux__
        auto process = [](const std::filesystem::path& path) {
            try {
                auto cache = path / "icon-theme.cache";
                if(!std::filesystem::exists(cache)) {
                    spdlog::debug("Icon theme cache not found at: {}", cache.string());
                    return;
                }
                gtkIconCaches.emplace_back(path, GtkIconCache(cache));
            } catch(const std::exception& e) {
                spdlog::error("Error while processing themed icons in path: {}", e.what());
            }
        };
        auto process_dir = [&](const std::string& dir) {
            std::filesystem::path path = std::filesystem::path(dir) / "icons";
            if(!std::filesystem::exists(path)) {
                spdlog::debug("Directory does not exist: {}", path.string());
                return;
            }
            try {
                for(const auto& entry : std::filesystem::directory_iterator(path)) {
                    if(entry.is_directory()) {
                        process(entry.path());
                    }
                }
            } catch(const std::exception& e) {
                spdlog::error("Error while processing directory {}: {}", dir, e.what());
            }
        };
        process_dir("/usr/share");
        process_dir("/usr/local/share");
        process_dir(Glib::get_home_dir()+"/.local/share");

        auto xdg_data_dirs = std::getenv("XDG_DATA_DIRS");
        if(xdg_data_dirs) {
            std::istringstream iss(xdg_data_dirs);
            std::string dir;
            while(std::getline(iss, dir, ":")) {
                process_dir(dir);
            }
        }
        spdlog::debug("Found {} themed icon caches", gtkIconCaches.size());
#endif
    };

    std::optional<std::filesystem::path> resolve_icon(const Gio::Icon* icon) {
        std::call_once(iconInitFlag, initialize_icons);

        if(auto* themed_icon = dynamic_cast<const Gio::ThemedIcon*>(icon)) {
            std::string name = *themed_icon->get_names().begin();
#if __linux__
            for(const auto& [p, c] : gtkIconCaches) {
                try {
                    if(auto r = c.lookup(name)) {
                        return p / r->front();
                    }
                } catch(const std::exception& e) {
                    spdlog::error("Error while looking up themed icon \"{}\" in cache {}: {}", name, p.string(), e.what());
                }
            }
#endif
            spdlog::warn("Themed icon \"{}\" not found", name);
        } else if(auto* file_icon = dynamic_cast<const Gio::FileIcon*>(icon)) {
            return file_icon->get_file()->get_path();
        } else {
            if(icon) {
                auto& r = *icon;
                spdlog::warn("Unsupported icon type: {}", typeid(r).name());
            }
        }
        return std::nullopt;
    }
}

namespace utils
{
    std::string to_fixed_string(double d, int n)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(n) << d;
        return oss.str();
    }
}

#ifdef __GNUG__
namespace utils
{
    std::string demangle(const char *name) {
        int status = -4;
        std::unique_ptr<char, void(*)(void*)> res{
            abi::__cxa_demangle(name, nullptr, nullptr, &status),
            std::free
        };
        return (status==0) ? res.get() : name;
    }
}
#else
namespace utils
{
    std::string demangle(const char *name) {
        return std::string(name);
    }
}
#endif