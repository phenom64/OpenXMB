module;
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
#include "../video_color.hpp"
export module openxmb.render:video_texture;
import vulkan_hpp;
import vma;
export {
#include "../video_texture_impl.hpp"
}
