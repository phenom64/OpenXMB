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
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <version>

// gettext
#include <libintl.h>
#include <ctime>
#include <SDL2/SDL_mouse.h>

#include <glm/vec2.hpp>

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
    namespace {
        events::logical_controller_button to_logical_button(sdl::GameControllerButton button)
        {
            return static_cast<events::logical_controller_button>(std::to_underlying(button));
        }

        events::logical_mouse_button to_logical_mouse_button(int button)
        {
            switch(button) {
                case SDL_BUTTON_LEFT:
                    return events::logical_mouse_button::left;
                case SDL_BUTTON_MIDDLE:
                    return events::logical_mouse_button::middle;
                case SDL_BUTTON_RIGHT:
                    return events::logical_mouse_button::right;
                case SDL_BUTTON_X1:
                    return events::logical_mouse_button::x1;
                case SDL_BUTTON_X2:
                    return events::logical_mouse_button::x2;
                default:
                    return events::logical_mouse_button::left;
            }
        }

        std::filesystem::path existing_asset_or_fallback(std::filesystem::path path, std::filesystem::path fallback)
        {
            std::error_code ec;
            if(std::filesystem::exists(path, ec) && !ec) {
                return path;
            }
            ec.clear();
            if(std::filesystem::exists(fallback, ec) && !ec) {
                return fallback;
            }
            return path;
        }
    }

    struct BlurConstants {
        int axis = 0;
        int size = 20;
    };

    void component::render_controller_buttons(app::shell* xmb, dreamrender::gui_renderer& renderer, float x, float y, std::span<const std::pair<::action, std::string_view>> buttons) const
    {
        xmb->render_controller_buttons(renderer, x, y, buttons);
    }

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

        add_task(font_render->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff));
        image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        original_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());

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

        if(!win->config.headless) {
            ok_sound = sdl::mix::unique_chunk{sdl::mix::LoadWAV((config::CONFIG.asset_directory/"sounds/ok.wav").string().c_str())};
            if(!ok_sound) {
                spdlog::warn("sdl::mix::LoadWAV: {}", sdl::mix::GetError());
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
        }

        reload_button_icons();

        cursorTexture = std::make_unique<texture>(device, allocator);
        loader->loadTexture(cursorTexture.get(), existing_asset_or_fallback(
            config::CONFIG.asset_directory/"icons/icon_cursor.png",
            config::CONFIG.asset_directory/"icons/icon_category_settings.png"));

        // Push startup splash overlay (plays jingle, fades text)
        if(!win->config.headless) {
            emplace_overlay<app::startup_overlay>();
        }
    }

    void shell::prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews)
    {
        phase::prepare(swapchainImages, swapchainViews);

        const auto imageCount = static_cast<std::uint32_t>(swapchainImages.size());
        this->swapchainImages = swapchainImages;

        framebuffers.clear();
        backgroundFramebuffers.clear();
        blurDescriptorSets.clear();
        blurDescriptorPool.reset();
        blurExtraDescriptorPool.reset();
        renderImages.clear();
        backgroundResolve.clear();
        blurFrames.clear();

        if(imageCount == 0) {
            font_render->prepare(0);
            image_render->prepare(0);
            simple_render->prepare(0);
            wave_render->prepare(0);
            original_render->prepare(0);
            return;
        }

        const auto extent = win->swapchainExtent;
        const vk::Extent2D halfExtent{
            std::max(1u, extent.width/2u),
            std::max(1u, extent.height/2u)
        };
        const vk::Extent2D quarterExtent{
            std::max(1u, halfExtent.width/2u),
            std::max(1u, halfExtent.height/2u)
        };
        const auto blurUsage = vk::ImageUsageFlagBits::eStorage
            | vk::ImageUsageFlagBits::eTransferSrc
            | vk::ImageUsageFlagBits::eTransferDst
            | vk::ImageUsageFlagBits::eSampled;
        auto makeBlurTexture = [&](vk::Extent2D imageExtent) {
            return std::make_unique<texture>(device, allocator,
                imageExtent, blurUsage,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1,
                false, vk::ImageAspectFlagBits::eColor);
        };

        framebuffers.reserve(imageCount);
        backgroundFramebuffers.reserve(imageCount);
        renderImages.reserve(imageCount);
        backgroundResolve.reserve(imageCount);
        blurFrames.reserve(imageCount);

        for(std::uint32_t i=0; i<imageCount; i++)
        {
            debugName(device, swapchainImages[i], "Swapchain Image #"+std::to_string(i));
            {
                renderImages.push_back(std::make_unique<texture>(device, allocator,
                    extent, vk::ImageUsageFlagBits::eColorAttachment,
                    win->swapchainFormat.format, win->config.sampleCount,
                    false, vk::ImageAspectFlagBits::eColor));
                debugName(device, renderImages.back()->image, "Shell Render Image #"+std::to_string(i));
            }
            {
                // Create per-frame background resolve target (single-sample, sampled + transfer)
                auto tex = std::make_unique<texture>(device, allocator,
                    extent,
                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
                    win->swapchainFormat.format, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor);
                debugName(device, tex->image, ("Background Resolve #"+std::to_string(i)).c_str());
                backgroundResolve.push_back(std::move(tex));
            }
            {
                blurFrames.emplace_back();
                auto& blurFrame = blurFrames.back();

                blurFrame.fullSrc = makeBlurTexture(extent);
                debugName(device, blurFrame.fullSrc->image, "Blur Image Source #"+std::to_string(i));

                blurFrame.fullDst = makeBlurTexture(extent);
                debugName(device, blurFrame.fullDst->image, "Blur Image Destination #"+std::to_string(i));

                blurFrame.halfSrc = makeBlurTexture(halfExtent);
                debugName(device, blurFrame.halfSrc->image, "Blur Half Source #"+std::to_string(i));

                blurFrame.halfDst = makeBlurTexture(halfExtent);
                debugName(device, blurFrame.halfDst->image, "Blur Half Destination #"+std::to_string(i));

                blurFrame.quarterSrc = makeBlurTexture(quarterExtent);
                debugName(device, blurFrame.quarterSrc->image, "Blur Quarter Source #"+std::to_string(i));

                blurFrame.quarterDst = makeBlurTexture(quarterExtent);
                debugName(device, blurFrame.quarterDst->image, "Blur Quarter Destination #"+std::to_string(i));
            }
            {
                std::array<vk::ImageView, 2> attachments = {renderImages[i]->imageView.get(), swapchainViews[i]};
                vk::FramebufferCreateInfo framebuffer_info({}, shellRenderPass.get(), attachments,
                    extent.width, extent.height, 1);
                framebuffers.push_back(device.createFramebufferUnique(framebuffer_info));
                debugName(device, framebuffers.back().get(), "XMB Shell Framebuffer #"+std::to_string(i));
            }
            {
                std::array<vk::ImageView, 2> attachments = {renderImages[i]->imageView.get(), backgroundResolve[i]->imageView.get()};
                vk::FramebufferCreateInfo framebuffer_info({}, backgroundRenderPass.get(), attachments,
                    extent.width, extent.height, 1);
                backgroundFramebuffers.push_back(device.createFramebufferUnique(framebuffer_info));
                debugName(device, backgroundFramebuffers.back().get(), "XMB Shell Background Framebuffer #"+std::to_string(i));
            }
        }

        {
            vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 2*imageCount);
            vk::DescriptorPoolCreateInfo pool_info({}, imageCount, size);
            blurDescriptorPool = device.createDescriptorPoolUnique(pool_info);

            std::vector<vk::DescriptorSetLayout> layouts(imageCount, blurDescriptorSetLayout.get());
            vk::DescriptorSetAllocateInfo alloc_info(blurDescriptorPool.get(), layouts);
            blurDescriptorSets = device.allocateDescriptorSets(alloc_info);
        }
        {
            vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 12*imageCount);
            vk::DescriptorPoolCreateInfo pool_info({}, 6*imageCount, size);
            blurExtraDescriptorPool = device.createDescriptorPoolUnique(pool_info);

            std::vector<vk::DescriptorSetLayout> layouts(6*imageCount, blurDescriptorSetLayout.get());
            vk::DescriptorSetAllocateInfo alloc_info(blurExtraDescriptorPool.get(), layouts);
            auto sets = device.allocateDescriptorSets(alloc_info);

            for(std::uint32_t i=0; i<imageCount; i++) {
                auto& blurFrame = blurFrames[i];
                const auto setBase = 6*i;
                blurFrame.downsampleSet  = sets[setBase + 0]; // full -> half
                blurFrame.halfBlurSet    = sets[setBase + 1]; // half -> half
                blurFrame.upsampleSet    = sets[setBase + 2]; // half -> full
                blurFrame.downsample2Set = sets[setBase + 3]; // half -> quarter
                blurFrame.quarterBlurSet = sets[setBase + 4]; // quarter -> quarter
                blurFrame.upsample2Set   = sets[setBase + 5]; // quarter -> half
            }
        }

        std::vector<vk::DescriptorImageInfo> imageInfos(2*imageCount);
        std::vector<vk::WriteDescriptorSet> writes(imageCount);
        std::vector<vk::DescriptorImageInfo> extraInfos(12*imageCount);
        std::vector<vk::WriteDescriptorSet> extraWrites(6*imageCount);

        for(std::uint32_t i=0; i<imageCount; i++)
        {
            auto& blurFrame = blurFrames[i];
            const auto imageInfoBase = 2*i;
            imageInfos[imageInfoBase] = vk::DescriptorImageInfo({}, blurFrame.fullSrc->imageView.get(), vk::ImageLayout::eGeneral);
            imageInfos[imageInfoBase+1] = vk::DescriptorImageInfo({}, blurFrame.fullDst->imageView.get(), vk::ImageLayout::eGeneral);
            writes[i] = vk::WriteDescriptorSet(blurDescriptorSets[i], 0, 0, 2, vk::DescriptorType::eStorageImage, &imageInfos[imageInfoBase]);

            const auto extraInfoBase = 12*i;
            const auto extraWriteBase = 6*i;
            // Downsample: input full src -> output half src
            extraInfos[extraInfoBase+0] = vk::DescriptorImageInfo({}, blurFrame.fullSrc->imageView.get(), vk::ImageLayout::eGeneral);
            extraInfos[extraInfoBase+1] = vk::DescriptorImageInfo({}, blurFrame.halfSrc->imageView.get(), vk::ImageLayout::eGeneral);
            extraWrites[extraWriteBase+0] = vk::WriteDescriptorSet(blurFrame.downsampleSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &extraInfos[extraInfoBase+0]);
            // Half blur: input half src -> output half dst
            extraInfos[extraInfoBase+2] = vk::DescriptorImageInfo({}, blurFrame.halfSrc->imageView.get(), vk::ImageLayout::eGeneral);
            extraInfos[extraInfoBase+3] = vk::DescriptorImageInfo({}, blurFrame.halfDst->imageView.get(), vk::ImageLayout::eGeneral);
            extraWrites[extraWriteBase+1] = vk::WriteDescriptorSet(blurFrame.halfBlurSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &extraInfos[extraInfoBase+2]);
            // Upsample: input half dst -> output full dst
            extraInfos[extraInfoBase+4] = vk::DescriptorImageInfo({}, blurFrame.halfDst->imageView.get(), vk::ImageLayout::eGeneral);
            extraInfos[extraInfoBase+5] = vk::DescriptorImageInfo({}, blurFrame.fullDst->imageView.get(), vk::ImageLayout::eGeneral);
            extraWrites[extraWriteBase+2] = vk::WriteDescriptorSet(blurFrame.upsampleSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &extraInfos[extraInfoBase+4]);
            // Downsample2: input half src -> output quarter src
            extraInfos[extraInfoBase+6] = vk::DescriptorImageInfo({}, blurFrame.halfSrc->imageView.get(), vk::ImageLayout::eGeneral);
            extraInfos[extraInfoBase+7] = vk::DescriptorImageInfo({}, blurFrame.quarterSrc->imageView.get(), vk::ImageLayout::eGeneral);
            extraWrites[extraWriteBase+3] = vk::WriteDescriptorSet(blurFrame.downsample2Set, 0, 0, 2, vk::DescriptorType::eStorageImage, &extraInfos[extraInfoBase+6]);
            // Quarter blur: input quarter src -> output quarter dst
            extraInfos[extraInfoBase+8] = vk::DescriptorImageInfo({}, blurFrame.quarterSrc->imageView.get(), vk::ImageLayout::eGeneral);
            extraInfos[extraInfoBase+9] = vk::DescriptorImageInfo({}, blurFrame.quarterDst->imageView.get(), vk::ImageLayout::eGeneral);
            extraWrites[extraWriteBase+4] = vk::WriteDescriptorSet(blurFrame.quarterBlurSet, 0, 0, 2, vk::DescriptorType::eStorageImage, &extraInfos[extraInfoBase+8]);
            // Upsample2: input quarter dst -> output half dst
            extraInfos[extraInfoBase+10] = vk::DescriptorImageInfo({}, blurFrame.quarterDst->imageView.get(), vk::ImageLayout::eGeneral);
            extraInfos[extraInfoBase+11] = vk::DescriptorImageInfo({}, blurFrame.halfDst->imageView.get(), vk::ImageLayout::eGeneral);
            extraWrites[extraWriteBase+5] = vk::WriteDescriptorSet(blurFrame.upsample2Set, 0, 0, 2, vk::DescriptorType::eStorageImage, &extraInfos[extraInfoBase+10]);
        }
        device.updateDescriptorSets(writes, {});
        device.updateDescriptorSets(extraWrites, {});

        font_render->prepare(swapchainViews.size());
        image_render->prepare(swapchainViews.size());
        simple_render->prepare(swapchainViews.size());
        wave_render->prepare(swapchainViews.size());
        original_render->prepare(swapchainViews.size());
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
        auto& blurFrame = blurFrames[frame];
        auto now = std::chrono::steady_clock::now();

        commandBuffer.begin(vk::CommandBufferBeginInfo());
        for(auto& overlay : std::views::reverse(overlays)) {
            overlay->prerender(commandBuffer, frame, this);
        }
        {
            auto themeColour = utils::xmb_resolve_theme_colour(std::chrono::system_clock::now());
            glm::vec3 baseThemeColour = themeColour.base_colour;
            float brightness = themeColour.brightness;
            // Always tint the background clear colour (for both Original and Classic)
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            {
                glm::vec3 c = themeColour.shaded_colour;
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
                    float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - shader_time_zero).count();
                    original_render->render(commandBuffer, frame, backgroundRenderPass.get(), baseThemeColour, brightness, seconds);
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
        const bool use_blur_background = blur_background || blur_background_progress < 1.0;
        vk::ImageView compositedBackgroundView = backgroundResolve[frame]->imageView.get();
        if(use_blur_background) {
            compositedBackgroundView = blurFrame.fullDst->imageView.get();
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
                        blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
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
                    { vk::Offset3D{0,0,0}, vk::Offset3D{static_cast<int>(blurFrame.fullSrc->width), static_cast<int>(blurFrame.fullSrc->height), 1} }
                };
                commandBuffer.blitImage(backgroundResolve[frame]->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurFrame.fullSrc->image, vk::ImageLayout::eTransferDstOptimal,
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
                        blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eShaderWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    ),
                }
            );

            // Decide path: full-res separable Gaussian for small radius; downsampled pipeline for larger
            BlurConstants constants{};
            const int targetRadius = static_cast<int>(20 * (blur_background ? blur_background_progress : (1.0 - blur_background_progress)));

            if(targetRadius <= 4) {
                int groupCountX = static_cast<int>(std::ceil(blurFrame.fullSrc->width/16.0));
                int groupCountY = static_cast<int>(std::ceil(blurFrame.fullSrc->height/16.0));

                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurDescriptorSets[frame]}, {});
                constants.size = targetRadius;
                // Pass 1: horizontal blur into blurFrame.fullDst
                constants.axis = 0;
                commandBuffer.pushConstants(blurPipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
                commandBuffer.dispatch(groupCountX, groupCountY, 1);

                // Prepare to copy blurFrame.fullDst -> blurFrame.fullSrc (ping-pong)
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );
                // Copy into src for the second pass to read
                vk::ImageCopy ic{};
                ic.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                ic.setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                ic.setExtent(vk::Extent3D{
                    static_cast<uint32_t>(blurFrame.fullSrc->width),
                    static_cast<uint32_t>(blurFrame.fullSrc->height),
                    1u
                });
                commandBuffer.copyImage(blurFrame.fullDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                        blurFrame.fullSrc->image, vk::ImageLayout::eTransferDstOptimal,
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
                            blurFrame.fullSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );

                // Pass 2: vertical blur into blurFrame.fullDst
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
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                    }
                );
            } else if(targetRadius <= 8) {
                // Downsample to half-res, blur there, upsample back
                int halfX = static_cast<int>(std::ceil(blurFrame.halfSrc->width/16.0));
                int halfY = static_cast<int>(std::ceil(blurFrame.halfSrc->height/16.0));

                // Ensure half images are in GENERAL layout before their first storage use.
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass A: downsample full->half into blurFrame.halfSrc
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.downsampleSet}, {});
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
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass B: horizontal blur (half) into blurFrame.halfDst
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.halfBlurSet}, {});
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
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );
                {
                    vk::ImageCopy ic2{};
                    ic2.setSrcSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                    ic2.setDstSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1});
                    ic2.setExtent(vk::Extent3D{
                        static_cast<uint32_t>(blurFrame.halfSrc->width),
                        static_cast<uint32_t>(blurFrame.halfSrc->height),
                        1u
                    });
                    commandBuffer.copyImage(blurFrame.halfDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                            blurFrame.halfSrc->image, vk::ImageLayout::eTransferDstOptimal,
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
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass C: vertical blur (half) into blurFrame.halfDst
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
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );

                // Pass D: upsample half -> full into blurFrame.fullDst
                int fullX = static_cast<int>(std::ceil(blurFrame.fullDst->width/16.0));
                int fullY = static_cast<int>(std::ceil(blurFrame.fullDst->height/16.0));
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.upsampleSet}, {});
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
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                        )
                    }
                );
            } else {
                // Two-level: Full -> Half -> Quarter, blur at quarter, then upsample back
                int halfX = static_cast<int>(std::ceil(blurFrame.halfSrc->width/16.0));
                int halfY = static_cast<int>(std::ceil(blurFrame.halfSrc->height/16.0));
                int qX = static_cast<int>(std::ceil(blurFrame.quarterSrc->width/16.0));
                int qY = static_cast<int>(std::ceil(blurFrame.quarterSrc->height/16.0));

                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // A: downsample full->half
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.downsampleSet}, {});
                commandBuffer.dispatch(halfX, halfY, 1);
                // make halfSrc readable
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {}, { vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        blurFrame.halfSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)) });

                // B: downsample half->quarter
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, downsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.downsample2Set}, {});
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
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // C: horizontal blur (quarter) into quarterDst
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, blurPipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.quarterBlurSet}, {});
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
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eTransferWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
                {
                    vk::ImageCopy ic{};
                    ic.setSrcSubresource({vk::ImageAspectFlagBits::eColor,0,0,1});
                    ic.setDstSubresource({vk::ImageAspectFlagBits::eColor,0,0,1});
                    ic.setExtent(vk::Extent3D{ static_cast<uint32_t>(blurFrame.quarterSrc->width), static_cast<uint32_t>(blurFrame.quarterSrc->height), 1u });
                    commandBuffer.copyImage(blurFrame.quarterDst->image, vk::ImageLayout::eTransferSrcOptimal,
                                            blurFrame.quarterSrc->image, vk::ImageLayout::eTransferDstOptimal,
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
                            blurFrame.quarterSrc->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
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
                            blurFrame.quarterDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.upsample2Set}, {});
                commandBuffer.dispatch(halfX, halfY, 1);

                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eComputeShader,
                    {}, {}, {},
                    {
                        vk::ImageMemoryBarrier(
                            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.halfDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        ),
                        vk::ImageMemoryBarrier(
                            {}, vk::AccessFlagBits::eShaderWrite,
                            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );

                // G: upsample half -> full into blurFrame.fullDst
                int fullX = static_cast<int>(std::ceil(blurFrame.fullDst->width/16.0));
                int fullY = static_cast<int>(std::ceil(blurFrame.fullDst->height/16.0));
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, upsamplePipeline.get());
                commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blurPipelineLayout.get(), 0, {blurFrame.upsampleSet}, {});
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
                            blurFrame.fullDst->image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)
                        )
                    }
                );
            }
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

            image_render->renderImageSized(commandBuffer, frame, shellRenderPass.get(), compositedBackgroundView,
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

        bool enable_cursor = false;
        for(unsigned int i=overlay_begin; i < overlays.size(); i++) {
            if(i == overlays.size()-1 && overlay_transition) {
                renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
                overlays[i]->render(renderer, this);
                renderer.pop_color();
            } else {
                overlays[i]->render(renderer, this);
            }
            enable_cursor = overlays[i]->enable_cursor();
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

        if(enable_cursor) {
            constexpr float cursor_size = 0.05f;
            if(cursorTexture && cursorTexture->loaded) {
                renderer.draw_image(*cursorTexture,
                    cursor_position.x - (cursor_size/2.0f)/renderer.aspect_ratio,
                    cursor_position.y - cursor_size/2.0f,
                    cursor_size, cursor_size);
            } else {
                renderer.draw_rect(glm::vec2{
                        cursor_position.x - (cursor_size/6.0f)/renderer.aspect_ratio,
                        cursor_position.y - cursor_size/6.0f
                    },
                    glm::vec2{(cursor_size/3.0f)/renderer.aspect_ratio, cursor_size/3.0f},
                    glm::vec4{1.0f, 1.0f, 1.0f, 0.85f});
            }
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
            std::filesystem::path fallback_icon = config::CONFIG.asset_directory / "icons" / std::format("icon_button_default_{}.png", name);

            buttonTextures[i] = std::make_unique<texture>(device, allocator);
            loader->loadTexture(buttonTextures[i].get(), existing_asset_or_fallback(std::move(icon_name), std::move(fallback_icon)));
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
                    dispatch<events::joystick_axis>(dir, i,
                        controller_axis_position[i].x,
                        controller_axis_position[i].y);
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
        poll_mouse();
        tick_cursor();
    }

    void shell::dispatch(const event& event) {
        if(background_only) {
            return;
        }

        for(int i=static_cast<int>(overlays.size())-1; i >= 0; i--) {
            auto& e = overlays[i];
            if(e->enable_cursor() && handle_cursor(event)) {
                return;
            }

            result res = result::unsupported;
            if(auto* recv = dynamic_cast<event_receiver*>(e.get())) {
                res = recv->on_event(event);
            } else if(auto* recv = dynamic_cast<action_receiver*>(e.get())) {
                res = recv->on_action(event.action);
            }

            if(res == result::unsupported) {
                if(auto* d = event.get<events::joystick_axis>()) {
                    if(auto* recv = dynamic_cast<joystick_receiver*>(e.get())) {
                        res = recv->on_joystick(static_cast<unsigned int>(d->index), d->x, d->y);
                    }
                } else if(auto* d = event.get<events::mouse_move>()) {
                    if(auto* recv = dynamic_cast<mouse_receiver*>(e.get())) {
                        res = recv->on_mouse_move(d->x, d->y);
                    }
                } else if(auto* d = event.get<events::mouse_scroll>()) {
                    if(auto* recv = dynamic_cast<mouse_receiver*>(e.get())) {
                        res = recv->on_mouse_scroll(d->x);
                    }
                }
            }

            if(res != result::unsupported) {
                if(res & result::close) {
                    remove_overlay(i);
                    i--;
                }
                handle(res);
                return;
            }
        }

        handle(menu.on_action(event.action));
    }

    void shell::poll_mouse()
    {
        int x = 0;
        int y = 0;
        std::uint32_t buttons = SDL_GetMouseState(&x, &y);
        glm::ivec2 position{x, y};

        const float width = static_cast<float>(std::max(1u, win->swapchainExtent.width));
        const float height = static_cast<float>(std::max(1u, win->swapchainExtent.height));
        const glm::vec2 normalized{
            glm::clamp(static_cast<float>(position.x) / width, 0.0f, 1.0f),
            glm::clamp(static_cast<float>(position.y) / height, 0.0f, 1.0f)
        };

        glm::ivec2 relative{0, 0};
        if(mouse_state_initialized) {
            relative = position - last_mouse_position;
        }

        if(!mouse_state_initialized || position != last_mouse_position) {
            dispatch(event{
                action::none,
                events::mouse_move{
                    normalized.x,
                    normalized.y,
                    static_cast<float>(relative.x) / width,
                    static_cast<float>(relative.y) / height
                }
            });
        }

        for(int button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; ++button) {
            const std::uint32_t mask = SDL_BUTTON(button);
            const bool was_down = (last_mouse_buttons & mask) != 0;
            const bool is_down = (buttons & mask) != 0;
            if(is_down && !was_down) {
                dispatch(event{
                    action::none,
                    events::mouse_button_down{to_logical_mouse_button(button)}
                });
            } else if(!is_down && was_down) {
                dispatch(event{
                    action::none,
                    events::mouse_button_up{to_logical_mouse_button(button)}
                });
            }
        }

        last_mouse_position = position;
        last_mouse_buttons = buttons;
        mouse_state_initialized = true;
    }

    void shell::tick_cursor()
    {
        const bool cursor_enabled = std::ranges::any_of(overlays, [](const auto& overlay) {
            return overlay->enable_cursor();
        });
        if(!cursor_enabled) {
            cursor_joystick_delta = glm::vec2{0.0f, 0.0f};
            return;
        }
        if(cursor_joystick_delta.x == 0.0f && cursor_joystick_delta.y == 0.0f) {
            return;
        }
        cursor_position = glm::clamp(cursor_position + cursor_joystick_delta, glm::vec2{0.0f}, glm::vec2{1.0f});
        dispatch<events::cursor_move>(action::none, cursor_position.x, cursor_position.y);
    }

    bool shell::handle_cursor(const event& event)
    {
        if(auto* d = event.get<events::mouse_move>()) {
            cursor_position = glm::vec2{d->x, d->y};
            dispatch<events::cursor_move>(action::none, cursor_position.x, cursor_position.y);
            return true;
        }
        if(auto* d = event.get<events::joystick_axis>()) {
            if(d->index == events::logical_joystick_index::right) {
                constexpr float controller_cursor_speed = 1.0f;
                cursor_joystick_delta = (glm::vec2{d->x, d->y} / 100.0f) * controller_cursor_speed;
                if(std::abs(d->x) < 0.1f) {
                    cursor_joystick_delta.x = 0.0f;
                }
                if(std::abs(d->y) < 0.1f) {
                    cursor_joystick_delta.y = 0.0f;
                }
                return true;
            }
        }
        return false;
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
            if(ok_sound && sdl::mix::PlayChannel(-1, ok_sound.get(), 0) == -1) {
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
        dispatch(event{
            action::none,
            events::key_up{static_cast<unsigned int>(std::to_underlying(key.scancode))}
        });
    }
    void shell::key_down(sdl::Keysym key)
    {
        spdlog::trace("Key down: {}", key.sym);
        switch(key.sym) {
            case sdl::KeyCode::SDLK_LEFT:
                dispatch(event{action::left, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_RIGHT:
                dispatch(event{action::right, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_UP:
                dispatch(event{action::up, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_DOWN:
                dispatch(event{action::down, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_RETURN:
                dispatch(event{action::ok, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_ESCAPE:
                dispatch(event{action::cancel, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_TAB:
                dispatch(event{action::options, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            case sdl::KeyCode::SDLK_CAPSLOCK:
                dispatch(event{action::extra, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
                break;
            default:
                dispatch(event{action::none, events::key_down{static_cast<unsigned int>(std::to_underlying(key.scancode))}});
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
            dispatch(event{action::left, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::DPAD_RIGHT) {
            dispatch(event{action::right, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::DPAD_UP) {
            dispatch(event{action::up, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::DPAD_DOWN) {
            dispatch(event{action::down, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::A) {
            dispatch(event{action::ok, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::B) {
            dispatch(event{action::cancel, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::Y) {
            dispatch(event{action::options, events::controller_button_down{to_logical_button(button)}});
        } else if(button == sdl::GameControllerButtonValues::X) {
            dispatch(event{action::extra, events::controller_button_down{to_logical_button(button)}});
        } else {
            dispatch(event{action::none, events::controller_button_down{to_logical_button(button)}});
        }
    }
    void shell::button_up(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button up: {}", fmt::underlying(button));
        last_controller_button_input = std::nullopt;
        dispatch(event{action::none, events::controller_button_up{to_logical_button(button)}});
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

        const auto default_dispatch = [&]() {
            dispatch<events::joystick_axis>(action::none, stick_index,
                controller_axis_position[stick_index].x,
                controller_axis_position[stick_index].y);
        };

        if(!config::CONFIG.controllerAnalogStick) {
            default_dispatch();
            return;
        }

        if(axis == sdl::GameControllerAxisValues::LEFTX || axis == sdl::GameControllerAxisValues::LEFTY) {
            unsigned int index = axis == sdl::GameControllerAxisValues::LEFTX ? 0 : 1;
            if(std::abs(value) < controller_axis_input_threshold) {
                last_controller_axis_input[index] = std::nullopt;
                last_controller_axis_input_time[index] = std::chrono::steady_clock::now();
                default_dispatch();
                return;
            }
            action dir = axis == sdl::GameControllerAxisValues::LEFTX  ? (value > 0 ? action::right : action::left)
                : (value > 0 ? action::down : action::up);
            if(last_controller_axis_input[index] && std::get<1>(*last_controller_axis_input[index]) == dir) {
                default_dispatch();
                return;
            }
            dispatch<events::joystick_axis>(dir, index,
                controller_axis_position[index].x,
                controller_axis_position[index].y);
            last_controller_axis_input[index] = std::make_tuple(controller, dir);
            last_controller_axis_input_time[index] = std::chrono::steady_clock::now();
        } else {
            default_dispatch();
        }
    }
}
