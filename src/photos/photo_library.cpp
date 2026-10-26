#include <openxmb/photos/photo_library.hpp>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace openxmb::photos {
namespace {
constexpr std::uintmax_t maximum_file_bytes = 128ULL * 1024 * 1024;
constexpr std::uint64_t maximum_pixels = 32ULL * 1024 * 1024;
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)>;
std::atomic_uint64_t sequence{};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}
std::filesystem::path reserve_directory(const std::filesystem::path& root,
                                       std::string_view prefix) {
    std::filesystem::create_directories(root);
    for(int attempt = 0; attempt < 100; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = root / (std::string(prefix) + std::to_string(stamp) + "-" +
                            std::to_string(sequence.fetch_add(1)));
        std::error_code ec;
        if(std::filesystem::create_directory(path, ec)) {
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            return path;
        }
        if(ec && ec != std::errc::file_exists) {
            throw std::filesystem::filesystem_error("Cannot create working directory", path, ec);
        }
    }
    throw std::runtime_error("Cannot reserve a working directory");
}
struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void validate_source(const std::filesystem::path& path) {
    const auto status = std::filesystem::symlink_status(path);
    if(!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("Select a regular image file");
    }
    if(std::filesystem::file_size(path) > maximum_file_bytes) {
        throw std::runtime_error("Image file exceeds the 128 MiB decoding limit");
    }
}
Surface surface(const std::filesystem::path& path) {
    validate_source(path);
    Surface loaded{IMG_Load(path.string().c_str()), SDL_FreeSurface};
    if(!loaded) throw std::runtime_error(std::string("Cannot decode image: ") + IMG_GetError());
    if(loaded->w <= 0 || loaded->h <= 0 ||
       static_cast<std::uint64_t>(loaded->w) * loaded->h > maximum_pixels) {
        throw std::runtime_error("Image exceeds the 32 megapixel decoding limit");
    }
    Surface converted{SDL_ConvertSurfaceFormat(loaded.get(), SDL_PIXELFORMAT_RGBA32, 0), SDL_FreeSurface};
    if(!converted) throw std::runtime_error(SDL_GetError());
    return converted;
}

