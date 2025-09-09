/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
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

export module openxmb.render:shaders;

import vulkan_hpp;

export namespace render::shaders {

namespace blur {
    vk::UniqueShaderModule comp(vk::Device device);
}

namespace downsample {
    vk::UniqueShaderModule comp(vk::Device device);
}

namespace upsample {
    vk::UniqueShaderModule comp(vk::Device device);
}

namespace wave_renderer {
    vk::UniqueShaderModule vert(vk::Device device);
    vk::UniqueShaderModule frag(vk::Device device);
}

namespace yuv420p {
    vk::UniqueShaderModule decode_comp(vk::Device device);
}

namespace original_bg {
    vk::UniqueShaderModule vert(vk::Device device);
    vk::UniqueShaderModule frag(vk::Device device);
}

namespace original_particles {
    vk::UniqueShaderModule vert(vk::Device device);
    vk::UniqueShaderModule frag(vk::Device device);
}

}
