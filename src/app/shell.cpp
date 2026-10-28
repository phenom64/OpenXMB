/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025-2026 Syndromatic Ltd. All rights reserved
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

#include <array>
#include <algorithm>
#include <charconv>
#include <fast_float/fast_float.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "../localization/shell_locale.hpp"
#include <ctime>
#include <SDL2/SDL.h>
#undef main
#ifdef _WIN32
#include <windows.h>
#endif

#include <glm/vec2.hpp>

#include "openxmb/xmb/boot_timeline.hpp"
#include "openxmb/xmb/background.hpp"
#include "openxmb/music/player.hpp"
#include "input_bindings.hpp"
#include "components/media_motion.hpp"
#include "../localization/font_selection.hpp"
#include "openxmb/integrations/desktop.hpp"
#include "openxmb/themes/library.hpp"
#include <map>

module openxmb.app;

import i18n;
import spdlog;
import auroreengine;
import glm;
import vulkan_hpp;
import vma;
import sdl2;
import openxmb.config;
import openxmb.constants;
import openxmb.render;
import openxmb.debug;
import openxmb.utils;
import openxmb.xmb.captured_wave_renderer;
import openxmb.xmb.monthly_background_renderer;
import :startup_overlay;
import :message_overlay;
import :programs;
import :theme_manager;
import :integration_center;
import :library_manager;
import :about;
import :desktop_settings;

using namespace mfk::i18n::literals;

namespace app
{
    struct shell::theme_state {
        std::shared_ptr<openxmb::themes::ThemeLibrary> library;
        std::optional<openxmb::themes::AppliedTheme> active;
        std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
        openxmb::themes::AnimationFrame frame;
        double animation_seconds=0;
        std::chrono::steady_clock::time_point sampled=std::chrono::steady_clock::now();
        std::map<std::filesystem::path,std::unique_ptr<texture>> textures;
    };

    namespace {
        inline constexpr float xmb_web_native_steady_wave_fill_alpha = 0.078F;

        bool truthy_environment_flag(const char* name) noexcept
        {
            const char* value = std::getenv(name);
            if(value == nullptr || value[0] == '\0') {
                return false;
            }
            const std::string_view text(value);
            return text != "0" && text != "false" && text != "FALSE" &&
                   text != "off" && text != "OFF" && text != "no" &&
                   text != "NO";
        }

        bool force_headless_startup_overlay() noexcept
        {
            return truthy_environment_flag("OPENXMB_HEADLESS_STARTUP");
        }

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

        [[nodiscard]] std::optional<double> fixed_seconds_from_environment(
            const char* name)
        {
            const char* value = std::getenv(name);
            if(value == nullptr || *value == '\0') {
                return std::nullopt;
            }

            const std::string_view text{value};
            double seconds{};
            const auto [end, error] = fast_float::from_chars(
                text.data(), text.data() + text.size(), seconds,
                fast_float::chars_format::general);
            if(error != std::errc{} || end != text.data() + text.size() ||
               !std::isfinite(seconds) || seconds < 0.0) {
                spdlog::warn(
                    "Ignoring invalid {} value '{}'; expected a finite non-negative number",
                    name, text);
                return std::nullopt;
            }
            return seconds;
        }

        [[nodiscard]] std::optional<double> fixed_wave_seconds_from_environment()
        {
            return fixed_seconds_from_environment("OPENXMB_FIXED_WAVE_SECONDS");
        }

        [[nodiscard]] std::optional<double> fixed_boot_seconds_from_environment()
        {
            return fixed_seconds_from_environment("OPENXMB_FIXED_BOOT_SECONDS");
        }
    }

    struct BlurConstants {
        int axis = 0;
        int size = 20;
    };

    void component::render_controller_buttons(app::shell* xmb, auroreengine::gui_renderer& renderer, float x, float y, std::span<const std::pair<::action, std::string_view>> buttons) const
    {
        const auto saved_scale = renderer.text_scale;
        renderer.text_scale /= std::max(1.0F, typography_scale());
        xmb->render_controller_buttons(renderer, x, y, buttons);
        renderer.text_scale = saved_scale;
    }

    shell::shell(window* window) : phase(window),theme(std::make_unique<theme_state>())
    {
    }

    shell::~shell()
    {
        openxmb::music::service().shutdown();
    }

    void shell::preload()
    {
        phase::preload();

        font_render = std::make_unique<font_renderer>(openxmb::localization::select_font(config::CONFIG.fontPath,config::CONFIG.asset_directory,config::CONFIG.language).string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        image_render = std::make_unique<image_renderer>(device, win->swapchainExtent, win->gpuFeatures);
        image_render->setAllocator(allocator);
        simple_render = std::make_unique<simple_renderer>(device, allocator, win->swapchainExtent, win->gpuFeatures);
#if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render = std::make_unique<render::video_background>(device, allocator, win);
#endif
        wave_render = std::make_unique<render::wave_renderer>(device, allocator, win->swapchainExtent);
        procedural_wave_render = std::make_unique<render::procedural_wave_renderer>(device, allocator, win->swapchainExtent);
        monthly_background_render =
            std::make_unique<openxmb::xmb::MonthlyBackgroundRenderer>(device, win->swapchainExtent);

        {
            const auto background_format=win->swapchainFormat.format==vk::Format::eB8G8R8A8Srgb?vk::Format::eB8G8R8A8Unorm:
                win->swapchainFormat.format==vk::Format::eR8G8B8A8Srgb?vk::Format::eR8G8B8A8Unorm:win->swapchainFormat.format;
            std::array<vk::SubpassDependency, 2> deps{
                vk::SubpassDependency(vk::SubpassExternal, 0,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                    vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite),
                vk::SubpassDependency(0, vk::SubpassExternal,
                    vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
                    vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead)
            };
            const auto depth_attachment = vk::AttachmentDescription({}, vk::Format::eD32Sfloat,
                win->config.sampleCount, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal);
            if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                const std::array attachments{
                    vk::AttachmentDescription({}, background_format,
                        vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
                    depth_attachment
                };
                const vk::AttachmentReference color_ref(0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference depth_ref(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);
                const vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics,
                    {}, color_ref, {}, &depth_ref);
                backgroundRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, deps));
            } else {
                const std::array attachments{
                    vk::AttachmentDescription({}, background_format, win->config.sampleCount,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal),
                    vk::AttachmentDescription({}, background_format, vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal),
                    depth_attachment
                };
                const vk::AttachmentReference color_ref(0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference resolve_ref(1, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference depth_ref(2, vk::ImageLayout::eDepthStencilAttachmentOptimal);
                const vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics,
                    {}, color_ref, resolve_ref, &depth_ref);
                backgroundRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, deps));
            }
            debugName(device, backgroundRenderPass.get(), "Background Render Pass");
        }
        {
            vk::SubpassDependency dep(vk::SubpassExternal, 0,
                vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eColorAttachmentWrite);
            if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                const std::array attachments{
                    vk::AttachmentDescription({}, win->swapchainFormat.format,
                        vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, win->swapchainFinalLayout)
                };
                const vk::AttachmentReference color_ref(
                    0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::SubpassDescription subpass(
                    {}, vk::PipelineBindPoint::eGraphics, {}, color_ref);
                shellRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, dep));
            } else {
                const std::array attachments{
                    vk::AttachmentDescription({}, win->swapchainFormat.format, win->config.sampleCount,
                        vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eDontCare,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal),
                    vk::AttachmentDescription({}, win->swapchainFormat.format, vk::SampleCountFlagBits::e1,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                        vk::ImageLayout::eUndefined, win->swapchainFinalLayout)
                };
                const vk::AttachmentReference color_ref(
                    0, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::AttachmentReference resolve_ref(
                    1, vk::ImageLayout::eColorAttachmentOptimal);
                const vk::SubpassDescription subpass(
                    {}, vk::PipelineBindPoint::eGraphics, {}, color_ref,
                    resolve_ref);
                shellRenderPass = device.createRenderPassUnique(
                    vk::RenderPassCreateInfo({}, attachments, subpass, dep));
            }
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

        font_render->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        add_task(font_render->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff));
        image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
        try {
            monthly_background_render->preload(
                {backgroundRenderPass.get()}, win->config.sampleCount,
                win->pipelineCache.get());
        } catch(const std::exception& error) {
            monthly_background_failed = true;
            spdlog::error(
                "Monthly-background Vulkan pipeline initialization failed ({}); using the safe clear-colour fallback",
                error.what());
        }
