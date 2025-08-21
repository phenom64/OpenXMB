module;

#include <array>
#include <cstdint>

module openxmb.render;

import dreamrender;
import vulkan_hpp;

namespace render::shaders {

namespace blur {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char comp_array[] = {
    #embed "shaders/blur.comp.spv"
    };
    #pragma clang diagnostic pop

    constexpr std::array comp_shader = dreamrender::convert<std::to_array(comp_array), uint32_t>();

    vk::UniqueShaderModule comp(vk::Device device) {
        return dreamrender::createShader(device, comp_shader);
    }
}

namespace wave_renderer {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char vert_array[] = {
    #embed "shaders/wave.vert.spv"
    };
    constexpr char frag_array[] = {
    #embed "shaders/wave.frag.spv"
    };
    #pragma clang diagnostic pop

    constexpr std::array vert_shader = dreamrender::convert<std::to_array(vert_array), uint32_t>();
    constexpr std::array frag_shader = dreamrender::convert<std::to_array(frag_array), uint32_t>();

    vk::UniqueShaderModule vert(vk::Device device) {
        return dreamrender::createShader(device, vert_shader);
    }
    vk::UniqueShaderModule frag(vk::Device device) {
        return dreamrender::createShader(device, frag_shader);
    }
}

namespace yuv420p {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char decode_comp_array[] = {
    #embed "shaders/yuv420p_decode.comp.spv"
    };
    #pragma clang diagnostic pop

    constexpr std::array decode_comp_shader = dreamrender::convert<std::to_array(decode_comp_array), uint32_t>();

    vk::UniqueShaderModule decode_comp(vk::Device device) {
        return dreamrender::createShader(device, decode_comp_shader);
    }
}

}