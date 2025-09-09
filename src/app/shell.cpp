/* XMBShell, a console-like desktop shell
 * Copyright (C) 2025 - JCM
 *
 * This file (or substantial portions of it) is derived from XMBShell:
 *   https://github.com/JnCrMx/xmbshell
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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <ranges>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#include <version>

// gettext
#include <libintl.h>
#include <ctime>

module openxmb.app;

import i18n;
import spdlog;
import dreamrender;
import glm;
import vulkan_hpp;
import vma;
import sdl2;
import openxmb.config;
import openxmb.constants;
import openxmb.render;
import openxmb.debug;
import openxmb.utils;
import :startup_overlay;
import :message_overlay;

using namespace mfk::i18n::literals;

namespace app
{
    struct BlurConstants {
        int axis = 0;
        int size = 20;
    };

    shell::shell(window* window) : phase(window)
    {
    }

    shell::~shell()
    {
    }

    void shell::preload()
    {
        phase::preload();

        font_render = std::make_unique<font_renderer>(config::CONFIG.fontPath.string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        image_render = std::make_unique<image_renderer>(device, win->swapchainExtent, win->gpuFeatures);
        simple_render = std::make_unique<simple_renderer>(device, allocator, win->swapchainExtent, win->gpuFeatures);
        wave_render = std::make_unique<render::wave_renderer>(device, allocator, win->swapchainExtent);
        original_render = std::make_unique<render::original_renderer>(device, win->swapchainExtent);
        particles_render = std::make_unique<render::particles_renderer>(device, allocator, win->swapchainExtent);

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
            vk::UniqueShaderModule compShader = render::shaders::downsample::comp(device);
            vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
            vk::ComputePipelineCreateInfo info({}, shader, blurPipelineLayout.get());
            downsamplePipeline = device.createComputePipelineUnique({}, info).value;
            debugName(device, downsamplePipeline.get(), "Downsample Pipeline");
        }
        {
            vk::UniqueShaderModule compShader = render::shaders::upsample::comp(device);
            vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
            vk::ComputePipelineCreateInfo info({}, shader, blurPipelineLayout.get());
            upsamplePipeline = device.createComputePipelineUnique({}, info).value;
            debugName(device, upsamplePipeline.get(), "Upsample Pipeline");
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

            // Half/quarter-resolution ping-pong images for downsampled blur
            vk::Extent2D halfExtent{ std::max(1u, win->swapchainExtent.width/2u), std::max(1u, win->swapchainExtent.height/2u) };
            blurHalfSrc = std::make_unique<texture>(device, allocator,
                halfExtent, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, blurHalfSrc->image, "Blur Half Source");
            blurHalfDst = std::make_unique<texture>(device, allocator,
                halfExtent, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, blurHalfDst->image, "Blur Half Destination");

            vk::Extent2D quarterExtent{ std::max(1u, halfExtent.width/2u), std::max(1u, halfExtent.height/2u) };
            blurQuarterSrc = std::make_unique<texture>(device, allocator,
                quarterExtent, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, blurQuarterSrc->image, "Blur Quarter Source");
            blurQuarterDst = std::make_unique<texture>(device, allocator,
                quarterExtent, vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
            debugName(device, blurQuarterDst->image, "Blur Quarter Destination");
        }

        font_render->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff);
        image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        original_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        particles_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());

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
        config::CONFIG.addCallback("language", [this](const std::string&){
            reload_language();
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

    void shell::preload_fixed_components()
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
        auto load_sound_multi = [&](sdl::mix::unique_chunk& slot, std::initializer_list<const char*> names){
            for(const char* n : names) {
                auto p = (config::CONFIG.asset_directory/"sounds"/n).string();
                slot = sdl::mix::unique_chunk{sdl::mix::LoadWAV(p.c_str())};
                if(slot) { spdlog::debug("Loaded sound {}", p); return; }
            }
            // final fallback to ok.wav so UX isn't silent
            auto fallback = (config::CONFIG.asset_directory/"sounds/ok.wav").string();
            slot = sdl::mix::unique_chunk{sdl::mix::LoadWAV(fallback.c_str())};
            if(!slot) spdlog::debug("Failed to load any sound from list; last error: {}", sdl::mix::GetError());
        };
        load_sound_multi(question_sound, {"NSE.questionMark.wav", "NSE.questionMark.ogg"});
        load_sound_multi(confirm_sound,  {"NSE.ui.Confirm.wav",   "NSE.ui.Confirm.ogg"});
        load_sound_multi(cancel_sound,   {"NSE.ui.Cancel.wav",    "NSE.ui.Cancel.ogg"});
        load_sound_multi(back_sound,     {"NSE.clicker.Cancel.wav","NSE.clicker.Cancel.ogg"});

        reload_button_icons();

        // Push startup splash overlay (plays jingle, fades text)
        emplace_overlay<app::startup_overlay>();
    }

    void shell::prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews)
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
        // Extra descriptor sets for downsample/half/quarter blur/upsample chain
        {
            vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 12);
            vk::DescriptorPoolCreateInfo pool_info({}, 6, size);
            blurExtraDescriptorPool = device.createDescriptorPoolUnique(pool_info);

            std::array<vk::DescriptorSetLayout,6> layouts{
                blurDescriptorSetLayout.get(), // downsample full->half
                blurDescriptorSetLayout.get(), // half blur
                blurDescriptorSetLayout.get(), // upsample half->full
                blurDescriptorSetLayout.get(), // downsample half->quarter
                blurDescriptorSetLayout.get(), // quarter blur
                blurDescriptorSetLayout.get()  // upsample quarter->half
            };
            vk::DescriptorSetAllocateInfo alloc_info(blurExtraDescriptorPool.get(), layouts);
            auto sets = device.allocateDescriptorSets(alloc_info);
            downsampleSet  = sets[0];   // full -> half
            halfBlurSet    = sets[1];   // half -> half
            upsampleSet    = sets[2];   // half -> full
            downsample2Set = sets[3];   // half -> quarter
            quarterBlurSet = sets[4];   // quarter -> quarter (reused for both blur passes)
            upsample2Set   = sets[5];   // quarter -> half

            std::array<vk::WriteDescriptorSet,6> writes{};
            std::array<vk::DescriptorImageInfo,12> infos{};
            // Downsample: input full src -> output half src
            infos[0] = vk::DescriptorImageInfo({}, blurImageSrc->imageView.get(), vk::ImageLayout::eGeneral);
            infos[1] = vk::DescriptorImageInfo({}, blurHalfSrc->imageView.get(), vk::ImageLayout::eGeneral);
            writes[0] = vk::WriteDescriptorSet(downsampleSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &infos[0]);
            // Half blur: input half src -> output half dst
            infos[2] = vk::DescriptorImageInfo({}, blurHalfSrc->imageView.get(), vk::ImageLayout::eGeneral);
            infos[3] = vk::DescriptorImageInfo({}, blurHalfDst->imageView.get(), vk::ImageLayout::eGeneral);
            writes[1] = vk::WriteDescriptorSet(halfBlurSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &infos[2]);
            // Upsample: input half dst -> output full dst
            infos[4] = vk::DescriptorImageInfo({}, blurHalfDst->imageView.get(), vk::ImageLayout::eGeneral);
            infos[5] = vk::DescriptorImageInfo({}, blurImageDst->imageView.get(), vk::ImageLayout::eGeneral);
            writes[2] = vk::WriteDescriptorSet(upsampleSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &infos[4]);
            // Downsample2: input half src -> output quarter src
            infos[6] = vk::DescriptorImageInfo({}, blurHalfSrc->imageView.get(), vk::ImageLayout::eGeneral);
            infos[7] = vk::DescriptorImageInfo({}, blurQuarterSrc->imageView.get(), vk::ImageLayout::eGeneral);
            writes[3] = vk::WriteDescriptorSet(downsample2Set, 0, 0, 2, vk::DescriptorType::eStorageImage, &infos[6]);
            // Quarter blur: input quarter src -> output quarter dst
            infos[8] = vk::DescriptorImageInfo({}, blurQuarterSrc->imageView.get(), vk::ImageLayout::eGeneral);
            infos[9] = vk::DescriptorImageInfo({}, blurQuarterDst->imageView.get(), vk::ImageLayout::eGeneral);
            writes[4] = vk::WriteDescriptorSet(quarterBlurSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &infos[8]);
            // Upsample2: input quarter dst -> output half dst
            infos[10] = vk::DescriptorImageInfo({}, blurQuarterDst->imageView.get(), vk::ImageLayout::eGeneral);
            infos[11] = vk::DescriptorImageInfo({}, blurHalfDst->imageView.get(), vk::ImageLayout::eGeneral);
            writes[5] = vk::WriteDescriptorSet(upsample2Set, 0, 0, 2, vk::DescriptorType::eStorageImage, &infos[10]);
            device.updateDescriptorSets(writes, {});
        }
        this->swapchainImages = swapchainImages;

        std::vector<vk::DescriptorImageInfo> imageInfos(2*imageCount);
        std::vector<vk::WriteDescriptorSet> writes(imageCount);

        framebuffers.clear();
        backgroundFramebuffers.clear();
        backgroundResolve.clear();
        backgroundResolve.reserve(imageCount);
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
                // Create per-frame background resolve target (single-sample, sampled + transfer)
                auto tex = std::make_unique<texture>(device, allocator,
                    win->swapchainExtent,
                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                    win->swapchainFormat.format, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
                debugName(device, tex->image, ("Background Resolve #"+std::to_string(i)).c_str());
                vk::ImageView bgView = tex->imageView.get();
                backgroundResolve.push_back(std::move(tex));

                std::array<vk::ImageView, 2> attachments = {renderImage->imageView.get(), bgView};
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
        original_render->prepare(swapchainViews.size());
        particles_render->prepare(swapchainViews.size());
    }

    void shell::reload_language() {
        // Apply LANGUAGE env and rebind gettext domain
        try {
            const std::string& lang = config::CONFIG.language;
            if(lang.empty() || lang == "auto") {
                // Let system locale decide
#if __linux__ || defined(__APPLE__)
                unsetenv("LANGUAGE");
                unsetenv("LC_MESSAGES");
                unsetenv("LC_ALL");
#endif
            } else {
#if __linux__ || defined(__APPLE__)
                setenv("LANGUAGE", lang.c_str(), 1);
                // Be more explicit so gettext reloads catalogs reliably
                setenv("LC_MESSAGES", lang.c_str(), 1);
                setenv("LC_ALL", lang.c_str(), 1);
#endif
            }
            // Reinitialize locale from environment or set to specific language
            if(lang.empty() || lang == "auto") {
                setlocale(LC_ALL, "");
            } else {
                // Try direct set first (may fail if locale not generated); fallback to env-driven
                if(!setlocale(LC_ALL, lang.c_str())) {
                    setlocale(LC_ALL, "");
                }
            }
            bindtextdomain(constants::name, config::CONFIG.locale_directory.string().c_str());
            bind_textdomain_codeset(constants::name, "UTF-8");
            textdomain(constants::name);
            spdlog::info("Language set to '{}'; reloading menus", lang);

            // Rebuild main menu to refresh translated strings
            menu = app::main_menu(this);
            menu.preload(device, allocator, *loader);
        } catch(const std::exception& e) {
            spdlog::error("reload_language failed: {}", e.what());
        }
    }

    void shell::render(int frame, vk::Semaphore imageAvailable, vk::Semaphore renderFinished, vk::Fence fence)
    {
        tick();

        vk::CommandBuffer commandBuffer = commandBuffers[frame];
        auto now = std::chrono::steady_clock::now();

        commandBuffer.begin(vk::CommandBufferBeginInfo());
        for(auto& overlay : std::views::reverse(overlays)) {
            overlay->prerender(commandBuffer, frame, this);
        }
        {
            // Compute PS3‑style theme colour (Original or custom) and time-of-day brightness
            glm::vec3 baseThemeColour = config::CONFIG.themeOriginalColour ? utils::xmb_dynamic_colour(std::chrono::system_clock::now())
                                                                          : config::CONFIG.themeCustomColour;
            float brightness = 1.0f;
            {
                std::time_t tnow = std::time(nullptr);
                std::tm lt{}; 
#if defined(_WIN32)
                localtime_s(&lt, &tnow);
#else
                localtime_r(&tnow, &lt);
#endif
                float minuteFrac = static_cast<float>(lt.tm_min) / 60.0f;
                brightness = utils::xmb_hour_brightness(lt.tm_hour, minuteFrac);
            }
            // Always tint the background clear colour (for both Original and Classic)
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            {
                glm::vec3 c = baseThemeColour * brightness;
                color = vk::ClearColorValue(std::array<float, 4>{ c.r, c.g, c.b, 1.0f });
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
                if(config::CONFIG.backgroundType == config::config::background_type::original) {
                    // Render original-style background only (no retro wave renderer here)
                    float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - shader_time_zero).count();
                    original_render->render(commandBuffer, frame, backgroundRenderPass.get(), baseThemeColour, brightness, seconds);
                    // Particle pass on top (additive)
                    particles_render->render(commandBuffer, frame, backgroundRenderPass.get(), baseThemeColour, brightness, seconds);
                }
                else if(config::CONFIG.backgroundType == config::config::background_type::wave) {
                    wave_render->waveColor = baseThemeColour; // PS3 look: wave uses base, brightness on background only
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
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
                vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {},
                {
                    // Make resolved background available for transfer
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
                        vk::AccessFlagBits::eTransferRead,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        backgroundResolve[frame]->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    // Prepare blur source for copy destination
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            // Copy the current swapchain image into our working src image (no scaling needed, copy is cheaper than blit)
            {
                // Blit allows format conversion (e.g., B8G8R8A8 -> R16G16B16A16)
                vk::ImageBlit blit{
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height), 1} },
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(blurImageSrc->width), static_cast<int>(blurImageSrc->height), 1} }
                };
                commandBuffer.blitImage(backgroundResolve[frame]->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurImageSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                        blit, vk::Filter::eLinear);
            }

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

            // Decide path: full-res separable Gaussian for small radius; downsampled pipeline for larger
            BlurConstants constants{};
            const int targetRadius = static_cast<int>(20 * (blur_background ? blur_background_progress : (1.0 - blur_background_progress)));

            if(targetRadius <= 4) {
                int groupCountX = static_cast<int>(std::ceil(blurImageSrc->width/16.0));
                int groupCountY = static_cast<int>(std::ceil(blurImageSrc->height/16.0));

                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurDescriptorSets[frame]}, {});
                constants.size = targetRadius;
                // Pass 1: horizontal blur into blurImageDst
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(groupCountX, groupCountY, 1);

                // Prepare to copy blurImageDst -> blurImageSrc (ping-pong)
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurImageSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );
                // Copy into src for the second pass to read
                vk::ImageCopy ic{};
                ic.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                ic.setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                ic.setExtent(vk::Extent3D{
                    static_cast<uint32_t>(blurImageSrc->width),
                    static_cast<uint32_t>(blurImageSrc->height),
                    1u
                });
                commandBuffer.copyImage(blurImageDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurImageSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                        ic);

                // Prepare images for second compute pass (vertical): src=GENERAL (read), dst=GENERAL (write)
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
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );

                // Pass 2: vertical blur into blurImageDst
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
            } else if(targetRadius <= 8) {
                // Downsample to half-res, blur there, upsample back
                int halfX = static_cast<int>(std::ceil(blurHalfSrc->width/16.0));
                int halfY = static_cast<int>(std::ceil(blurHalfSrc->height/16.0));

                // Ensure half images are in GENERAL layout
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass A: downsample full->half into blurHalfSrc
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {downsampleSet}, {});
                commandBuffer.dispatch(halfX, halfY, 1);

                // Prepare halfSrc for read, halfDst for write
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass B: horizontal blur (half) into blurHalfDst
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {halfBlurSet}, {});
                constants.size = std::max(1, targetRadius / 2);
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(halfX, halfY, 1);

                // Ping-pong: copy halfDst -> halfSrc
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );
                {
                    vk::ImageCopy ic2{};
                    ic2.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                    ic2.setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                    ic2.setExtent(vk::Extent3D{
                        static_cast<uint32_t>(blurHalfSrc->width),
                        static_cast<uint32_t>(blurHalfSrc->height),
                        1u
                    });
                    commandBuffer.copyImage(blurHalfDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                            blurHalfSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                            ic2);
                }
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass C: vertical blur (half) into blurHalfDst
                constants.axis = 1;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(halfX, halfY, 1);

                // Prepare for upsample: halfDst read, fullDst write
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass D: upsample half -> full into blurImageDst
                int fullX = static_cast<int>(std::ceil(blurImageDst->width/16.0));
                int fullY = static_cast<int>(std::ceil(blurImageDst->height/16.0));
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {upsampleSet}, {});
                commandBuffer.dispatch(fullX, fullY, 1);

                // Transition for sampling
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );
            } else {
                // Two-level: Full -> Half -> Quarter, blur at quarter, then upsample back
                int halfX = static_cast<int>(std::ceil(blurHalfSrc->width/16.0));
                int halfY = static_cast<int>(std::ceil(blurHalfSrc->height/16.0));
                int qX = static_cast<int>(std::ceil(blurQuarterSrc->width/16.0));
                int qY = static_cast<int>(std::ceil(blurQuarterSrc->height/16.0));

                // A: downsample full->half
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {downsampleSet}, {});
                commandBuffer.dispatch(halfX, halfY, 1);
                // make halfSrc readable
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {}, { vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurHalfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)) });

                // B: downsample half->quarter
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {downsample2Set}, {});
                commandBuffer.dispatch(qX, qY, 1);
                // Make quarterSrc read, quarterDst write
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // C: horizontal blur (quarter) into quarterDst
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {quarterBlurSet}, {});
                constants.size = std::max(1, targetRadius / 4);
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(qX, qY, 1);

                // Ping-pong quarter: copy dst->src
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
                {
                    vk::ImageCopy ic{};
                    ic.setSrcSubresource({vk::ImageAspectFlagBits::eColor,0,0,1});
                    ic.setDstSubresource({vk::ImageAspectFlagBits::eColor,0,0,1});
                    ic.setExtent(vk::Extent3D{ static_cast<uint32_t>(blurQuarterSrc->width), static_cast<uint32_t>(blurQuarterSrc->height), 1u });
                    commandBuffer.copyImage(blurQuarterDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                            blurQuarterSrc->image, vk::ImageLayout::eTransferDstOptimal,
                                            ic);
                }
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // D: vertical blur (quarter) into quarterDst
                constants.axis = 1;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(qX, qY, 1);

                // E: upsample quarter -> half (into halfDst) using compute
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurQuarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurHalfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {upsample2Set}, {});
                commandBuffer.dispatch(halfX, halfY, 1);

                // G: upsample half -> full into blurImageDst
                int fullX = static_cast<int>(std::ceil(blurImageDst->width/16.0));
                int fullY = static_cast<int>(std::ceil(blurImageDst->height/16.0));
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {upsampleSet}, {});
                commandBuffer.dispatch(fullX, fullY, 1);

                // Transition for sampling
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
            }
        }
        else {
            commandBuffer.pipelineBarrier(
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
                vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {},
                {
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
                        vk::AccessFlagBits::eTransferRead,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        backgroundResolve[frame]->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurImageDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            // No blur case: copy swapchain into blurImageDst to sample without extra filtering
            {
                vk::ImageBlit blit{
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height), 1} },
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(blurImageDst->width), static_cast<int>(blurImageDst->height), 1} }
                };
                commandBuffer.blitImage(backgroundResolve[frame]->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurImageDst->image, vk::ImageLayout::eTransferDstOptimal,
                                        blit, vk::Filter::eLinear);
            }

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
            // No manual transition needed; attachment initial/final layouts handle swapchain transitions in render pass
            commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(shellRenderPass.get(), framebuffers[frame].get(),
                vk::Rect2D({0, 0}, win->swapchainExtent), color), vk::SubpassContents::eInline);
            vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(win->swapchainExtent.width), static_cast<float>(win->swapchainExtent.height), 0.0f, 1.0f);
            vk::Rect2D scissor({0,0}, win->swapchainExtent);
            commandBuffer.setViewport(0, viewport);
            commandBuffer.setScissor(0, scissor);

            image_render->renderImageSized(commandBuffer, frame, shellRenderPass.get(), blurImageDst->imageView.get(),
                0.0f, 0.0f, static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height));

            gui_renderer ctx(commandBuffer, frame, shellRenderPass.get(), win->swapchainExtent, font_render.get(), image_render.get(), simple_render.get());
            // Interface/FX debug overlays: draw font atlas for verification
            if(openxmb::debug::interfacefx_debug) {
                const dreamrender::texture* atlas = font_render->get_atlas();
                if(atlas && atlas->loaded) {
                    if(!openxmb::debug::interfacefx_debug_once_atlas_logged) {
                        spdlog::info("[InterfaceFXDEBUG] Font atlas: {}x{}", atlas->width, atlas->height);
                        openxmb::debug::interfacefx_debug_once_atlas_logged = true;
                    }
                    int dw = std::min(256, atlas->width);
                    int dh = std::min(256, atlas->height);
                    ctx.draw_image_sized(*atlas, 0.02f, 0.02f, dw, dh);
                }
                // Text probe: ensure font rendering path emits visible geometry
                ctx.draw_text("InterfaceFX TEXT PROBE: The quick brown fox jumps over the lazy dog.", 0.02f, 0.14f, 0.06f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
            }
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

    void shell::render_gui(gui_renderer& renderer) {
        bool render_menu = true;
        unsigned int overlay_begin = 0;
        bool has_overlay = !overlays.empty();
        bool top_is_message = has_overlay && (dynamic_cast<app::message_overlay*>(overlays.back().get()) != nullptr);

        if(has_overlay) {
            for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
                if(overlays[i]->is_opaque()) {
                    overlay_begin = i;
                    render_menu = false;
                    break;
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        // TODO: somehow fix this.... god this is gonna be a huge mess
        double overlay_progress = utils::progress(now, overlay_fade_time, overlay_transition_duration);
        double dir_progress = overlay_fade_direction == transition_direction::in ? overlay_progress : 1.0 - overlay_progress;
        bool overlay_transition = overlay_progress < 1.0;
        // Consider fade-out of a message overlay (stored as old_overlay) as part of the transition too
        bool fading_out_message = (!has_overlay && overlay_transition && old_overlay && (dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr));
        // If a message overlay is appearing/disappearing, still render menu during transition
        bool allow_menu = render_menu || ((top_is_message || fading_out_message) && overlay_transition);
        if(allow_menu){
            if(overlay_transition || has_overlay || fading_out_message) {
                // Fade UI to transparency: scale RGB and A together by (1 - progress)
                float s = 1.0f - static_cast<float>(dir_progress);
                renderer.push_color(glm::vec4(s, s, s, s));
            }
            // Quick zoom via gui_renderer helper so viewport/scissor are applied per draw
            const bool pushed_zoom = (top_is_message || fading_out_message);
            if(pushed_zoom) {
                float scale = static_cast<float>(glm::mix(1.0, 0.85, dir_progress));
                renderer.push_zoom(scale);
            }
            // Render the entire XMB UI (menu + time + news) within the zoom scope
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
            if(pushed_zoom) renderer.pop_zoom();
            if(overlay_transition || has_overlay || fading_out_message) {
                renderer.pop_color();
            }

        }

        for(unsigned int i=overlay_begin; i < overlays.size(); i++) {
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
            // Fade-out finished; if it was a message overlay, drop background blur now
            if(dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr) {
                set_blur_background(false);
            }
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

    void shell::reload_background() {
        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
    }
    void shell::reload_button_icons() {
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
    std::string shell::get_controller_type() const {
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

    void shell::tick() {
        if(background_only) {
            return;
        }

        for(unsigned int i=0; i<2; i++) {
            if(last_controller_axis_input[i]) {
                auto time_since_input = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_controller_axis_input_time[i]);
                if(time_since_input > controller_axis_input_duration) {
                    auto [controller, dir] = *last_controller_axis_input[i];
                    dispatch(dir);
                    last_controller_axis_input_time[i] = std::chrono::steady_clock::now();
                }
            }
        }
        if(last_controller_button_input) {
            auto time_since_input = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_controller_button_input_time);
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

    void shell::dispatch(action action) {
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
    void shell::handle(result result) {
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
        if(result & result::confirm_sound) {
            if(confirm_sound && sdl::mix::PlayChannel(-1, confirm_sound.get(), 0) == -1) {
                spdlog::debug("PlayChannel(confirm): {}", sdl::mix::GetError());
            }
        }
        if(result & result::cancel_sound) {
            if(cancel_sound && sdl::mix::PlayChannel(-1, cancel_sound.get(), 0) == -1) {
                spdlog::debug("PlayChannel(cancel): {}", sdl::mix::GetError());
            }
        }
        if(result & result::back_sound) {
            if(back_sound && sdl::mix::PlayChannel(-1, back_sound.get(), 0) == -1) {
                spdlog::debug("PlayChannel(back): {}", sdl::mix::GetError());
            }
        }
    }

    void shell::key_up(sdl::Keysym key)
    {
        spdlog::trace("Key up: {}", key.sym);
    }
    void shell::key_down(sdl::Keysym key)
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

    void shell::add_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void shell::remove_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void shell::button_down(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button down: {}", fmt::underlying(button));
        last_controller_button_input = std::make_tuple(controller, button);
        last_controller_button_input_time = std::chrono::steady_clock::now();

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
    void shell::button_up(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button up: {}", fmt::underlying(button));
        last_controller_button_input = std::nullopt;
    }
    void shell::axis_motion(sdl::GameController* controller, sdl::GameControllerAxis axis, int16_t value)
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
                last_controller_axis_input_time[index] = std::chrono::steady_clock::now();
                return;
            }
            action dir = axis == sdl::GameControllerAxisValues::LEFTX  ? (value > 0 ? action::right : action::left)
                : (value > 0 ? action::down : action::up);
            if(last_controller_axis_input[index] && std::get<1>(*last_controller_axis_input[index]) == dir) {
                return;
            }
            dispatch(dir);
            last_controller_axis_input[index] = std::make_tuple(controller, dir);
            last_controller_axis_input_time[index] = std::chrono::steady_clock::now();
        }
    }
}
