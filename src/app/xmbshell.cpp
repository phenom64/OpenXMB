module;

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>
#include <version>

module xmbshell.app;

import i18n;
import spdlog;
import dreamrender;
import glm;
import vulkan_hpp;
import vma;
import sdl2;
import xmbshell.config;
import xmbshell.render;
import xmbshell.utils;

using namespace mfk::i18n::literals;

namespace app
{
    struct BlurConstants {
        int axis = 0;
        int size = 20;
    };

    xmbshell::xmbshell(window* window) : phase(window)
    {
    }

    xmbshell::~xmbshell()
    {
    }

    void xmbshell::preload()
    {
        phase::preload();

        font_render = std::make_unique<font_renderer>(config::CONFIG.fontPath.string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        image_render = std::make_unique<image_renderer>(device, win->swapchainExtent, win->gpuFeatures);
        simple_render = std::make_unique<simple_renderer>(device, allocator, win->swapchainExtent, win->gpuFeatures);
        wave_render = std::make_unique<render::wave_renderer>(device, allocator, win->swapchainExtent);

        {
            std::array<vk::AttachmentDescription, 2> attachments = {
                vk::AttachmentDescription({}, win->swapchainFormat.format, win->config.sampleCount,
                    vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                    vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal),
                vk::AttachmentDescription({}, win->swapchainFormat.format, vk::SampleCountFlagBits::e1,
                    vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                    vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal)
            };
            vk::AttachmentReference ref(0, vk::ImageLayout::eColorAttachmentOptimal);
            vk::AttachmentReference rref(1, vk::ImageLayout::eColorAttachmentOptimal);
            vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, {}, ref, rref);
            std::array<vk::SubpassDependency, 2> deps{
                vk::SubpassDependency(vk::SubpassExternal, 0,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eColorAttachmentWrite),
                vk::SubpassDependency(0, vk::SubpassExternal,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
                    vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead)
            };
            vk::RenderPassCreateInfo renderpass_info({}, attachments, subpass, deps);
            backgroundRenderPass = device.createRenderPassUnique(renderpass_info);
            debugName(device, backgroundRenderPass.get(), "Background Render Pass");
        }
        {
            std::array<vk::AttachmentDescription, 2> attachments = {
                vk::AttachmentDescription({}, win->swapchainFormat.format, win->config.sampleCount,
                    vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                    vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal),
                vk::AttachmentDescription({}, win->swapchainFormat.format, vk::SampleCountFlagBits::e1,
                    vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                    vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                    vk::ImageLayout::eUndefined, win->swapchainFinalLayout)
            };
            vk::AttachmentReference ref(0, vk::ImageLayout::eColorAttachmentOptimal);
            vk::AttachmentReference rref(1, vk::ImageLayout::eColorAttachmentOptimal);
            vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, {}, ref, rref);
            vk::SubpassDependency dep(vk::SubpassExternal, 0,
                vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eColorAttachmentWrite);
            vk::RenderPassCreateInfo renderpass_info({}, attachments, subpass, dep);
            shellRenderPass = device.createRenderPassUnique(renderpass_info);
            debugName(device, shellRenderPass.get(), "Shell Render Pass");
        }
        {
            std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
                vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
                vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
            };
            vk::DescriptorSetLayoutCreateInfo info({}, bindings);
            blurDescriptorSetLayout = device.createDescriptorSetLayoutUnique(info);
        }
        {
            vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants)};
            vk::PipelineLayoutCreateInfo info({}, blurDescriptorSetLayout.get(), range);
            blurPipelineLayout = device.createPipelineLayoutUnique(info);
        }
        {
            vk::UniqueShaderModule compShader = render::shaders::blur::comp(device);
            vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
            vk::ComputePipelineCreateInfo info({}, shader, blurPipelineLayout.get());
            blurPipeline = device.createComputePipelineUnique({}, info).value;
            debugName(device, blurPipeline.get(), "Blur Pipeline");
        }

        {
            renderImage = std::make_unique<texture>(device, allocator,
                win->swapchainExtent, vk::ImageUsageFlagBits::eColorAttachment,
                win->swapchainFormat.format, win->config.sampleCount, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, renderImage->image, "Shell Render Image");

            blurImageSrc = std::make_unique<texture>(device, allocator,
                win->swapchainExtent, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, blurImageSrc->image, "Blur Image Source");

            blurImageDst = std::make_unique<texture>(device, allocator,
                win->swapchainExtent, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, blurImageDst->image, "Blur Image Destination");
        }

        font_render->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff);
        image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());

        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
        config::CONFIG.addCallback("background-type", [this](const std::string&){
            if(config::CONFIG.backgroundType == config::config::background_type::image) {
                reload_background();
            } else {
                backgroundTexture.reset();
            }
        });
        config::CONFIG.addCallback("background-image", [this](const std::string&){
            if(config::CONFIG.backgroundType == config::config::background_type::image) {
                reload_background();
            } else {
                backgroundTexture.reset();
            }
        });
        config::CONFIG.addCallback("controller-type", [this](const std::string&){
            reload_button_icons();
        });
        config::CONFIG.addCallback("vsync", [this](const std::string&){
            spdlog::info("VSync changed to {}", config::CONFIG.preferredPresentMode == vk::PresentModeKHR::eFifoRelaxed ? "on" : "off");
            win->config.preferredPresentMode = config::CONFIG.preferredPresentMode;
            win->recreateSwapchain();
        });

        if(!background_only) {
            preload_fixed_components();
        }
    }

    void xmbshell::preload_fixed_components()
    {
        if(fixed_components_loaded) {
            return;
        }

        menu.preload(device, allocator, *loader);
        news.preload(device, allocator, *loader);

        ok_sound = sdl::mix::unique_chunk{sdl::mix::LoadWAV((config::CONFIG.asset_directory/"sounds/ok.wav").string().c_str())};
        if(!ok_sound) {
            spdlog::error("sdl::mix::LoadWAV: {}", sdl::mix::GetError());
        }

        reload_button_icons();
    }

    void xmbshell::prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews)
    {
        phase::prepare(swapchainImages, swapchainViews);

        const unsigned int imageCount = swapchainImages.size();
        {
            vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 2*imageCount);
            vk::DescriptorPoolCreateInfo pool_info({}, imageCount, size);
            blurDescriptorPool = device.createDescriptorPoolUnique(pool_info);

            std::vector<vk::DescriptorSetLayout> layouts(imageCount, blurDescriptorSetLayout.get());
            vk::DescriptorSetAllocateInfo alloc_info(blurDescriptorPool.get(), layouts);
            blurDescriptorSets = device.allocateDescriptorSets(alloc_info);
        }
        this->swapchainImages = swapchainImages;

        std::vector<vk::DescriptorImageInfo> imageInfos(2*imageCount);
        std::vector<vk::WriteDescriptorSet> writes(imageCount);

        framebuffers.clear();
        backgroundFramebuffers.clear();
        for(int i=0; i<imageCount; i++)
        {
            debugName(device, swapchainImages[i], "Swapchain Image #"+std::to_string(i));
            {
                std::array<vk::ImageView, 2> attachments = {renderImage->imageView.get(), swapchainViews[i]};
                vk::FramebufferCreateInfo framebuffer_info({}, shellRenderPass.get(), attachments,
                    win->swapchainExtent.width, win->swapchainExtent.height, 1);
                framebuffers.push_back(device.createFramebufferUnique(framebuffer_info));
                debugName(device, framebuffers.back().get(), "XMB Shell Framebuffer #"+std::to_string(i));
            }
            {
                std::array<vk::ImageView, 2> attachments = {renderImage->imageView.get(), swapchainViews[i]};
                vk::FramebufferCreateInfo framebuffer_info({}, backgroundRenderPass.get(), attachments,
                    win->swapchainExtent.width, win->swapchainExtent.height, 1);
                backgroundFramebuffers.push_back(device.createFramebufferUnique(framebuffer_info));
                debugName(device, backgroundFramebuffers.back().get(), "XMB Shell Background Framebuffer #"+std::to_string(i));
            }

            imageInfos[2*i] = vk::DescriptorImageInfo({}, blurImageSrc->imageView.get(), vk::ImageLayout::eGeneral);
            imageInfos[2*i+1] = vk::DescriptorImageInfo({}, blurImageDst->imageView.get(), vk::ImageLayout::eGeneral);
            writes[i] = vk::WriteDescriptorSet(blurDescriptorSets[i], 0, 0, 2, vk::DescriptorType::eStorageImage, &imageInfos[2*i]);
        }
        device.updateDescriptorSets(writes, {});

        font_render->prepare(swapchainViews.size());
        image_render->prepare(swapchainViews.size());
        simple_render->prepare(swapchainViews.size());
        wave_render->prepare(swapchainViews.size());
    }

    void xmbshell::render(int frame, vk::Semaphore imageAvailable, vk::Semaphore renderFinished, vk::Fence fence)
    {
        tick();

        vk::CommandBuffer commandBuffer = commandBuffers[frame];
        auto now = std::chrono::system_clock::now();

        commandBuffer.begin(vk::CommandBufferBeginInfo());
        for(auto& overlay : std::views::reverse(overlays)) {
            overlay->prerender(commandBuffer, frame, this);
        }
        {
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            if(config::CONFIG.backgroundType == config::config::background_type::color) {
                color = vk::ClearColorValue(std::array<float, 4>{
                    config::CONFIG.backgroundColor.r,
                    config::CONFIG.backgroundColor.g,
                    config::CONFIG.backgroundColor.b,
                    1.0f
                });
            }
            if(ingame_mode) {
                color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.5f});
            }
            commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(backgroundRenderPass.get(), backgroundFramebuffers[frame].get(),
                vk::Rect2D({0, 0}, win->swapchainExtent), color), vk::SubpassContents::eInline);
            vk::Viewport viewport(0.0f, 0.0f,
                static_cast<float>(win->swapchainExtent.width),
                static_cast<float>(win->swapchainExtent.height), 0.0f, 1.0f);
            vk::Rect2D scissor({0,0}, win->swapchainExtent);
            commandBuffer.setViewport(0, viewport);
            commandBuffer.setScissor(0, scissor);

            if(!ingame_mode) {
                if(config::CONFIG.backgroundType == config::config::background_type::wave) {
                    wave_render->waveColor = config::CONFIG.waveColor;
                    wave_render->render(commandBuffer, frame, backgroundRenderPass.get());
                }
                else if(config::CONFIG.backgroundType == config::config::background_type::image) {
                    if(backgroundTexture) {
                        image_render->renderImageSized(commandBuffer, frame, backgroundRenderPass.get(), *backgroundTexture,
                            0.0f, 0.0f,
                            static_cast<int>(win->swapchainExtent.width),
                            static_cast<int>(win->swapchainExtent.height)
                        );
                    }
                }
            }

            commandBuffer.endRenderPass();
        }
        double blur_background_progress = utils::progress(now, last_blur_background_change, blur_background_transition_duration);
        if(blur_background || blur_background_progress < 1.0) {
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferRead,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        swapchainImages[frame], vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            commandBuffer.blitImage(swapchainImages[frame], vk::ImageLayout::eTransferSrcOptimal,
                blurImageSrc->image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageBlit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    {vk::Offset3D(0, 0, 0), vk::Offset3D(static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height), 1)},
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), {vk::Offset3D(0, 0, 0), vk::Offset3D(blurImageSrc->width, blurImageSrc->height, 1)}),
                vk::Filter::eLinear);

            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eShaderWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            int groupCountX = static_cast<int>(std::ceil(blurImageSrc->width/16.0));
            int groupCountY = static_cast<int>(std::ceil(blurImageSrc->height/16.0));

            BlurConstants constants{};
            constants.size = static_cast<int>(20 * (blur_background ? blur_background_progress : (1.0 - blur_background_progress)));
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
            commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurDescriptorSets[frame]}, {});

            constants.axis = 0;
            commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
            commandBuffer.dispatch(groupCountX, groupCountY, 1);

            // TODO: THIS IS WRONG! We need to copy dst -> src with appropriate barriers (see blur_layer)
            // But it kinda work, so I'll leave it for now

            constants.axis = 1;
            commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
            commandBuffer.dispatch(groupCountX, groupCountY, 1);

            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );
        }
        else {
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferRead,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        swapchainImages[frame], vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            commandBuffer.blitImage(swapchainImages[frame], vk::ImageLayout::eTransferSrcOptimal,
                blurImageDst->image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageBlit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    {vk::Offset3D(0, 0, 0), vk::Offset3D(static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height), 1)},
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), {vk::Offset3D(0, 0, 0), vk::Offset3D(blurImageDst->width, blurImageDst->height, 1)}),
                vk::Filter::eLinear);

            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );
            /*commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eColorAttachmentWrite,
                        vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                        swapchainImages[frame], vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );*/
        }
        {
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(shellRenderPass.get(), framebuffers[frame].get(),
                vk::Rect2D({0, 0}, win->swapchainExtent), color), vk::SubpassContents::eInline);
            vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(win->swapchainExtent.width), static_cast<float>(win->swapchainExtent.height), 0.0f, 1.0f);
            vk::Rect2D scissor({0,0}, win->swapchainExtent);
            commandBuffer.setViewport(0, viewport);
            commandBuffer.setScissor(0, scissor);

            image_render->renderImageSized(commandBuffer, frame, shellRenderPass.get(), blurImageDst->imageView.get(),
                0.0f, 0.0f, static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height));

            gui_renderer ctx(commandBuffer, frame, shellRenderPass.get(), win->swapchainExtent, font_render.get(), image_render.get(), simple_render.get());
            if(!background_only) {
                render_gui(ctx);
            }

            commandBuffer.endRenderPass();
        }
        font_render->finish(frame);
        image_render->finish(frame);
        simple_render->finish(frame);
        commandBuffer.end();

        vk::PipelineStageFlags waitFlags = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo submit_info(imageAvailable, waitFlags, commandBuffer, renderFinished);
        graphicsQueue.submit(submit_info, fence);
    }

    void xmbshell::render_gui(gui_renderer& renderer) {
        bool render_menu = true;
        unsigned int overlay_begin = 0;
        bool has_overlay = !overlays.empty();

        if(has_overlay) {
            for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
                if(overlays[i]->is_opaque()) {
                    overlay_begin = i;
                    render_menu = false;
                    break;
                }
            }
        }

        auto now = std::chrono::system_clock::now();
        // TODO: somehow fix this.... god this is gonna be a huge mess
        double overlay_progress = utils::progress(now, overlay_fade_time, overlay_transition_duration);
        double dir_progress = overlay_fade_direction == transition_direction::in ? overlay_progress : 1.0 - overlay_progress;
        bool overlay_transition = overlay_progress < 1.0;
        if(render_menu){
            if(overlay_transition || has_overlay) {
                constexpr glm::vec4 factor{0.25f, 0.25f, 0.25f, 1.0f};
                renderer.push_color(glm::mix(glm::vec4(1.0), factor, dir_progress));
            }
            menu.render(renderer);

#if __cpp_lib_chrono >= 201907L || defined(__GLIBCXX__)
            static const std::chrono::time_zone* timezone = [](){
                auto tz = std::chrono::current_zone();
                auto system = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                auto local = std::chrono::zoned_time(tz, system);
                spdlog::debug("{}", std::format("Timezone: {}, System Time: {}, Local Time: {}", tz->name(), system, local));
                return tz;
            }();
            auto local_now = std::chrono::zoned_time(timezone, std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
#else
            auto local_now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
#endif
            renderer.draw_text(std::vformat("{:"+config::CONFIG.dateTimeFormat+"}", std::make_format_args(local_now)),
                static_cast<float>(0.831770833f+config::CONFIG.dateTimeOffset), 0.086111111f, 0.021296296f*2.5f);

            news.render(renderer);
            if(overlay_transition || has_overlay) {
                renderer.pop_color();
            }
            /*if(choice_overlay || choice_overlay_progress < 1.0) {
                renderer.pop_color();
                renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), choice_overlay ? choice_overlay_progress : 1.0 - choice_overlay_progress));
                choice_overlay.or_else([this](){return old_choice_overlay;})->render(renderer);
                renderer.pop_color();
            } else {
                old_choice_overlay = std::nullopt;
            }*/
        }

        for(unsigned int i=overlay_begin; i < overlays.size(); i++) {
            // TODO: support darkening overlays on top of overlays (i.e. a choice_overlay over a message_overlay)
            if(i == overlays.size()-1 && overlay_transition) {
                renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
                overlays[i]->render(renderer, this);
                renderer.pop_color();
            } else {
                overlays[i]->render(renderer, this);
            }
        }
        if(overlay_transition && overlay_fade_direction == transition_direction::out && old_overlay) {
            renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
            old_overlay->render(renderer, this);
            renderer.pop_color();
        } else if(old_overlay) {
            old_overlay.reset();
        }

        float debug_y = 0.0;
        if(config::CONFIG.showFPS) {
            renderer.draw_text("FPS: {:.2f}"_(win->currentFPS), 0, debug_y, 0.05f, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
            debug_y += 0.025f;
        }
        if(config::CONFIG.showMemory) {
            vk::DeviceSize budget{}, usage{};
            for(const auto& b : allocator.getHeapBudgets()) {
                budget += b.budget;
                usage += b.usage;
            }
            constexpr double mb = 1024.0*1024.0;
            auto u = static_cast<double>(usage)/mb;
            auto b = static_cast<double>(budget)/mb;
            renderer.draw_text("Video Memory: {:.2f}/{:.2f} MB"_(u, b), 0, debug_y, 0.05f, glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
            debug_y += 0.025f;
        }
    }

    void xmbshell::reload_background() {
        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
    }
    void xmbshell::reload_button_icons() {
        auto controller_type = get_controller_type();

        for(std::underlying_type_t<action> i = std::to_underlying(action::none)+1; i < std::to_underlying(action::_length); i++) {
            auto a = static_cast<action>(i);
            if(controller_type == "none") {
                continue;
            }

            std::string_view name = utils::enum_name(a);
            std::filesystem::path icon_name = config::CONFIG.asset_directory / "icons" / std::format("icon_button_{}_{}.png", controller_type, name);

            buttonTextures[i] = std::make_unique<texture>(device, allocator);
            loader->loadTexture(buttonTextures[i].get(), icon_name);
        }
    }
    std::string xmbshell::get_controller_type() const {
        auto type = config::CONFIG.controllerType;
        if(type == "auto") {
            if(win->controllers.empty()) {
                return "keyboard";
            }
            for(const auto& [id, controller] : win->controllers) {
                sdl::GameControllerType ctype = sdl::GameControllerGetType(controller.get());
                if(ctype == sdl::GameControllerTypeValues::PS4 ||
                   ctype == sdl::GameControllerTypeValues::PS5) {
                    return "playstation";
                } else if(ctype == sdl::GameControllerTypeValues::XBOX360 ||
                          ctype == sdl::GameControllerTypeValues::XBOXONE) {
                    return "xbox";
                }
                std::string_view name = sdl::GameControllerName(controller.get());
                if(name == "Steam Virtual Gamepad" || name == "Steam Controller") {
                    return "steam";
                }
            }
            return "ouya"; // totally sensible default :P
        }
        return type;
    }

    void xmbshell::tick() {
        if(background_only) {
            return;
        }

        for(unsigned int i=0; i<2; i++) {
            if(last_controller_axis_input[i]) {
                auto time_since_input = std::chrono::duration<double>(std::chrono::system_clock::now() - last_controller_axis_input_time[i]);
                if(time_since_input > controller_axis_input_duration) {
                    auto [controller, dir] = *last_controller_axis_input[i];
                    dispatch(dir);
                    last_controller_axis_input_time[i] = std::chrono::system_clock::now();
                }
            }
        }
        if(last_controller_button_input) {
            auto time_since_input = std::chrono::duration<double>(std::chrono::system_clock::now() - last_controller_button_input_time);
            if(time_since_input > controller_button_input_duration) {
                auto [controller, button] = *last_controller_button_input;
                button_down(controller, button);
            }
        }

        for(unsigned int i=0; i<overlays.size(); i++) {
            auto res = overlays[i]->tick(this);
            if(res & result::close) {
                remove_overlay(i);
                i--;
            }
            handle(res);
        }
    }

    void xmbshell::dispatch(action action) {
        if(background_only) {
            return;
        }

        for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
            auto& e = overlays[i];
            if(auto* recv = dynamic_cast<action_receiver*>(e.get())) {
                result res = recv->on_action(action);
                if(res & result::close) {
                    remove_overlay(i);
                    i--;
                }
                handle(res);
                if(res != result::unsupported) {
                    return;
                }
            }
        }

        handle(menu.on_action(action));
    }
    void xmbshell::handle(result result) {
        if(result & result::error_rumble) {
            if(config::CONFIG.controllerRumble) {
                for(const auto& [id, controller] : win->controllers) {
                    sdl::GameControllerRumble(controller.get(), 1000, 10000, 100);
                }
            }
        }
        if(result & result::ok_sound) {
            if(sdl::mix::PlayChannel(-1, ok_sound.get(), 0) == -1) {
                spdlog::error("sdl::mix::PlayChannel: {}", sdl::mix::GetError());
            }
        }
    }

    void xmbshell::key_up(sdl::Keysym key)
    {
        spdlog::trace("Key up: {}", key.sym);
    }
    void xmbshell::key_down(sdl::Keysym key)
    {
        spdlog::trace("Key down: {}", key.sym);
        switch(key.sym) {
            case SDLK_LEFT:
                dispatch(action::left);
                break;
            case SDLK_RIGHT:
                dispatch(action::right);
                break;
            case SDLK_UP:
                dispatch(action::up);
                break;
            case SDLK_DOWN:
                dispatch(action::down);
                break;
            case SDLK_RETURN:
                dispatch(action::ok);
                break;
            case SDLK_ESCAPE:
                dispatch(action::cancel);
                break;
            case SDLK_TAB:
                dispatch(action::options);
                break;
            case SDLK_CAPSLOCK:
                dispatch(action::extra);
                break;
        }
    }

    void xmbshell::add_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void xmbshell::remove_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void xmbshell::button_down(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button down: {}", fmt::underlying(button));
        last_controller_button_input = std::make_tuple(controller, button);
        last_controller_button_input_time = std::chrono::system_clock::now();

        if(button == sdl::GameControllerButtonValues::DPAD_LEFT) {
            dispatch(action::left);
        } else if(button == sdl::GameControllerButtonValues::DPAD_RIGHT) {
            dispatch(action::right);
        } else if(button == sdl::GameControllerButtonValues::DPAD_UP) {
            dispatch(action::up);
        } else if(button == sdl::GameControllerButtonValues::DPAD_DOWN) {
            dispatch(action::down);
        } else if(button == sdl::GameControllerButtonValues::A) {
            dispatch(action::ok);
        } else if(button == sdl::GameControllerButtonValues::B) {
            dispatch(action::cancel);
        } else if(button == sdl::GameControllerButtonValues::Y) {
            dispatch(action::options);
        } else if(button == sdl::GameControllerButtonValues::X) {
            dispatch(action::extra);
        }
    }
    void xmbshell::button_up(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button up: {}", fmt::underlying(button));
        last_controller_button_input = std::nullopt;
    }
    void xmbshell::axis_motion(sdl::GameController* controller, sdl::GameControllerAxis axis, int16_t value)
    {
        spdlog::trace("Axis motion: {} {}", fmt::underlying(axis), value);

        unsigned int stick_index = 0;
        float v = static_cast<float>(value) / std::numeric_limits<int16_t>::max();
        switch(axis) {
            case sdl::GameControllerAxisValues::LEFTX:
                stick_index = 0;
                controller_axis_position[0].x = v;
                break;
            case sdl::GameControllerAxisValues::LEFTY:
                stick_index = 0;
                controller_axis_position[0].y = v;
                break;
            case sdl::GameControllerAxisValues::RIGHTX:
                stick_index = 1;
                controller_axis_position[1].x = v;
                break;
            case sdl::GameControllerAxisValues::RIGHTY:
                stick_index = 1;
                controller_axis_position[1].y = v;
                break;
            default:
                break;
        }
        for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
            auto& e = overlays[i];
            if(auto* recv = dynamic_cast<joystick_receiver*>(e.get())) {
                result res = recv->on_joystick(stick_index,
                    controller_axis_position[stick_index].x,
                    controller_axis_position[stick_index].y);
                if(res & result::close) {
                    remove_overlay(i);
                    i--;
                }
                handle(res);
                if(res != result::unsupported) {
                    return;
                }
            }
        }

        if(!config::CONFIG.controllerAnalogStick) {
            return;
        }

        if(axis == sdl::GameControllerAxisValues::LEFTX || axis == sdl::GameControllerAxisValues::LEFTY) {
            unsigned int index = axis == sdl::GameControllerAxisValues::LEFTX ? 0 : 1;
            if(std::abs(value) < controller_axis_input_threshold) {
                last_controller_axis_input[index] = std::nullopt;
                last_controller_axis_input_time[index] = std::chrono::system_clock::now();
                return;
            }
            action dir = axis == sdl::GameControllerAxisValues::LEFTX  ? (value > 0 ? action::right : action::left)
                : (value > 0 ? action::down : action::up);
            if(last_controller_axis_input[index] && std::get<1>(*last_controller_axis_input[index]) == dir) {
                return;
            }
            dispatch(dir);
            last_controller_axis_input[index] = std::make_tuple(controller, dir);
            last_controller_axis_input_time[index] = std::chrono::system_clock::now();
        }
    }
}
