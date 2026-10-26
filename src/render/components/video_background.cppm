module;

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <openxmb/video_worker.hpp>

export module openxmb.render:video_background;

import dreamrender;
import :shaders;
import vulkan_hpp;
import vma;

namespace render {

export class video_background {
public:
    video_background(vk::Device device, vma::Allocator allocator, dreamrender::window* window = nullptr)
        : device(device), allocator(allocator), window(window) {}

    ~video_background() {
        decoder.reset();
        try { wait_gpu(); } catch(...) {}
        staging_buffers.clear();
        staging_allocations.clear();
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
        pipelines = dreamrender::createPipelines(device, cache, info, render_passes, "Video Background Pipeline");
    }

    void prepare(unsigned int count) {
        if(count == image_count && descriptor_pool) return;
        wait_gpu();
        initialize_material();
        image_count = count;
        descriptor_sets.clear();
        descriptor_pool.reset();
        staging_buffers.clear();
        staging_allocations.clear();
        if(count == 0) return;
        vk::DescriptorPoolSize size(vk::DescriptorType::eCombinedImageSampler, count);
        descriptor_pool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({}, count, size));
        std::vector<vk::DescriptorSetLayout> layouts(count, descriptor_layout.get());
        descriptor_sets = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(descriptor_pool.get(), layouts));
        if(texture) allocate_staging(texture->width, texture->height);
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
        if(active == value) return;
        active = value;
        if(decoder) decoder->pause(!active);
    }

    void tick() {
        if(!decoder || !active) return;
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
            if(!texture || texture->width != picture.width || texture->height != picture.height) {
                wait_gpu();
                texture.reset();
                staging_buffers.clear();
                staging_allocations.clear();
                texture = std::make_unique<dreamrender::texture>(device, allocator, picture.width, picture.height);
                allocate_staging(picture.width, picture.height);
                initialized = false;
            }
            allocator.copyMemoryToAllocation(picture.rgba.data(), staging_allocations[frame].get(), 0, picture.rgba.size());
            vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
            command.pipelineBarrier(initialized ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                vk::ImageMemoryBarrier(initialized ? vk::AccessFlags{vk::AccessFlagBits::eShaderRead} : vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite,
                    initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                    vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, texture->image, range));
            command.copyBufferToImage(staging_buffers[frame].get(), texture->image, vk::ImageLayout::eTransferDstOptimal,
                vk::BufferImageCopy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D{},
                    vk::Extent3D(static_cast<uint32_t>(picture.width), static_cast<uint32_t>(picture.height), 1)));
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
                vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                    vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, texture->image, range));
            source_aspect = picture.aspect_ratio;
            initialized = true;
            displayed_generation = true;
            pending.reset();
        }
        if(initialized && texture) {
            vk::DescriptorImageInfo image(sampler.get(), texture->imageView.get(), vk::ImageLayout::eShaderReadOnlyOptimal);
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
    std::string error() const { return decoder ? decoder->snapshot()->error : std::string{}; }
    double position() const { return decoder ? decoder->snapshot()->position : 0; }
    double duration() const { return decoder ? decoder->snapshot()->info.duration : 0; }

private:
    vk::Device device;
    vma::Allocator allocator;
    dreamrender::window* window = nullptr;
    std::filesystem::path path;
    std::vector<std::filesystem::path> playlist;
    std::size_t playlist_index = 0;
    std::unique_ptr<openxmb::video::worker> decoder;
    std::optional<openxmb::video::frame> pending;
    std::unique_ptr<dreamrender::texture> texture;
    std::vector<vma::UniqueAllocation> staging_allocations;
    std::vector<vma::UniqueBuffer> staging_buffers;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout descriptor_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorPool descriptor_pool;
    std::vector<vk::DescriptorSet> descriptor_sets;
    dreamrender::UniquePipelineMap pipelines;
    unsigned int image_count = 0;
    bool active = true;
    bool initialized = false;
    bool displayed_generation = false;
    double source_aspect = 1;

    void wait_gpu() { if(window) window->waitIdle(); else device.waitIdle(); }

    void begin_path(const std::filesystem::path& value, bool retain_frame) {
        path = value;
        if(!decoder) decoder = std::make_unique<openxmb::video::worker>();
        openxmb::video::options settings;
        settings.audio_output = false;
        settings.hardware_acceleration = OPENXMB_HWACCEL_STR;
        decoder->set_path(value, std::move(settings));
        decoder->set_repeat(playlist.size() <= 1);
        decoder->pause(!active);
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

    void allocate_staging(int width, int height) {
        staging_allocations.resize(image_count);
        staging_buffers.resize(image_count);
        vk::BufferCreateInfo buffer({}, static_cast<vk::DeviceSize>(width) * height * 4u, vk::BufferUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
        vma::AllocationCreateInfo allocation({}, vma::MemoryUsage::eCpuToGpu);
        for(unsigned int i = 0; i < image_count; ++i) std::tie(staging_buffers[i], staging_allocations[i]) = allocator.createBufferUnique(buffer, allocation);
    }
};

}
