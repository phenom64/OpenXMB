/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
 *
 * Modified by Syndromatic Ltd for OpenXMB.
 * Portions Copyright (C) 2025 Syndromatic Ltd, Kavish Krishnakumar.
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

namespace downsample {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char comp_array[] = {
    #embed "shaders/downsample.comp.spv"
    };
    #pragma clang diagnostic pop

    constexpr std::array comp_shader = dreamrender::convert<std::to_array(comp_array), uint32_t>();

    vk::UniqueShaderModule comp(vk::Device device) {
        return dreamrender::createShader(device, comp_shader);
    }
}

namespace upsample {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char comp_array[] = {
    #embed "shaders/upsample.comp.spv"
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

namespace original_bg {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char vert_array[] = {
    #embed "shaders/original.vert.spv"
    };
    constexpr char frag_array[] = {
    #embed "shaders/original.frag.spv"
    };
    #pragma clang diagnostic pop

    constexpr std::array vert_shader = dreamrender::convert<std::to_array(vert_array), uint32_t>();
    constexpr std::array frag_shader = dreamrender::convert<std::to_array(frag_array), uint32_t>();

    vk::UniqueShaderModule vert(vk::Device device) { return dreamrender::createShader(device, vert_shader); }
    vk::UniqueShaderModule frag(vk::Device device) { return dreamrender::createShader(device, frag_shader); }
}

namespace original_particles {
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wc23-extensions"
    constexpr char vert_array[] = {
    #embed "shaders/original_particles.vert.spv"
    };
    constexpr char frag_array[] = {
    #embed "shaders/original_particles.frag.spv"
    };
    #pragma clang diagnostic pop

    constexpr std::array vert_shader = dreamrender::convert<std::to_array(vert_array), uint32_t>();
    constexpr std::array frag_shader = dreamrender::convert<std::to_array(frag_array), uint32_t>();

    vk::UniqueShaderModule vert(vk::Device device) { return dreamrender::createShader(device, vert_shader); }
    vk::UniqueShaderModule frag(vk::Device device) { return dreamrender::createShader(device, frag_shader); }
}

}
