module;

export module openxmb.app:blur_layer;

import dreamrender;
import vulkan_hpp;
import :component;

namespace app {

export class blur_layer : public component {
    public:
        blur_layer(class shell* xmb);

        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override;
        [[nodiscard]] bool is_opaque() const override { return false; }
    private:
        dreamrender::texture srcTexture;
        dreamrender::texture dstTexture;

        vk::UniqueDescriptorSetLayout descriptorSetLayout;
        vk::UniqueDescriptorPool descriptorPool;
        vk::DescriptorSet descriptorSet;
        vk::UniquePipelineLayout pipelineLayout;
        vk::UniquePipeline pipeline;
};

}
