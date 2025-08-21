module;

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <glm/glm.hpp>

export module openxmb.utils;

export enum class result {
    unsupported  = (1<<0),
    success      = (1<<1),
    failure      = (1<<2),

    submenu	     = (1<<3),
    close        = (1<<4),

    ok_sound     = (1<<5),
    error_rumble = (1<<6),
};
export inline result operator|(result a, result b) {
    return static_cast<result>(static_cast<int>(a) | static_cast<int>(b));
}
export inline bool operator&(result a, result b) {
    return static_cast<bool>(static_cast<int>(a) & static_cast<int>(b));
}

export enum class action {
    none = 0,
    left = 1,
    right = 2,
    up = 3,
    down = 4,
    ok = 5,
    cancel = 6,
    options = 7,
    extra = 8,

    _length,
};
export class action_receiver {
    public:
        virtual ~action_receiver() = default;
        virtual result on_action(action action) {
            return result::unsupported;
        }
};
export class joystick_receiver {
    public:
        virtual ~joystick_receiver() = default;
        virtual result on_joystick(unsigned int index, float x, float y) {
            return result::unsupported;
        }
};
export class mouse_receiver {
    public:
        virtual ~mouse_receiver() = default;
        virtual result on_mouse_move(float x, float y) {
            return result::unsupported;
        }
        virtual result on_mouse_scroll(float x) {
            return result::unsupported;
        }
};

export namespace utils
{
    // JSON-based icon resolution instead of Gio::Icon
    std::optional<std::filesystem::path> resolve_icon_from_json(const std::string& icon_name);

    template<typename R>
    bool is_ready(std::future<R> const& f)
    { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }

    template<typename R>
    bool is_ready(std::shared_future<R> const& f)
    { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }

    std::string to_fixed_string(double d, int n);

    template<int n, typename T>
    std::string to_fixed_string(T d)
    {
        return to_fixed_string(d, n);
    }

    using time_point = std::chrono::time_point<std::chrono::system_clock>;
    constexpr double progress(time_point now, time_point start, std::chrono::duration<double> duration) {
        double d = std::chrono::duration<double>(now - start).count() / duration.count();
        return std::clamp(d, 0.0, 1.0);
    }

    template<typename T>
    class aligned_wrapper {
        public:
            aligned_wrapper(void* data, std::size_t alignment) :
                data(static_cast<std::byte*>(data)), alignment(alignment) {}

            T& operator[](std::size_t i) {
                return *reinterpret_cast<T*>(data + i * aligned(sizeof(T), alignment)); // NOLINT
            }

            const T& operator[](std::size_t i) const {
                return *reinterpret_cast<const T*>(data + i * aligned(sizeof(T), alignment)); // NOLINT
            }

            T* operator+(std::size_t i) {
                return reinterpret_cast<T*>(data + i * aligned(sizeof(T), alignment)); // NOLINT
            }

            std::size_t offset(std::size_t i) const {
                return i * aligned(sizeof(T), alignment);
            }
        private:
            constexpr static std::size_t aligned(std::size_t size, std::size_t alignment) {
                return (size + alignment - 1) & ~(alignment - 1);
            }

            std::byte* data;
            std::size_t alignment;
    };

    std::string demangle(const char* name);

    template <class T>
    std::string type_name(const T& t) {
        return demangle(typeid(t).name());
    }
}

namespace utils {
    template<typename T>
    constexpr std::string_view get_typename() {
        std::string_view name = std::source_location::current().function_name();
        name.remove_prefix(std::string_view::traits_type::length("std::string_view utils::get_typename() "));
        name.remove_prefix(std::string_view::traits_type::length("[T = "));
        name.remove_suffix(std::string_view::traits_type::length("]"));
        return name;
    }

    // based on https://stackoverflow.com/questions/28828957/how-to-convert-an-enum-to-a-string-in-modern-c#comment115999268_55312360
    template<auto... values>
    constexpr std::string_view get_enum_values_impl() {
        return std::source_location::current().function_name();
    }

    template<size_t N>
    constexpr std::array<std::string_view, N> parse_enum_list(std::string_view sv, size_t prefix = 0) {
        std::array<std::string_view, N> array;
        size_t p = prefix + 2;
        for(size_t i = 0; i<N; i++) {
            size_t np = sv.find(",", p);
            auto name = sv.substr(p, np-p);
            array[i] = name;
            p = np + prefix + 2 + 2;
        }
        return array;
    }

    // based on https://stackoverflow.com/questions/28828957/how-to-convert-an-enum-to-a-string-in-modern-c#comment115999268_55312360
    template<typename Enum, auto... values>
    constexpr std::string_view get_enum_values_str(std::index_sequence<values...>) {
        auto sv = get_enum_values_impl<static_cast<Enum>(values)...>();
        sv.remove_prefix(std::string_view::traits_type::length("std::string_view utils::get_enum_values_impl() [values = <"));
        sv.remove_suffix(std::string_view::traits_type::length(">]"));

        return sv;
    }

    template<typename Enum, size_t N = 256>
    constexpr std::array<std::string_view, N> get_enum_values() {
        constexpr auto sv = get_enum_values_str<Enum>(std::make_index_sequence<N>());
        return parse_enum_list<N>(sv, get_typename<Enum>().size());
    }

    export template<typename Enum, size_t N = 256>
    constexpr std::string_view enum_name(Enum e) {
        constexpr auto list = get_enum_values<Enum, N>();
        return list[std::to_underlying(e)];
    }

}
