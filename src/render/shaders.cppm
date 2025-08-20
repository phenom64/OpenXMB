module;

export module shell.render:shaders;

import vulkan_hpp;

export namespace render::shaders {

namespace blur {
    vk::UniqueShaderModule comp(vk::Device device);
}

namespace wave_renderer {
    vk::UniqueShaderModule vert(vk::Device device);
    vk::UniqueShaderModule frag(vk::Device device);
}

namespace yuv420p {
    vk::UniqueShaderModule decode_comp(vk::Device device);
}

}