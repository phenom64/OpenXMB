module;

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <openxmb/video_worker.hpp>

export module openxmb.render:video_background;

import auroreengine;
import :shaders;
export import :video_texture;
import openxmb.config;
import vulkan_hpp;
import vma;

namespace render {

export class video_background {
public:
    video_background(vk::Device device, vma::Allocator allocator, auroreengine::window* window = nullptr)
        : device(device), allocator(allocator), window(window) {}

    ~video_background() {
        decoder.reset();
        try { wait_gpu(); } catch(...) {}
        texture.reset();
    }

    void preload(const std::vector<vk::RenderPass>& render_passes, vk::SampleCountFlagBits samples,
                 vk::PipelineCache cache = {}) {
        initialize_material();
        auto vertex = shaders::video_background::vert(device);
        auto fragment = shaders::video_background::frag(device);
        std::array stages{
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, vertex.get(), "main"),
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fragment.get(), "main")
        };
        vk::PipelineVertexInputStateCreateInfo vertex_input{};
        vk::PipelineInputAssemblyStateCreateInfo assembly({}, vk::PrimitiveTopology::eTriangleList);
        vk::Viewport viewport{};
        vk::Rect2D scissor{};
        vk::PipelineViewportStateCreateInfo viewport_state({}, viewport, scissor);
        vk::PipelineRasterizationStateCreateInfo raster({}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise, false, 0, 0, 0, 1);
        vk::PipelineMultisampleStateCreateInfo multisample({}, samples);
        vk::PipelineDepthStencilStateCreateInfo depth({}, false, false);
        vk::PipelineColorBlendAttachmentState attachment(false);
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo blend({}, false, vk::LogicOp::eClear, attachment);
        std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic({}, dynamic_states);
        vk::GraphicsPipelineCreateInfo info({}, stages, &vertex_input, &assembly, {}, &viewport_state, &raster, &multisample, &depth, &blend, &dynamic, pipeline_layout.get(), {});
        pipelines = auroreengine::createPipelines(device, cache, info, render_passes, "Video Background Pipeline");
    }

    void prepare(unsigned int count) {
        if(count == image_count && descriptor_pool) return;
        wait_gpu();
        initialize_material();
        image_count = count;
        descriptor_sets.clear();
        descriptor_pool.reset();
        if(count == 0) return;
        vk::DescriptorPoolSize size(vk::DescriptorType::eCombinedImageSampler, count);
        descriptor_pool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({}, count, size));
        std::vector<vk::DescriptorSetLayout> layouts(count, descriptor_layout.get());
        descriptor_sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(descriptor_pool.get(), layouts));
    }

    // set_path only posts a request. Opening files, decoding, seeking, and
    // repeating happen on the persistent worker, including path replacements.
    void set_path(const std::filesystem::path& value) {
        if(path == value && playlist.empty()) return;
        playlist.clear();
        playlist_index = 0;
        begin_path(value, false);
    }

    void set_playlist(const std::vector<std::filesystem::path>& values) {
        if(playlist == values) return;
        playlist = values;
        playlist_index = 0;
        begin_path(playlist.empty() ? std::filesystem::path{} : playlist.front(), false);
    }

    void set_active(bool value) {
        active = value;
        sync_decoder_pause();
    }

    void tick() {
        sync_decoder_pause();
        if(!decoder || !active) return;
        // Motion policy pauses decoding, not visibility. Keep the last rendered
        // image; a newly selected background may still produce its first frame.
        if(motion_paused && initialized && displayed_generation) {pending.reset();return;}
        if(!playlist.empty() && decoder->snapshot()->status == openxmb::video::state::ended) {
            playlist_index = (playlist_index + 1) % playlist.size();
            begin_path(playlist[playlist_index], true);
        }
        if(auto frame = decoder->take_frame()) pending = std::move(frame);
    }

    void prerender(vk::CommandBuffer command, int frame) {
        tick();
        if(frame < 0 || static_cast<unsigned int>(frame) >= image_count) return;
        if(pending) {
            auto& picture = *pending;
            try {
                if(!texture) texture=std::make_unique<video_texture>(device,allocator,
                    window&&video_texture::supports_native(window->physicalDevice,window->queueFamilyIndices.graphicsFamily.value()),
                    [this] {return shaders::yuv420p::decode_comp(device);},[this] {wait_gpu();});
                if(!texture->upload(command,frame,image_count,picture)) return;
                upload_error.clear();
            } catch(const std::exception& exception) {upload_error=exception.what();pending.reset();return;}
            source_aspect = picture.aspect_ratio;
            initialized = true;
            displayed_generation = true;
            pending.reset();
        }
        if(initialized && texture) {
            vk::DescriptorImageInfo image(sampler.get(), texture->view(), vk::ImageLayout::eShaderReadOnlyOptimal);
            device.updateDescriptorSets(vk::WriteDescriptorSet(descriptor_sets[frame], 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &image), {});
        }
    }

    void render(vk::CommandBuffer command, int frame, vk::RenderPass render_pass, vk::Extent2D extent) {
        if(!active || !ready() || frame < 0 || static_cast<std::size_t>(frame) >= descriptor_sets.size() || extent.width == 0 || extent.height == 0) return;
        auto pipeline = pipelines.find(render_pass);
        if(pipeline == pipelines.end()) return;
        float target_aspect = static_cast<float>(extent.width) / extent.height;
        std::array<float, 2> crop{1, 1};
        if(source_aspect > target_aspect) crop[0] = static_cast<float>(target_aspect / source_aspect);
        else crop[1] = static_cast<float>(source_aspect / target_aspect);
        command.setViewport(0, vk::Viewport(0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1));
        command.setScissor(0, vk::Rect2D({}, extent));
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->second.get());
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout.get(), 0, descriptor_sets[frame], {});
        command.pushConstants(pipeline_layout.get(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(crop), crop.data());
        command.draw(3, 1, 0, 0);
    }

    bool ready() const { return active && initialized && displayed_generation; }
    std::string error() const { if(!upload_error.empty()) return upload_error;return decoder ? decoder->snapshot()->error : std::string{}; }
    double position() const { return decoder ? decoder->snapshot()->position : 0; }
    double duration() const { return decoder ? decoder->snapshot()->info.duration : 0; }