// JPEG EXIF is optional. Parse the bounded TIFF IFD for orientation, camera and
// capture date without invoking a process or retaining arbitrary image metadata.
void exif(Metadata& result) {
    std::ifstream input(result.path, std::ios::binary);
    std::array<unsigned char, 2> magic{};
    input.read(reinterpret_cast<char*>(magic.data()), 2);
    if(magic != std::array<unsigned char, 2>{0xff, 0xd8}) return;
    for(int segment = 0; segment < 100 && input; ++segment) {
        int marker = input.get();
        if(marker != 0xff) return;
        do { marker = input.get(); } while(marker == 0xff);
        if(marker == 0xda || marker == 0xd9 || marker < 0) return;
        const auto hi = input.get(), lo = input.get();
        if(hi < 0 || lo < 0) return;
        const int length = (hi << 8) | lo;
        if(length < 2) return;
        std::vector<unsigned char> data(static_cast<std::size_t>(length - 2));
        if(!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) return;
        if(marker != 0xe1 || data.size() < 14 || std::memcmp(data.data(), "Exif\0\0", 6) != 0) continue;
        const std::size_t base = 6;
        const bool little = data[6] == 'I' && data[7] == 'I';
        if(!little && !(data[6] == 'M' && data[7] == 'M')) return;
        auto u16 = [&](std::size_t at) -> std::uint32_t {
            if(at + 2 > data.size()) throw std::out_of_range("EXIF");
            return little ? data[at] | data[at+1] << 8 : data[at] << 8 | data[at+1];
        };
        auto u32 = [&](std::size_t at) -> std::uint32_t {
            if(at + 4 > data.size()) throw std::out_of_range("EXIF");
            std::uint32_t value = 0;
            for(int n = 0; n < 4; ++n) value |= static_cast<std::uint32_t>(data[at+n]) << (little ? n*8 : (3-n)*8);
            return value;
        };
        auto read_ifd = [&](std::size_t offset, bool capture, auto&& self) -> void {
            if(offset > data.size() - base) return;
            const auto begin = base + offset;
            const auto count = std::min<std::uint32_t>(u16(begin), 256);
            for(std::uint32_t i=0; i<count; ++i) {
                const auto at = begin + 2 + i*12;
                if(at + 12 > data.size()) break;
                const auto tag = u16(at), type = u16(at+2), size = u32(at+4);
                if(tag == 0x112 && type == 3 && size == 1) {
                    const auto value = u16(at+8);
                    if(value >= 1 && value <= 8) result.orientation = static_cast<int>(value);
                }
                if(tag == 0x8769 && !capture) self(u32(at+8), true, self);
                if(type != 2 || size == 0 || size > 256) continue;
                const auto start = size <= 4 ? at+8 : base + static_cast<std::size_t>(u32(at+8));
                if(start > data.size() || size > data.size() - start) continue;
                std::string value(reinterpret_cast<const char*>(data.data()+start), size);
                if(const auto nul = value.find('\0'); nul != std::string::npos) value.resize(nul);
                value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }), value.end());
                if(tag == 0x110) result.camera = value;
                if(tag == 0x132 && result.captured.empty()) result.captured = value;
                if(tag == 0x9003) result.captured = value;
            }
        };
        try { if(u16(base+2) == 42) read_ifd(u32(base+4), false, read_ifd); } catch(const std::out_of_range&) {}
        return;
    }
}
Bitmap oriented(Bitmap image, int orientation) {
    if(orientation == 3 || orientation == 4) image = rotate(std::move(image), 2);
    if(orientation == 5 || orientation == 6) image = rotate(std::move(image), 1);
    if(orientation == 7 || orientation == 8) image = rotate(std::move(image), 3);
    if(orientation == 2 || orientation == 4 || orientation == 5 || orientation == 7) {
        for(int y=0; y<image.height; ++y) for(int x=0; x<image.width/2; ++x) {
            const auto a = (static_cast<std::size_t>(y)*image.width+x)*4;
            const auto b = (static_cast<std::size_t>(y)*image.width+image.width-1-x)*4;
            for(int c=0; c<4; ++c) std::swap(image.rgba[a+c], image.rgba[b+c]);
        }
    }
    return image;
}
}