#if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
#endif
        procedural_wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());

        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
        iconGlassAmbientTexture = std::make_unique<texture>(device, allocator);
        iconGlassEnvironmentTexture = std::make_unique<texture>(device, allocator);
        const auto glass_environment_directory =
            config::CONFIG.asset_directory / "materials";
        loader->loadTexture(iconGlassAmbientTexture.get(),
                            glass_environment_directory / "icon_amb.png");
        loader->loadTexture(iconGlassEnvironmentTexture.get(),
                            glass_environment_directory / "texenv.png");
        config::CONFIG.addCallback("background-type", [this](const std::string&){
            if(config::CONFIG.backgroundType == config::config::background_type::image || config::CONFIG.backgroundType == config::config::background_type::video) {
                reload_background();
            } else {
                backgroundTexture.reset();
            }
        });
        config::CONFIG.addCallback("background-image", [this](const std::string&){
            if(config::CONFIG.backgroundType == config::config::background_type::image || config::CONFIG.backgroundType == config::config::background_type::video) {
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

        reload_background();
        if(!background_only) {
            if(auto saved = theme_library()->active()) apply_theme(*saved);
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
            ok_sound = sdl::mix::unique_chunk{sdl::mix::LoadWAV(config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds/ok.wav").string().c_str())};
            if(!ok_sound) {
                spdlog::warn("sdl::mix::LoadWAV: {}", sdl::mix::GetError());
            }
            auto load_sound_multi = [&](sdl::mix::unique_chunk& slot, std::initializer_list<const char*> names){
                for(const char* n : names) {
                    auto p = config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds"/n).string();
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

        // Push startup splash overlay (plays jingle, fades text). Headless
        // runs keep their settled-scene default unless visual verification
        // explicitly opts into the real boot overlay.
        if(!win->config.headless || force_headless_startup_overlay()) {
            shader_time_zero = std::chrono::steady_clock::now();
            startup_timeline.reset();
            emplace_overlay<app::startup_overlay>();
        }
        fixed_components_loaded = true;
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
        backgroundAttachmentViews.clear();backgroundMsaaViews.clear();
        backgroundResolve.clear();
        backgroundDepth.clear();
        blurFrames.clear();
        procedural_wave_render->prepare(static_cast<int>(swapchainImages.size()));
#if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render->prepare(imageCount);
#endif

        if(imageCount == 0) {
            font_render->prepare(0);
            image_render->prepare(0);
            simple_render->prepare(0);
            wave_render->prepare(0);
            return;
        }

        const auto extent = win->swapchainExtent;
        framebuffers.reserve(imageCount);
        backgroundFramebuffers.reserve(imageCount);
        renderImages.reserve(imageCount);
        backgroundResolve.reserve(imageCount);
        backgroundDepth.reserve(imageCount);
        blurFrames.reserve(imageCount);

        for(std::uint32_t i=0; i<imageCount; i++)
        {
            backgroundDepth.push_back(std::make_unique<texture>(device, allocator, extent,
                vk::ImageUsageFlagBits::eDepthStencilAttachment, vk::Format::eD32Sfloat,
                win->config.sampleCount, false, vk::ImageAspectFlagBits::eDepth));
            debugName(device, swapchainImages[i], "Swapchain Image #"+std::to_string(i));
            if(win->config.sampleCount != vk::SampleCountFlagBits::e1) {
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
                const auto unorm=win->swapchainFormat.format==vk::Format::eB8G8R8A8Srgb?vk::Format::eB8G8R8A8Unorm:
                    win->swapchainFormat.format==vk::Format::eR8G8B8A8Srgb?vk::Format::eR8G8B8A8Unorm:win->swapchainFormat.format;
                backgroundAttachmentViews.push_back(tex->create_view(unorm));
                if(win->config.sampleCount!=vk::SampleCountFlagBits::e1) backgroundMsaaViews.push_back(renderImages[i]->create_view(unorm));
                backgroundResolve.push_back(std::move(tex));
            }
            blurFrames.emplace_back();
            {
                if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                    const std::array attachments{swapchainViews[i]};
                    framebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, shellRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                } else {
                    const std::array attachments{
                        renderImages[i]->imageView.get(), swapchainViews[i]};
                    framebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, shellRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                }
                debugName(device, framebuffers.back().get(), "XMB Shell Framebuffer #"+std::to_string(i));
            }
            {
                if(win->config.sampleCount == vk::SampleCountFlagBits::e1) {
                    const std::array attachments{
                        backgroundAttachmentViews[i].get(), backgroundDepth[i]->imageView.get()};
                    backgroundFramebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, backgroundRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                } else {
                    const std::array attachments{
                        backgroundMsaaViews[i].get(),
                        backgroundAttachmentViews[i].get(), backgroundDepth[i]->imageView.get()};
                    backgroundFramebuffers.push_back(device.createFramebufferUnique(
                        vk::FramebufferCreateInfo({}, backgroundRenderPass.get(), attachments,
                            extent.width, extent.height, 1)));
                }
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

        font_render->prepare(swapchainViews.size());
        image_render->prepare(swapchainViews.size());
        simple_render->prepare(swapchainViews.size());
        wave_render->prepare(swapchainViews.size());
    }

    void shell::ensure_blur(int frame) {
        auto& resources = blurFrames.at(frame);
        if(resources.fullSrc) return;
        const auto extent = win->swapchainExtent;
        const vk::Extent2D half{std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)};
        const vk::Extent2D quarter{std::max(1u, half.width / 2), std::max(1u, half.height / 2)};
        const auto usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        const auto make = [&](vk::Extent2D size) {
            return std::make_unique<texture>(device, allocator, size, usage,
                vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false);
        };
        resources.fullSrc = make(extent); resources.fullDst = make(extent);
        resources.halfSrc = make(half); resources.halfDst = make(half);
        resources.quarterSrc = make(quarter); resources.quarterDst = make(quarter);
        const std::array<vk::DescriptorSet, 7> sets{blurDescriptorSets[frame],
            resources.downsampleSet, resources.halfBlurSet, resources.upsampleSet,
            resources.downsample2Set, resources.quarterBlurSet, resources.upsample2Set};
        const std::array<std::array<texture*, 2>, 7> pairs{{
            {resources.fullSrc.get(), resources.fullDst.get()},
            {resources.fullSrc.get(), resources.halfSrc.get()},
            {resources.halfSrc.get(), resources.halfDst.get()},
            {resources.halfDst.get(), resources.fullDst.get()},
            {resources.halfSrc.get(), resources.quarterSrc.get()},
            {resources.quarterSrc.get(), resources.quarterDst.get()},
            {resources.quarterDst.get(), resources.halfDst.get()}}};
        std::array<std::array<vk::DescriptorImageInfo, 2>, 7> infos;
        std::array<vk::WriteDescriptorSet, 7> writes;
        for(std::size_t i = 0; i < sets.size(); ++i) {
            for(std::size_t j = 0; j < 2; ++j)
                infos[i][j] = vk::DescriptorImageInfo({}, pairs[i][j]->imageView.get(), vk::ImageLayout::eGeneral);
            writes[i] = vk::WriteDescriptorSet(sets[i], 0, 0, 2, vk::DescriptorType::eStorageImage, infos[i].data());
        }
        device.updateDescriptorSets(writes, {});
    }

    void shell::reload_language() {
        try {
            const std::string& lang = config::CONFIG.language;
            openxmb::localization::shell_locale::apply(lang, config::CONFIG.locale_directory);
            reload_fonts();
            spdlog::info("Language set to '{}'; reloading menus", lang);

            // Rebuild main menu to refresh translated strings
            reload_menu();
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

        if(theme->active && !theme->active->animation.empty()) {
            try {
                if(!config::CONFIG.enabled("power.reduced.motion",false))theme->animation_seconds+=std::chrono::duration<double>(now-theme->sampled).count();
                theme->sampled=now;
                const auto seconds=theme->animation_seconds;
                theme->frame = openxmb::themes::ThemeLibrary::sample_animation(*theme->active,
                    fixed_wave_seconds_from_environment().value_or(seconds));
                std::vector<std::filesystem::path> needed;
                if(!theme->frame.background.empty()) needed.push_back(theme->frame.background);
                for(const auto& layer : theme->frame.layers) needed.push_back(layer.image);
                for(const auto& draw : theme->frame.draws) needed.push_back(draw.image);
                for(const auto& path : needed) {
                    const auto ext=path.extension().string();
                    const bool video=ext==".mp4" || ext==".webm" || ext==".mkv" || ext==".mov" || ext==".avi";
#if OPENXMB_ENABLE_VIDEO_PLAYER
                    if(video && path==theme->frame.background) {
                        if(background_video_path!=path) { video_background_render->set_path(path); background_video_path=path; }
                        continue;
                    }
#endif
                    if(path.empty() || video || theme->textures.contains(path)) continue;
                    std::size_t bytes=0;
                    for(const auto& [key, value] : theme->textures) if(value->loaded) bytes+=static_cast<std::size_t>(value->width)*value->height*4;
                    if(theme->textures.size()>=128 || bytes>128u*1024u*1024u) {
                        win->waitIdle();
                        std::erase_if(theme->textures,[&](const auto& item) { return std::find(needed.begin(),needed.end(),item.first)==needed.end(); });
                        if(theme->textures.size()>=128) break;
                    }
                    auto image=std::make_unique<texture>(device,allocator);
                    loader->loadTexture(image.get(),path);
                    theme->textures.emplace(path,std::move(image));
                }
            } catch(const std::exception& error) { spdlog::warn("Theme animation failed: {}",error.what()); }
        }
#if OPENXMB_ENABLE_VIDEO_PLAYER
        const bool background_video_active=!background_video_path.empty() && !ingame_mode &&
            (config::CONFIG.backgroundType==config::config::background_type::video || theme->frame.background==background_video_path);
        video_background_render->set_active(background_video_active);
        video_background_render->tick();
#endif
        commandBuffer.begin(vk::CommandBufferBeginInfo());
#if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render->prerender(commandBuffer,frame);
#endif
        for(auto& overlay : std::views::reverse(overlays)) {
            overlay->prerender(commandBuffer, frame, this);
        }
        {
            const auto wall_now = openxmb::xmb::wall_clock_now();
            auto themeColour = utils::xmb_resolve_theme_colour(wall_now);
            glm::vec3 baseThemeColour = themeColour.base_colour;
            const auto wall_time = std::chrono::system_clock::to_time_t(wall_now);
            std::tm local_time{};
#if defined(_WIN32)
            localtime_s(&local_time, &wall_time);
#else
            localtime_r(&wall_time, &local_time);
#endif
            const auto local_hour = static_cast<float>(local_time.tm_hour) +
                static_cast<float>(local_time.tm_min) / 60.0F +
                static_cast<float>(local_time.tm_sec) / 3600.0F;
            const auto day_night_hour =
                utils::xmb_effective_day_night_hour(local_hour);
            const auto original_gradient =
                openxmb::xmb::resolve_background_gradient(
                    local_time.tm_mon, day_night_hour);
            const auto manual_gradient =
                openxmb::xmb::resolve_manual_background_gradient(
                    {themeColour.base_colour.r, themeColour.base_colour.g,
                     themeColour.base_colour.b},
                    day_night_hour);
            auto background_gradient = config::CONFIG.themeOriginalColour
                ? original_gradient
                : manual_gradient;
            // xmb-web's Original background lets the calendar-driven gradient
            // carry the hue.  The captured ribbon and dust/glints stay close to
            // neutral silver/white so they read over every month instead of
            // forcing the older OpenXMB theme colour (which made the scene feel
            // stuck on June/cyan).
            const bool music_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->music_background();});
            const bool reactive_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->music_reactive();});
            const bool flow_scene=reactive_scene&&config::CONFIG.preference("music.visualizer","Wave")=="Flow";
            procedural_wave_render->flow_mode=flow_scene;
            procedural_wave_render->visualizer_time=flow_scene?static_cast<float>(config::CONFIG.enabled("power.reduced.motion",false)?36.0:fixed_wave_seconds_from_environment().value_or(-1.0)):-1.0F;
            // Preserve the departing scene's endpoint until its mix reaches
            // zero; switching endpoints on close made the ribbon jump first.
            if(music_scene)procedural_wave_render->music_vertical=reactive_scene?-0.02F:0.26F;
            const auto& music_snapshot=openxmb::music::service().snapshot();
            const bool reactive_audio=reactive_scene&&music_snapshot.status==openxmb::music::Status::playing&&!config::CONFIG.enabled("power.reduced.motion",false);
            procedural_wave_render->set_music_signal(music_snapshot.spectrum,reactive_audio,music_snapshot.generation);
            procedural_wave_render->set_music_mode(music_scene);
            const float music_mix=procedural_wave_render->music_mix();background_gradient.music_mix=music_mix;
            if(flow_scene) {background_gradient.top_rgb={0,0,0};background_gradient.bottom_rgb={0,0,0};background_gradient.music_mix=0;}
            glm::vec3 effect_color{background_gradient.bottom_rgb[0],background_gradient.bottom_rgb[1],background_gradient.bottom_rgb[2]};
            effect_color=glm::mix(glm::min(effect_color*1.6F,glm::vec3(1.0F)),glm::vec3(1.0F),0.35F);
            const glm::vec3 original_effect_tint=glm::mix(effect_color,glm::vec3(192.0F/255,108.0F/255,200.0F/255),music_mix);
            const float original_effect_brightness = 1.0F;
            const bool can_render_monthly_background =
                (music_scene||music_mix>0.001F||config::CONFIG.backgroundType ==
                    config::config::background_type::original) &&
                monthly_background_render->pipelines_ready() &&
                !monthly_background_failed;
            // Always tint the background clear colour (for both Original and Classic)
            vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            if(!can_render_monthly_background) {
                glm::vec3 c = themeColour.shaded_colour;
                color = vk::ClearColorValue(std::array<float, 4>{ c.r, c.g, c.b, 1.0f });
            }
            if(ingame_mode) {
                color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.5f});
            }
            std::array<vk::ClearValue, 3> background_clears{color, color, color};
            const auto depth_index = win->config.sampleCount == vk::SampleCountFlagBits::e1 ? 1u : 2u;
            background_clears[depth_index] = vk::ClearDepthStencilValue(1.0F, 0);
            image_render->setOutputEncoded(true);
            commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(backgroundRenderPass.get(), backgroundFramebuffers[frame].get(),
                vk::Rect2D({0, 0}, win->swapchainExtent), depth_index + 1, background_clears.data()), vk::SubpassContents::eInline);
            vk::Viewport viewport(0.0f, 0.0f,
                static_cast<float>(win->swapchainExtent.width),
                static_cast<float>(win->swapchainExtent.height), 0.0f, 1.0f);
            vk::Rect2D scissor({0,0}, win->swapchainExtent);
            commandBuffer.setViewport(0, viewport);
            commandBuffer.setScissor(0, scissor);

            const auto boot = startup_sample();

            if(!ingame_mode) {
                if(music_scene||music_mix>0.001F||config::CONFIG.backgroundType == config::config::background_type::original) {
                    if(can_render_monthly_background) {
                        try {
                            const std::array boot_background{
                                static_cast<float>(boot.background_exposure_top),
                                static_cast<float>(boot.background_exposure_bottom),
                                static_cast<float>(boot.background_sweep),
                                boot.background_active ? 1.0F : 0.0F,
                            };
                            monthly_background_render->render(
                                commandBuffer, backgroundRenderPass.get(),
                                background_gradient, boot_background);
                        } catch(const std::exception& error) {
                            monthly_background_failed = true;
                            spdlog::error(
                                "Monthly-background rendering failed ({}); using the safe clear-colour fallback",
                                error.what());
                        }
                    }
                    procedural_wave_render->render(
                        commandBuffer, frame, backgroundRenderPass.get(),
                        original_effect_tint, original_effect_brightness,
                        static_cast<float>(config::CONFIG.enabled("power.reduced.motion",false)?36.0:fixed_wave_seconds_from_environment().value_or(boot.wave_boot_time_seconds)));

                }
                else if(config::CONFIG.backgroundType == config::config::background_type::wave) {
                    wave_render->waveColor = baseThemeColour; // PS3 look: wave uses base, brightness on background only
                    wave_render->paused=config::CONFIG.enabled("power.reduced.motion",false);
                    if(config::CONFIG.wavesEnabled)wave_render->render(commandBuffer, frame, backgroundRenderPass.get());
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

#if OPENXMB_ENABLE_VIDEO_PLAYER
            if(background_video_active) video_background_render->render(commandBuffer,frame,backgroundRenderPass.get(),win->swapchainExtent);
#endif
            if(theme->active) {
                gui_renderer theme_gui(commandBuffer,frame,backgroundRenderPass.get(),win->swapchainExtent,font_render.get(),image_render.get(),simple_render.get());
                if(const auto it=theme->textures.find(theme->frame.background);it!=theme->textures.end() && it->second->loaded)
                    image_render->renderImageSized(commandBuffer,frame,backgroundRenderPass.get(),*it->second,0,0,win->swapchainExtent.width,win->swapchainExtent.height);
                for(const auto& layer : theme->frame.layers) {
                    const auto it=theme->textures.find(layer.image);
                    if(it!=theme->textures.end() && it->second->loaded)
                        image_render->renderImageSized(commandBuffer,frame,backgroundRenderPass.get(),*it->second,layer.x,layer.y,
                            layer.width*win->swapchainExtent.width,layer.height*win->swapchainExtent.height,glm::vec4(1,1,1,layer.opacity));
                }
                for(const auto& draw : theme->frame.draws) {
                    const auto it=theme->textures.find(draw.image);
                    if(it==theme->textures.end() || !it->second->loaded) continue;
                    std::vector<image_renderer::textured_vertex> vertices;
                    vertices.reserve(draw.vertices.size());
                    for(const auto& v:draw.vertices) vertices.push_back({{v.x,v.y,v.z,v.w},{v.u,v.v},{v.color[0],v.color[1],v.color[2],v.color[3]}});
                    theme_gui.draw_textured_triangles(*it->second,vertices,draw.additive,draw.depth);
                }
            }
            commandBuffer.endRenderPass();
            image_render->setOutputEncoded(false);
        }
        double blur_background_progress = utils::progress(now, last_blur_background_change, blur_background_transition_duration);
        const auto startup_blur_radius = std::max(0.0f, startup_background_blur_px);
        const bool use_startup_blur_background = startup_blur_radius > 0.05f;
        const bool opaque_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->is_opaque();});
        const auto menu_blur_radius = background_only ? 0.0F : media_motion::backdrop(menu.background_blur_px(),opaque_scene);
        const auto menu_dim_alpha = background_only ? 0.0F : media_motion::backdrop(menu.background_dim_alpha(),opaque_scene);
        const bool use_menu_blur_background = menu_blur_radius > 0.05F;
        const bool use_blur_background = blur_background || blur_background_progress < 1.0 ||
            use_startup_blur_background || use_menu_blur_background;
        vk::ImageView compositedBackgroundView = backgroundResolve[frame]->imageView.get();
        if(use_blur_background) {
            ensure_blur(frame);
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
            const int modalRadius = static_cast<int>(20 * (blur_background ? blur_background_progress : (1.0 - blur_background_progress)));
            const int startupRadius = static_cast<int>(std::round(startup_blur_radius));
            const int menuRadius = static_cast<int>(std::round(
                menu_blur_radius * static_cast<float>(win->swapchainExtent.height) / 1080.0F));
            const int targetRadius = std::max({modalRadius, startupRadius, menuRadius});

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

            const auto dim_setting=config::CONFIG.preference("power.dim","Off");
            const double dim_seconds=dim_setting=="1 minute"?60:dim_setting=="5 minutes"?300:dim_setting=="10 minutes"?600:0;
            const bool dimmed=dim_seconds>0&&std::chrono::duration<double>(std::chrono::steady_clock::now()-last_activity).count()>dim_seconds;
            const float brightness=std::clamp(static_cast<float>(config::CONFIG.number("display.brightness",1))*(dimmed?0.3F:1.0F),0.0F,1.2F);
            image_render->renderImageSized(commandBuffer, frame, shellRenderPass.get(), compositedBackgroundView,
                0.0f, 0.0f, static_cast<int>(win->swapchainExtent.width), static_cast<int>(win->swapchainExtent.height),glm::vec4(brightness,brightness,brightness,1.0F));
            gui_renderer ctx(commandBuffer, frame, shellRenderPass.get(), win->swapchainExtent, font_render.get(), image_render.get(), simple_render.get());
            if(menu_dim_alpha > 0.001F) {
                ctx.draw_rect(glm::vec2{0.0F, 0.0F}, glm::vec2{1.0F, 1.0F},
                    glm::vec4{0.0F, 0.0F, 0.0F, menu_dim_alpha});
            }
            image_render->setGlassBackground(compositedBackgroundView);
            image_render->setGlassMaterial(
                iconGlassAmbientTexture && iconGlassAmbientTexture->loaded
                    ? iconGlassAmbientTexture->imageView.get()
                    : vk::ImageView{},
                iconGlassEnvironmentTexture && iconGlassEnvironmentTexture->loaded
                    ? iconGlassEnvironmentTexture->imageView.get()
                    : vk::ImageView{});

            // Interface/FX debug overlays: draw font atlas for verification
            if(openxmb::debug::interfacefx_debug) {
                const auroreengine::texture* atlas = font_render->get_atlas();
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
        submit_graphics(submit_info, fence);
    }

    void shell::render_gui(gui_renderer& renderer) {
        renderer.text_color = glm::vec4(config::CONFIG.textColor, 1.0F);
        renderer.text_scale = static_cast<float>(config::CONFIG.textScale);
        // Applying persisted settings here also covers a profile with no active
        // imported theme. Unchanged parameters preserve the sampler's history.
        if(procedural_wave_render) {
            static const void* previous_renderer = nullptr;
            static decltype(config::CONFIG.waveParameters) previous_parameters;
            if(previous_renderer != procedural_wave_render.get() || previous_parameters != config::CONFIG.waveParameters) {
                procedural_wave_render->reset_parameters();
                for(const auto& [group, parameters] : config::CONFIG.waveParameters)
                    for(const auto& [key, number] : parameters) procedural_wave_render->set_parameter(group, key, number);
                previous_renderer = procedural_wave_render.get();
                previous_parameters = config::CONFIG.waveParameters;
            }
            procedural_wave_render->waves_enabled = config::CONFIG.wavesEnabled;
            procedural_wave_render->particles_enabled = config::CONFIG.particlesEnabled&&!config::CONFIG.enabled("power.reduced.motion",false);
            procedural_wave_render->particle_color = config::CONFIG.particleColor;
        }
        bool render_menu = true;
        unsigned int overlay_begin = 0;
        bool has_overlay = !overlays.empty();
        bool top_is_message = has_overlay && (dynamic_cast<app::message_overlay*>(overlays.back().get()) != nullptr);
        bool top_uses_shell_fade = has_overlay && overlays.back()->do_fade_in();

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
        const bool shell_fade_transition = overlay_transition && top_uses_shell_fade;
        bool allow_menu = render_menu || ((top_is_message || fading_out_message) && shell_fade_transition);
        if(allow_menu){
            const float startup_reveal = static_cast<float>(startup_sample().ui_reveal);
            renderer.push_color(glm::vec4(startup_reveal));
            if(shell_fade_transition || fading_out_message) {
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

            const auto text_color = renderer.text_color;
            renderer.text_color = glm::vec4(1.0F);
            renderer.push_color(glm::vec4(config::CONFIG.clockColor, 1.0F));
            if(!status_bar_render.render_local(
                   renderer, openxmb::xmb::wall_clock_now())) {
                static bool status_bar_time_warning_logged = false;
                if(!status_bar_time_warning_logged) {
                    spdlog::warn(
                        "The local civil time is unavailable; hiding the XMB status bar clock");
                    status_bar_time_warning_logged = true;
                }
            }

            renderer.pop_color();
            renderer.text_color = text_color;
            // The reference root scene has no legacy placeholder ticker.
            if(pushed_zoom) renderer.pop_zoom();
            if(shell_fade_transition || fading_out_message) {
                renderer.pop_color();
            }
            renderer.pop_color();

        }

        bool enable_cursor = false;
        for(unsigned int i=overlay_begin; i < overlays.size(); i++) {
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale) * overlays[i]->typography_scale();
            if(i == overlays.size()-1 && shell_fade_transition) {
                renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
                overlays[i]->render(renderer, this);
                renderer.pop_color();
            } else {
                overlays[i]->render(renderer, this);
            }
            enable_cursor = overlays[i]->enable_cursor();
        }
        if(!allow_menu && has_overlay && overlays[overlay_begin]->show_status_bar()) {
            const auto overlay_text_scale = renderer.text_scale;
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale);
            renderer.push_color(glm::vec4(config::CONFIG.clockColor,1.0F));
            static_cast<void>(status_bar_render.render_local(renderer,openxmb::xmb::wall_clock_now()));
            renderer.pop_color();
            renderer.text_scale = overlay_text_scale;
        }
        if(overlay_transition && overlay_fade_direction == transition_direction::out &&
           old_overlay && old_overlay->do_fade_out()) {
            renderer.push_color(glm::mix(glm::vec4(0.0), glm::vec4(1.0), dir_progress));
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale) * old_overlay->typography_scale();
            old_overlay->render(renderer, this);
            renderer.pop_color();
        } else if(old_overlay) {
            // Fade-out finished; if it was a message overlay, drop background blur now
            if(dynamic_cast<app::message_overlay*>(old_overlay.get()) != nullptr) {
                set_blur_background(false);
            }
            // Earlier submitted frames can still sample the retiring overlay's
            // icon/artwork textures after its fade has finished on the CPU.
            win->waitIdle();
            old_overlay.reset();
        }

        renderer.text_scale = static_cast<float>(config::CONFIG.textScale);
        if(enable_cursor && std::chrono::steady_clock::now()-cursor_active_at < std::chrono::seconds(2)) {
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


    std::shared_ptr<openxmb::themes::ThemeLibrary> shell::theme_library() {
        const auto directory = config::CONFIG.profileConfigPath.empty()
            ? openxmb::integrations::user_data_directory() / "themes"
            : config::CONFIG.profileConfigPath.parent_path() / "themes";
        if(!theme->library || theme->library->root() != directory) {
            theme->library = std::make_shared<openxmb::themes::ThemeLibrary>(directory);
            theme->library->set_baseline(nlohmann::json::parse(config::CONFIG.shell_snapshot()));
        }
        return theme->library;
    }

    void shell::reload_menu() {
        if(!fixed_components_loaded) return;
        win->waitIdle();
        menu.preload(device, allocator, *loader);
    }

    void shell::reload_theme_sounds() {
        const auto load=[&](sdl::mix::unique_chunk& chunk,std::initializer_list<const char*> names){for(const auto* name:names){const auto file=config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds"/name);if(auto* replacement=sdl::mix::LoadWAV(file.string().c_str())){chunk=sdl::mix::unique_chunk{replacement};return;}}};
        load(ok_sound,{"ok.wav"});
        load(confirm_sound,{"NSE.ui.Confirm.wav","NSE.ui.Confirm.ogg","confirm.wav"});
        load(cancel_sound,{"NSE.ui.Cancel.wav","NSE.ui.Cancel.ogg","cancel.wav"});
        load(back_sound,{"NSE.clicker.Cancel.wav","NSE.clicker.Cancel.ogg","back.wav"});
    }
    bool shell::set_audio_device(const std::string& name) {
        // Settings is reached after closing foreground playback; retire the
        // background decoder before resetting the shared SDL mixer device.
        win->waitIdle();
        #if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render.reset();
        #endif
        const bool changed=openxmb::music::service().set_output_device(name);
        if(changed)active_audio_device=name;
        #if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render=std::make_unique<render::video_background>(device,allocator,win);
        video_background_render->preload({backgroundRenderPass.get()},win->config.sampleCount,win->pipelineCache.get());
        video_background_render->prepare(win->swapchainImageCount);
        #endif
        reload_background();return changed;
    }
    void shell::open_panel(const std::string& name) {
        if(name.starts_with("bind.")) {
            emplace_overlay<app::binding_capture>(name.substr(5));
        } else if(name == "licenses") {
            emplace_overlay<programs::licenses_screen>(*loader);
        } else if(name == "about") {
            emplace_overlay<programs::about_screen>(*loader);
        } else if(name == "themes") {
            emplace_overlay<programs::theme_manager>(*loader, theme_library(),
                [this](const openxmb::themes::AppliedTheme& value) { apply_theme(value); },
                [] { return nlohmann::json::parse(config::CONFIG.shell_snapshot()); });
        } else if(name == "settings") {
            menu.show_settings();
        } else if(name == "users" || name == "games" || name == "custom" || name == "friends" || name == "account" || name == "trophies") {
            emplace_overlay<programs::library_manager>(name, *loader,
                [this](const std::filesystem::path& path) { switch_profile(path); });
        } else {
            emplace_overlay<programs::integration_center>(name, *loader);
        }
    }

    void shell::open_media(const std::filesystem::path& path) {
        const auto programs = programs::get_open_infos(path, programs::file_info(path));
        if(programs.empty()) {
            emplace_overlay<app::message_overlay>("Cannot open file", "No viewer is available for " + path.extension().string(), std::vector<std::string>{"OK"});
            return;
        }
        spdlog::info("Opening media: {} ({})", path.string(), programs.front().name);
        push_overlay(programs.front().create(path, *loader));
    }

    void shell::refresh_desktop_preferences() {
        desktop_preferences_applied=false;
        // Profile and reset callbacks run in input handling, before the engine
        // acquires a swapchain image. Rebuild here, never inside render/tick.
            if(win->config.sampleCount!=config::CONFIG.sampleCount) {
                const auto supported=win->physicalDevice.getProperties().limits.framebufferColorSampleCounts;
                if(supported&config::CONFIG.sampleCount) {win->waitIdle();win->config.sampleCount=config::CONFIG.sampleCount;win->recreateSwapchain();}
                else {spdlog::warn("Restored MSAA level is unsupported; retaining active level");config::CONFIG.sampleCount=win->config.sampleCount;}
            }
    }

    void shell::reset_settings() {
        win->waitIdle();
        static_cast<void>(theme_library()->revert());
        theme->active.reset();theme->frame={};theme->textures.clear();theme->library.reset();
        config::CONFIG.reset_to_defaults();
        refresh_desktop_preferences();reload_background();reload_fonts();reload_menu();reload_button_icons();reload_theme_sounds();
    }

    void shell::switch_profile(const std::filesystem::path& path) {
        win->waitIdle();
        config::CONFIG.switch_profile(path);
        refresh_desktop_preferences();
        theme->library.reset(); theme->active.reset(); theme->textures.clear();
        config::CONFIG.themeIcons.clear(); config::CONFIG.themeSounds.clear();
        if(auto saved = theme_library()->active()) apply_theme(*saved);
        else { reload_background(); reload_fonts(); reload_menu(); }
        reload_button_icons();reload_theme_sounds();
    }

    void shell::apply_theme(const openxmb::themes::AppliedTheme& theme) {
        win->waitIdle();
        const auto previous_font = config::CONFIG.fontPath;
        auto value = theme.shell;
        const auto& source = theme.settings;
        const auto inherit = [&](const char* from, const char* to) {
            if(source.contains(from) && !source[from].is_null()) value[to] = source[from];
        };
        // Imported packs share validated style setters. Disabled components
        // retain the pre-theme baseline.
        if(theme.components.colors) {
            inherit("icon-color", "icon-color"); inherit("text-color", "text-color");
            inherit("clock-color", "clock-color"); inherit("particle-color", "particle-color");
            inherit("dotColor", "particle-color");
        }
        if(theme.components.icons) { inherit("icon-style", "icon-style"); inherit("icon-glass-refraction", "icon-glass-refraction"); }
        if(theme.components.font) inherit("font-size", "font-size");
        if(theme.components.background) {
            inherit("theme-waves-enabled", "theme-waves-enabled"); inherit("wavesEnabled", "theme-waves-enabled");
            inherit("theme-particles-enabled", "theme-particles-enabled"); inherit("dotsEnabled", "theme-particles-enabled");
        }
        std::unique_ptr<font_renderer> validated_font;
        if(value.contains("font-path")&&value["font-path"].is_string()) {
            const auto requested=value["font-path"].get<std::string>();
            const auto path=requested=="default"?config::CONFIG.fallback_font:std::filesystem::u8path(requested);
            if(path!=previous_font)try {
                if(!openxmb::localization::font_covers(path,u8""))throw std::runtime_error("Cannot load font face");
                const auto rendered=openxmb::localization::select_font(path,config::CONFIG.asset_directory,config::CONFIG.language);
                validated_font=std::make_unique<font_renderer>(rendered.string(),32,device,allocator,win->swapchainExtent,win->gpuFeatures);
                validated_font->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
                validated_font->preload(loader,{shellRenderPass.get()},win->config.sampleCount,win->pipelineCache.get(),nullptr,0x20,0x1ff).get();
                validated_font->prepare(win->swapchainImageCount);
            } catch(...) {
                // The library may have persisted its selection before handing
                // it to the renderer. Restore that selection on load failure.
                try {if(this->theme->active&&!this->theme->active->reverted)static_cast<void>(theme_library()->apply(this->theme->active->id,this->theme->active->components));else static_cast<void>(theme_library()->revert());}
                catch(const std::exception& error){spdlog::error("Could not restore the previous theme selection: {}",error.what());}
                throw;
            }
        }
        this->theme->active = theme;
        this->theme->frame = {};
        this->theme->started = std::chrono::steady_clock::now();
        this->theme->sampled=this->theme->started;this->theme->animation_seconds=0;
        this->theme->textures.clear();
        config::CONFIG.themeIcons = theme.icons;
        config::CONFIG.themeSounds = theme.sounds;
        const auto text = [&](const char* key, auto&& setter) {
            if(value.contains(key) && value[key].is_string()) setter(value[key].get<std::string>());
        };
        text("background-image", [](const auto& v) { config::CONFIG.setBackgroundImage(v); });
        text("background-type", [](const auto& v) { config::CONFIG.setBackgroundType(v); });
        text("background-color", [](const auto& v) { config::CONFIG.setBackgroundColor(v); });
        text("wave-color", [](const auto& v) { config::CONFIG.setWaveColor(v); });
        text("theme-custom-colour", [](const auto& v) { config::CONFIG.setThemeCustomColour(v); });
        text("theme-colour-mode", [](const auto& v) { config::CONFIG.setThemeOriginalColour(v == "original"); });
        text("theme-day-night-mode", [](const auto& v) { config::CONFIG.setThemeDayNightMode(v); });
        text("font-path", [](const auto& v) { config::CONFIG.setFontPath(v); });
        text("icon-color", [](const auto& v) { config::CONFIG.setIconColor(v); });
        text("text-color", [](const auto& v) { config::CONFIG.setTextColor(v); });
        text("clock-color", [](const auto& v) { config::CONFIG.setClockColor(v); });
        text("particle-color", [](const auto& v) { config::CONFIG.setParticleColor(v == "auto" ? "#ffffff" : v); });
        if(value.contains("font-size") && value["font-size"].is_number()) config::CONFIG.setTextScale(value["font-size"].get<double>() / 100.0);
        if(value.contains("icon-glass-refraction") && value["icon-glass-refraction"].is_boolean()) config::CONFIG.setIconGlassRefraction(value["icon-glass-refraction"].get<bool>());
        text("icon-style", [](const auto& v) { config::CONFIG.setIconStyle(v); });
        if(value.contains("theme-waves-enabled") && value["theme-waves-enabled"].is_boolean()) config::CONFIG.setWavesEnabled(value["theme-waves-enabled"].get<bool>());
        if(value.contains("theme-particles-enabled") && value["theme-particles-enabled"].is_boolean()) config::CONFIG.setParticlesEnabled(value["theme-particles-enabled"].get<bool>());
        if(value.contains("wave-parameters") && value["wave-parameters"].is_object()) config::CONFIG.setWaveParameters(value["wave-parameters"].dump());
        else config::CONFIG.setWaveParameters("{}");
        procedural_wave_render->reset_parameters();
        for(const auto& [group, parameters] : config::CONFIG.waveParameters)
            for(const auto& [key, number] : parameters) procedural_wave_render->set_parameter(group, key, number);
        procedural_wave_render->waves_enabled = config::CONFIG.wavesEnabled;
        procedural_wave_render->particles_enabled = config::CONFIG.particlesEnabled&&!config::CONFIG.enabled("power.reduced.motion",false);
        procedural_wave_render->particle_color = config::CONFIG.particleColor;
        if(validated_font)font_render=std::move(validated_font);
        if(fixed_components_loaded) {
            const auto load = [&](auto& sound, const std::filesystem::path& path) {
                sound = sdl::mix::unique_chunk{sdl::mix::LoadWAV(config::CONFIG.resolve_sound(path).string().c_str())};
            };
            const auto dir = config::CONFIG.asset_directory / "sounds";
            load(ok_sound, dir/"ok.wav"); load(confirm_sound, dir/"NSE.ui.Confirm.wav");
            load(back_sound, dir/"NSE.clicker.Cancel.wav"); load(cancel_sound, dir/"NSE.ui.Cancel.wav");
            load(question_sound, dir/"NSE.questionMark.wav");
            reload_menu();
        }
        config::CONFIG.save_config();
    }

    void shell::set_font_file(const std::string& value) {
        const auto path=value=="default"?config::CONFIG.fallback_font:std::filesystem::u8path(value);
        win->waitIdle();
        if(!openxmb::localization::font_covers(path,u8""))throw std::runtime_error("Cannot load font face");
        const auto rendered=openxmb::localization::select_font(path,config::CONFIG.asset_directory,config::CONFIG.language);
        auto replacement=std::make_unique<font_renderer>(rendered.string(),32,device,allocator,win->swapchainExtent,win->gpuFeatures);
        replacement->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        replacement->preload(loader,{shellRenderPass.get()},win->config.sampleCount,win->pipelineCache.get(),nullptr,0x20,0x1ff).get();
        replacement->prepare(win->swapchainImageCount);
        config::CONFIG.setFontPath(path.string());font_render=std::move(replacement);
    }
    void shell::reload_fonts() {
        win->waitIdle();
        const auto rendered=openxmb::localization::select_font(config::CONFIG.fontPath,config::CONFIG.asset_directory,config::CONFIG.language);
        auto replacement = std::make_unique<font_renderer>(rendered.string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        replacement->set_preload_texts(openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        replacement->preload(loader, {shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get(), nullptr, 0x20, 0x1ff).get();
        replacement->prepare(win->swapchainImageCount);
        font_render = std::move(replacement);
    }

    void shell::reload_background() {
#if OPENXMB_ENABLE_VIDEO_PLAYER
        if(config::CONFIG.backgroundType == config::config::background_type::video) {
            background_video_path=config::CONFIG.backgroundImage;
            video_background_render->set_path(background_video_path);
            video_background_render->set_active(true);
        }
#endif
        if(config::CONFIG.backgroundType == config::config::background_type::image) {
            backgroundTexture = std::make_unique<texture>(device, allocator);
            loader->loadTexture(backgroundTexture.get(), config::CONFIG.backgroundImage);
        }
    }
    void shell::reload_button_icons() {
        const auto family=get_controller_type();
        for(std::size_t i=1;i<std::to_underlying(action::_length);++i) {
            button_labels[i].clear();buttonTextures[i].reset();if(family=="none")continue;
            const bool keyboard=family=="keyboard";const auto index=input_bindings::glyph_index(config::CONFIG.preferences,!keyboard,i-1);
            const auto code=input_bindings::read(config::CONFIG.preferences,!keyboard,i-1);
            if(family=="xbox") {
                const auto artwork=input_bindings::xbox_artwork(code);
                if(!artwork.empty()) {buttonTextures[i]=std::make_unique<texture>(device,allocator);loader->loadTexture(buttonTextures[i].get(),config::CONFIG.asset_directory/"icons/controller"/(std::string(artwork)+".png"));continue;}
            }
            if(index<0||family=="default") {
                if(keyboard)button_labels[i]=SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
                else {switch(code){case 0:button_labels[i]="South";break;case 1:button_labels[i]="East";break;case 2:button_labels[i]="West";break;case 3:button_labels[i]="North";break;case 4:button_labels[i]=family=="playstation"?"Select":"View";break;case 6:button_labels[i]=family=="playstation"?"Start":"Menu";break;case 9:button_labels[i]=family=="playstation"?"L1":"LB";break;case 10:button_labels[i]=family=="playstation"?"R1":"RB";break;default:button_labels[i]="Button "+std::to_string(code);}}
                if(button_labels[i].empty())button_labels[i]="Key "+std::to_string(code);continue;
            }
            const auto name=input_bindings::actions[static_cast<std::size_t>(index)];
            const auto icon=config::CONFIG.asset_directory/"icons"/std::format("icon_button_{}_{}.png",family,name);
            buttonTextures[i]=std::make_unique<texture>(device,allocator);
            loader->loadTexture(buttonTextures[i].get(),icon);
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
                if(ctype == sdl::GameControllerTypeValues::PS3 ||
                   ctype == sdl::GameControllerTypeValues::PS4 ||
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
            return "default";
        }
        return type;
    }

    void shell::tick() {
        if(!desktop_preferences_applied) {
            desktop_preferences_applied=true;
            if(!win->config.headless) {
                if(!desktop_defaults_captured) {
                    desktop_defaults_captured=true;desktop_default_flags=SDL_GetWindowFlags(win->win.get())&(SDL_WINDOW_FULLSCREEN_DESKTOP|SDL_WINDOW_FULLSCREEN);
                    SDL_GetWindowPosition(win->win.get(),&desktop_default_x,&desktop_default_y);
                    SDL_GetWindowSize(win->win.get(),&desktop_default_width,&desktop_default_height);
                }
                if(const auto name=config::CONFIG.preference("system.name","OpenXMB system");!name.empty())SDL_SetWindowTitle(win->win.get(),name.c_str());
                const auto mode=config::CONFIG.preference("display.mode");
                SDL_SetWindowFullscreen(win->win.get(),mode.empty()?desktop_default_flags:mode=="Windowed"?0:mode=="Borderless"?SDL_WINDOW_FULLSCREEN_DESKTOP:SDL_WINDOW_FULLSCREEN);
                if(config::CONFIG.preference("display.monitor").empty())SDL_SetWindowPosition(win->win.get(),desktop_default_x,desktop_default_y);
                if(config::CONFIG.preference("display.resolution").empty()){SDL_SetWindowDisplayMode(win->win.get(),nullptr);SDL_SetWindowSize(win->win.get(),desktop_default_width,desktop_default_height);}
                if(const auto monitor=config::CONFIG.preference("display.monitor");!monitor.empty()) {SDL_Rect bounds{};if(SDL_GetDisplayBounds(static_cast<int>(config::CONFIG.number("display.monitor",0)),&bounds)==0)SDL_SetWindowPosition(win->win.get(),bounds.x,bounds.y);}
                if(const auto resolution=config::CONFIG.preference("display.resolution");!resolution.empty()) {const auto x=resolution.find('x');if(x!=std::string::npos)try{SDL_DisplayMode display{};display.w=std::stoi(resolution.substr(0,x));display.h=std::stoi(resolution.substr(x+1));if(display.w>0&&display.h>0){SDL_SetWindowDisplayMode(win->win.get(),&display);SDL_SetWindowSize(win->win.get(),display.w,display.h);}}catch(const std::exception&) {spdlog::warn("Ignoring invalid saved resolution");}}
                const auto requested_device=config::CONFIG.preference("audio.device");
                const auto wanted_device=requested_device=="System Default"?std::string{}:requested_device;
                if(wanted_device!=active_audio_device)set_audio_device(wanted_device);
            }
        }
        const bool focused=win->config.headless||(SDL_GetWindowFlags(win->win.get())&SDL_WINDOW_INPUT_FOCUS)!=0;
        const auto cap=config::CONFIG.maxFPS>10000?0:config::CONFIG.maxFPS;
        win->config.fpsLimit=focused?cap:config::CONFIG.number("power.background.fps",30);
        auto& player=openxmb::music::service();
        static bool focus_paused=false;
        if(!focused&&!config::CONFIG.enabled("audio.background")&&player.snapshot().status==openxmb::music::Status::playing) {player.pause();focus_paused=true;}
        if(focused&&focus_paused) {player.play();focus_paused=false;}
        const bool keep_awake=config::CONFIG.enabled("power.keep.awake")&&(player.snapshot().status==openxmb::music::Status::playing || std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->keeps_awake();}));
        static bool awake=false;
        if(awake!=keep_awake) {awake=keep_awake;if(awake) SDL_DisableScreenSaver();else SDL_EnableScreenSaver();
#ifdef _WIN32
            SetThreadExecutionState(ES_CONTINUOUS|(awake?ES_DISPLAY_REQUIRED|ES_SYSTEM_REQUIRED:0));
#endif
        }
        player.set_output_gain(static_cast<float>(config::CONFIG.number("audio.master",100)*config::CONFIG.number("audio.music",100)/10000.0));
        player.set_background_enabled(config::CONFIG.enabled("music.background"));
        player.tick();
        if(background_only) {
            return;
        }

        if(!pending_panel.empty()) {
            const auto panel = std::exchange(pending_panel, {});
            open_panel(panel);
        }
        if(!pending_media.empty()) {
            const auto path = std::exchange(pending_media, {});
            open_media(path);
        }
        for(unsigned int i=0; i<2; i++) {
            if(last_controller_axis_input[i]) {
                auto time_since_input = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_controller_axis_input_time[i]);
                if(time_since_input > std::chrono::milliseconds(static_cast<int>(config::CONFIG.number("input.repeat",180)))) {
                    auto [controller, dir] = *last_controller_axis_input[i];
                    dispatch<events::joystick_axis>(dir, i,
                        controller_axis_position[i].x,
                        controller_axis_position[i].y);
                    last_controller_axis_input_time[i] = std::chrono::steady_clock::now();
                }
            }
        }
        const double button_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(const auto repeated=button_timing.repeat(button_seconds,config::CONFIG.number("input.repeat",180)/1000.0)) {
            for(std::size_t i=0;i<4;++i)if(static_cast<int>(*repeated)==input_bindings::read(config::CONFIG.preferences,true,i))dispatch(event{static_cast<action>(i+1),events::controller_button_down{to_logical_button(static_cast<sdl::GameControllerButton>(*repeated))}});
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

    double shell::startup_elapsed_seconds() const {
        if(win->config.headless && force_headless_startup_overlay()) {
            if(const auto fixed = fixed_boot_seconds_from_environment()) return *fixed;
        }
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - shader_time_zero).count();
        if(!config::CONFIG.enabled("startup.intro")) return elapsed+(config::CONFIG.enabled("startup.warning")?openxmb::xmb::BootMilestones::warning_in_seconds:openxmb::xmb::BootMilestones::complete_seconds);
        return elapsed + (win->config.headless && !force_headless_startup_overlay()
            ? openxmb::xmb::BootMilestones::complete_seconds : 0.0);
    }

    openxmb::xmb::BootSample shell::startup_sample() {
        const auto elapsed=startup_elapsed_seconds();
        auto sample=startup_timeline.sample(elapsed);
        if(!config::CONFIG.enabled("startup.warning") && sample.warning_opacity>0) {static_cast<void>(startup_timeline.dismiss_warning(elapsed));sample=startup_timeline.sample(elapsed);}
        return sample;
    }

    void shell::dismiss_startup_warning(const event& input) {
        if(!startup_timeline.dismiss_warning(startup_elapsed_seconds())) return;
        if(const auto* button = input.get<events::controller_button_down>())
            startup_dismiss_button = button->button;
        if(const auto* key = input.get<events::key_down>())
            startup_dismiss_key = key->keycode;
        spdlog::debug("Startup warning dismissed");
    }

    void shell::dispatch(const event& event) {
        if(event.action!=action::none||event.get<events::key_down>()||event.get<events::controller_button_down>()||event.get<events::mouse_move>()) last_activity=std::chrono::steady_clock::now();
        // A held dismissal must not auto-repeat into the revealed menu.
        if(startup_dismiss_button) {
            if(const auto* up = event.get<events::controller_button_up>();
               up && up->button == *startup_dismiss_button) {
                startup_dismiss_button.reset();
                return;
            }
            if(event.test<events::controller_button_down>([this](const auto& down) {
                return down.button == *startup_dismiss_button;
            })) return;
        }
        if(startup_dismiss_key) {
            if(const auto* up = event.get<events::key_up>();
               up && up->keycode == *startup_dismiss_key) {
                startup_dismiss_key.reset();
                return;
            }
            if(event.test<events::key_down>([this](const auto& down) {
                return down.keycode == *startup_dismiss_key;
            })) return;
        }
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
        cursor_active_at=std::chrono::steady_clock::now();
        cursor_position = glm::clamp(cursor_position + cursor_joystick_delta, glm::vec2{0.0f}, glm::vec2{1.0f});
        dispatch<events::cursor_move>(action::none, cursor_position.x, cursor_position.y);
    }

    bool shell::handle_cursor(const event& event)
    {
        if(auto* d = event.get<events::mouse_move>()) {
            cursor_active_at=std::chrono::steady_clock::now();
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
        const auto& music=openxmb::music::service().snapshot();
        const float quiet=music.status==openxmb::music::Status::playing && config::CONFIG.enabled("audio.duck")?0.22F:1.0F;
        const int effect_volume=static_cast<int>(128*quiet*config::CONFIG.number("audio.master",100)*config::CONFIG.number("audio.effects",100)/10000.0);
        for(auto* sound:{ok_sound.get(),confirm_sound.get(),cancel_sound.get(),back_sound.get(),question_sound.get()}) if(sound)sdl::mix::VolumeChunk(sound,effect_volume);
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
        const auto scan=static_cast<unsigned>(std::to_underlying(key.scancode));if(scan<held_scancodes.size())held_scancodes[scan]=false;
        dispatch(event{
            action::none,
            events::key_up{static_cast<unsigned int>(std::to_underlying(key.scancode))}
        });
    }
    void shell::key_down(sdl::Keysym key) {
        spdlog::trace("Key down: symbol={} scancode={}",key.sym,std::to_underlying(key.scancode));
        const auto scan=static_cast<unsigned>(std::to_underlying(key.scancode));
        const bool held=scan<held_scancodes.size()&&held_scancodes[scan];
        if(scan<held_scancodes.size())held_scancodes[scan]=true;
        for(std::size_t i=0;i<input_bindings::actions.size();++i) if(scan==static_cast<unsigned>(input_bindings::read(config::CONFIG.preferences,false,i))) {
            if(held&&i>=4)return;
            dispatch(event{static_cast<action>(i+1),events::key_down{scan}});return;
        }
        dispatch(event{action::none,events::key_down{scan}});
    }

    void shell::add_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void shell::remove_controller(sdl::GameController* controller)
    {
        button_timing.clear();
        if(config::CONFIG.controllerType == "auto") {
            reload_button_icons();
        }
    }
    void shell::button_down(sdl::GameController* controller,sdl::GameControllerButton button) {
        const auto code=static_cast<unsigned>(button);
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for(std::size_t i=0;i<input_bindings::actions.size();++i)if(static_cast<int>(button)==input_bindings::read(config::CONFIG.preferences,true,i)) {
            if(button_timing.press(code,i<4,seconds))dispatch(event{static_cast<action>(i+1),events::controller_button_down{to_logical_button(button)}});return;
        }
        if(button_timing.press(code,false,seconds))dispatch(event{action::none,events::controller_button_down{to_logical_button(button)}});
    }

    void shell::button_up(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        spdlog::trace("Button up: {}", fmt::underlying(button));
        button_timing.release(static_cast<unsigned>(button));
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