private:
    vk::Device device;
    vma::Allocator allocator;
    auroreengine::window* window = nullptr;
    std::filesystem::path path;
    std::vector<std::filesystem::path> playlist;
    std::size_t playlist_index = 0;
    std::unique_ptr<openxmb::video::worker> decoder;
    std::optional<openxmb::video::frame> pending;
    std::unique_ptr<video_texture> texture;
    std::string upload_error;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout descriptor_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorPool descriptor_pool;
    std::vector<vk::DescriptorSet> descriptor_sets;
    auroreengine::UniquePipelineMap pipelines;
    unsigned int image_count = 0;
    bool active = true;
    bool motion_paused = false;
    std::optional<bool> decoder_paused;
    bool initialized = false;
    bool displayed_generation = false;
    double source_aspect = 1;

    void wait_gpu() { if(window) window->waitIdle(); else device.waitIdle(); }
    void sync_decoder_pause() {
        motion_paused=config::CONFIG.enabled("power.reduced.motion",false);
        const bool pause=!active||motion_paused;
        if(decoder&&(!decoder_paused||*decoder_paused!=pause)) {decoder->pause(pause);decoder_paused=pause;}
    }

    void begin_path(const std::filesystem::path& value, bool retain_frame) {
        path = value;
        if(!decoder) decoder = std::make_unique<openxmb::video::worker>();
        openxmb::video::options settings;
        settings.audio_output = false;
        settings.hardware_acceleration = config::CONFIG.preference("video.hardware",OPENXMB_HWACCEL_STR);
        settings.native_yuv=true;
        decoder->set_path(value, std::move(settings));
        decoder->set_repeat(playlist.size() <= 1);
        decoder_paused.reset();sync_decoder_pause();
        pending.reset();
        if(!retain_frame) displayed_generation = false;
    }

    void initialize_material() {
        if(descriptor_layout) return;
        vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
        descriptor_layout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, binding));
        vk::PushConstantRange range(vk::ShaderStageFlagBits::eFragment, 0, sizeof(float) * 2);
        pipeline_layout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, descriptor_layout.get(), range));
        sampler = device.createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eNearest,
            vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge));
    }


};

}