bool supported_image(const std::filesystem::path& path) {
    const auto extension = lower(path.extension().string());
    constexpr std::array extensions{".bmp", ".gif", ".jpg", ".jpeg", ".pcx", ".png", ".pbm", ".pgm", ".ppm", ".pnm", ".svg", ".tga", ".tiff", ".tif", ".webp", ".xcf", ".xpm"};
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}
std::vector<std::filesystem::path> gallery(const std::filesystem::path& selected) {
    std::vector<std::filesystem::path> images;
    std::error_code ec;
    auto directory = std::filesystem::absolute(selected).parent_path();
    for(std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if(it->is_regular_file(ec) && !it->is_symlink(ec) && supported_image(it->path()) && !it->path().filename().string().starts_with('.')) images.push_back(it->path());
    }
    std::sort(images.begin(), images.end(), [](const auto& a, const auto& b) { return lower(a.filename().string()) < lower(b.filename().string()); });
    auto absolute = std::filesystem::absolute(selected).lexically_normal();
    if(std::find(images.begin(), images.end(), absolute) == images.end()) images.push_back(absolute);
    return images;
}
static Metadata image_metadata(const std::filesystem::path& path,int width,int height) {
    Metadata info;
    info.path=std::filesystem::absolute(path); info.bytes=std::filesystem::file_size(path); info.width=width; info.height=height;
    std::error_code ec;
    auto modified = std::filesystem::last_write_time(path, ec);
    if(!ec) {
        auto seconds = std::chrono::system_clock::to_time_t(std::chrono::file_clock::to_sys(modified));
        std::tm value{};
#if defined(_WIN32)
        localtime_s(&value, &seconds);
#else
        localtime_r(&seconds, &value);
#endif
        std::ostringstream output; output << std::put_time(&value, "%Y-%m-%d %H:%M:%S"); info.modified = output.str();
    }
    exif(info);
    return info;
}
Metadata metadata(const std::filesystem::path& path) {
    auto image=surface(path);
    return image_metadata(path,image->w,image->h);
}
LoadedPhoto load_photo(const std::filesystem::path& path,int maximum_edge) {
    auto decoded = surface(path);
    auto info=image_metadata(path,decoded->w,decoded->h);
    if(maximum_edge > 0 && std::max(decoded->w, decoded->h) > maximum_edge) {
        const double factor = static_cast<double>(maximum_edge) / std::max(decoded->w, decoded->h);
        SDL_SetSurfaceBlendMode(decoded.get(), SDL_BLENDMODE_NONE);
        Surface resized{SDL_CreateRGBSurfaceWithFormat(0, std::max(1, static_cast<int>(std::lround(decoded->w*factor))), std::max(1, static_cast<int>(std::lround(decoded->h*factor))), 32, SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface};
        if(!resized || SDL_BlitScaled(decoded.get(), nullptr, resized.get(), nullptr) != 0) throw std::runtime_error(SDL_GetError());
        decoded = std::move(resized);
    }
    Bitmap result{decoded->w, decoded->h, std::vector<std::uint8_t>(static_cast<std::size_t>(decoded->w)*decoded->h*4)};
    if(SDL_LockSurface(decoded.get()) != 0) throw std::runtime_error(SDL_GetError());
    for(int y=0; y<decoded->h; ++y) std::memcpy(result.rgba.data()+static_cast<std::size_t>(y)*decoded->w*4, static_cast<unsigned char*>(decoded->pixels)+static_cast<std::size_t>(y)*decoded->pitch, static_cast<std::size_t>(decoded->w)*4);
    SDL_UnlockSurface(decoded.get());
    result=oriented(std::move(result),info.orientation);
    return {std::move(result),std::move(info)};
}
Bitmap load_bitmap(const std::filesystem::path& path,int maximum_edge) {
    return load_photo(path,maximum_edge).image;
}
Bitmap rotate(Bitmap image, int quarter_turns) {
    const int turns = (quarter_turns % 4 + 4) % 4;
    if(turns == 0) return image;
    Bitmap output{turns%2 ? image.height : image.width, turns%2 ? image.width : image.height, std::vector<std::uint8_t>(image.rgba.size())};
    for(int y=0; y<image.height; ++y) for(int x=0; x<image.width; ++x) {
        int dx{}, dy{};
        if(turns == 1) { dx=image.height-1-y; dy=x; }
        else if(turns == 2) { dx=image.width-1-x; dy=image.height-1-y; }
        else { dx=y; dy=image.width-1-x; }
        std::memcpy(output.rgba.data()+(static_cast<std::size_t>(dy)*output.width+dx)*4, image.rgba.data()+(static_cast<std::size_t>(y)*image.width+x)*4, 4);
    }
    return output;
}
Bitmap crop(const Bitmap& image, Crop region) {
    if(!std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.width) || !std::isfinite(region.height) || region.width <= 0 || region.height <= 0 || region.x < 0 || region.y < 0 || region.x + region.width > 1.000001 || region.y + region.height > 1.000001) throw std::invalid_argument("Crop must fit within the image");
    const int x=std::clamp(static_cast<int>(region.x*image.width), 0, image.width-1);
    const int y=std::clamp(static_cast<int>(region.y*image.height), 0, image.height-1);
    const int w=std::clamp(static_cast<int>(std::lround(region.width*image.width)), 1, image.width-x);
    const int h=std::clamp(static_cast<int>(std::lround(region.height*image.height)), 1, image.height-y);
    Bitmap output{w,h,std::vector<std::uint8_t>(static_cast<std::size_t>(w)*h*4)};
    for(int row=0; row<h; ++row) std::memcpy(output.rgba.data()+static_cast<std::size_t>(row)*w*4, image.rgba.data()+(static_cast<std::size_t>(row+y)*image.width+x)*4, static_cast<std::size_t>(w)*4);
    return output;
}
Crop centered_crop(int width, int height, double aspect_ratio) {
    if(width <= 0 || height <= 0 || !std::isfinite(aspect_ratio) || aspect_ratio <= 0) throw std::invalid_argument("Invalid crop aspect ratio");
    const double aspect=static_cast<double>(width)/height;
    Crop region;
    if(aspect > aspect_ratio) region.width=aspect_ratio/aspect;
    else region.height=aspect/aspect_ratio;
    region.x=(1-region.width)*0.5; region.y=(1-region.height)*0.5;
    return region;
}
std::filesystem::path save_copy(const std::filesystem::path& original, int quarter_turns, std::optional<Crop> region) {
    auto image=rotate(load_bitmap(original, 0), quarter_turns);
    if(region) image=crop(image,*region);
    const auto parent=std::filesystem::absolute(original).parent_path();
    Cleanup working{reserve_directory(parent, ".openxmb-edit-")};
    const auto temporary=working.path/"edited.png";
    Surface output{SDL_CreateRGBSurfaceWithFormatFrom(image.rgba.data(),image.width,image.height,32,image.width*4,SDL_PIXELFORMAT_RGBA32), SDL_FreeSurface};
    if(!output || IMG_SavePNG(output.get(),temporary.string().c_str()) != 0) throw std::runtime_error(std::string("Cannot save image: ")+IMG_GetError());
    for(int n=1; n<=10000; ++n) {
        auto destination=parent/(original.stem().string()+"-edited-"+std::to_string(n)+".png");
        std::error_code ec;
        if(std::filesystem::copy_file(temporary,destination,std::filesystem::copy_options::none,ec)) return destination;
        if(ec != std::errc::file_exists && ec) throw std::filesystem::filesystem_error("Cannot save copy",destination,ec);
    }
    throw std::runtime_error("Cannot reserve a unique image filename");
}
std::filesystem::path move_to_trash(const std::filesystem::path& original, const std::filesystem::path& requested_root) {
    validate_source(original);
    auto root=requested_root;
    if(root.empty()) {
        if(const char* data=std::getenv("XDG_DATA_HOME"); data && *data) root=std::filesystem::path(data)/"OpenXMB/Trash";
        else if(const char* home=std::getenv("HOME"); home && *home) root=std::filesystem::path(home)/".local/share/OpenXMB/Trash";
#if defined(_WIN32)
        else if(const char* appdata=std::getenv("LOCALAPPDATA"); appdata && *appdata) root=std::filesystem::path(appdata)/"OpenXMB/Trash";
#endif
        else throw std::runtime_error("No writable user data directory is configured");
    }
    auto reserved=reserve_directory(root,"photo-");
    auto destination=reserved/"files"/original.filename();
    try {
        std::filesystem::create_directory(reserved/"files");
        std::ofstream manifest(reserved/"original-path.txt",std::ios::binary);
        manifest << std::filesystem::absolute(original).string(); manifest.close();
        if(!manifest) throw std::runtime_error("Cannot record the original photo location");
        std::error_code ec;
        std::filesystem::rename(original,destination,ec);
        if(ec == std::errc::cross_device_link) {
            std::filesystem::copy_file(original,destination,std::filesystem::copy_options::none);
            std::filesystem::remove(original);
        } else if(ec) throw std::filesystem::filesystem_error("Cannot move image to Trash",original,ec);
        return destination;
    } catch(...) { std::error_code ec; if(!std::filesystem::exists(destination)) std::filesystem::remove_all(reserved,ec); throw; }
}
std::string describe(const Metadata& info) {
    std::ostringstream text;
    text << info.path.filename().string() << "\n" << info.width << " × " << info.height << " pixels\n" << std::fixed << std::setprecision(1) << static_cast<double>(info.bytes)/1024.0 << " KiB\nModified: " << info.modified;
    if(!info.camera.empty()) text << "\nCamera: " << info.camera;
    if(!info.captured.empty()) text << "\nCaptured: " << info.captured;
    text << "\n" << info.path.string();
    return text.str();
}
}
