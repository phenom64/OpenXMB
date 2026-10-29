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
#include <future>
#include <format>
#include <memory>
#include <ranges>
#include <optional>
#include <span>
#include <string>
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
#if defined(_WIN32) || defined(__linux__)
#include <SDL2/SDL_syswm.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <SDL2/SDL_system.h>
#endif

#include <glm/vec2.hpp>

#include "openxmb/xmb/boot_timeline.hpp"
#include "openxmb/xmb/background.hpp"
#include "openxmb/music/player.hpp"
#include "input_bindings.hpp"
#include "components/media_motion.hpp"
#include "../localization/font_selection.hpp"
#include "openxmb/integrations/desktop.hpp"
#include "openxmb/desktop/library.hpp"
#include "openxmb/desktop/session.hpp"
#include "openxmb/desktop/rpcs3_session.hpp"
#include "openxmb/desktop/trophy_watch.hpp"
#include "openxmb/desktop/trophy_sound.hpp"
#include "openxmb/xmb/trophy_notice.hpp"
#include "openxmb/desktop/runners.hpp"
#include "openxmb/desktop/youtube.hpp"
#include "openxmb/desktop/steamgriddb.hpp"
#include "openxmb/desktop/steam.hpp"
#include "openxmb/desktop/async_job.hpp"
#include <atomic>
#include <set>
#include "openxmb/integrations/session_window.hpp"
#include "openxmb/integrations/xmb_overlay.hpp"
#include "openxmb/integrations/guide_input.hpp"
#include "openxmb/integrations/session_hotkey.hpp"
#include "guide_timing.hpp"
#include "overlay_keys.hpp"
#include "components/dialog_text_layout.hpp"
#include "components/trophy_notice_view.hpp"
#include "openxmb/themes/library.hpp"
#include "openxmb/setup/model.hpp"
#include "openxmb/input/prompt.hpp"
#include "keyboard_glyphs.hpp"
#include <charconv>
#include "openxmb/xmb/status_bar.hpp"
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
import :initial_setup;
import :game_presentation;
import :youtube_setup;
import :trophy_collection;
import :game_artwork;
import :session_xmb;
import :game_launch_settings;

using namespace mfk::i18n::literals;

namespace app
{
    namespace {
        std::string controller_prompt_family(sdl::GameController* controller) {
            if(!controller)return "keyboard";
            const auto type=sdl::GameControllerGetType(controller);
            if(type==sdl::GameControllerTypeValues::PS3||type==sdl::GameControllerTypeValues::PS4||type==sdl::GameControllerTypeValues::PS5)return "playstation";
            if(type==sdl::GameControllerTypeValues::XBOX360||type==sdl::GameControllerTypeValues::XBOXONE)return "xbox";
            const auto* raw=sdl::GameControllerName(controller);
            const std::string_view name=raw?raw:"";
            return name=="Steam Virtual Gamepad"||name=="Steam Controller"?"steam":"default";
        }
        void configure_shell_font(auroreengine::font_renderer& font,bool setup) {
            spdlog::debug("Preparing shell font '{}'; setup={}",config::CONFIG.fontPath.string(),setup);
            const auto& assets=config::CONFIG.asset_directory;
            std::vector<std::string> fallbacks;
            for(const auto* name:{"fonts/NotoSans-Regular.ttf","fonts/NotoSansDevanagari-Regular.ttf","fonts/NotoSansCJK-Regular.ttc"}) {
                const auto path=assets/name;
                if(std::filesystem::is_regular_file(path))fallbacks.push_back(path.string());
            }
            font.set_fallback_fonts(std::move(fallbacks));
            font.set_preload_texts(setup?openxmb::localization::setup_font_texts(config::CONFIG.locale_directory):
                openxmb::localization::font_catalog_texts(config::CONFIG.locale_directory,config::CONFIG.language));
        }
    }
    struct shell::game_state {
        struct ArtworkResult {std::filesystem::path root,icon,cover,background;std::string profile,game_id;};
        openxmb::desktop::SessionManager sessions;
        openxmb::desktop::Rpcs3TrophyWatch trophy_watch;
        openxmb::desktop::TrophySound trophy_sound;
        static int audio_percent(const char* key) {
            const auto value=config::CONFIG.number(key,100);
            return std::isfinite(value)?static_cast<int>(std::clamp(value,0.0,100.0)):100;
        }
        std::optional<openxmb::desktop::Rpcs3SessionConfig> rpcs3_config;
        std::string rpcs3_session_id;
        void retire_rpcs3_config() {
            if(!rpcs3_config)return;
            const auto records=sessions.snapshot();
            const auto record=std::ranges::find_if(records,[&](const auto& value){return value.id==rpcs3_session_id;});
            // Shutdown/detached sessions may still use their private files.
            // Only a confirmed process exit authorizes storage retirement.
            if(record!=records.end()&&record->state==openxmb::desktop::SessionState::exited) {
                const auto cleaned=openxmb::desktop::cleanup_rpcs3_session(*rpcs3_config);
                if(!cleaned)spdlog::warn("RPCS3 private snapshot cleanup deferred: {}",cleaned.message);
            }
            rpcs3_config.reset();rpcs3_session_id.clear();
        }
        std::shared_ptr<const openxmb::desktop::Game> session_game;
        std::optional<openxmb::desktop::TrophyWatchSession> trophy_origin;
        std::uint64_t trophy_generation{};
        openxmb::xmb::TrophyNoticeQueue trophy_notices;
        // Artwork for the current notice only. A loaded image is destroyed
        // after a device idle; a load owns its texture until its future ends.
        struct NoticeArt {
            std::uint64_t serial{};std::unique_ptr<texture> image;std::future<void> ready;bool failed{};
            NoticeArt()=default;NoticeArt(const NoticeArt&)=delete;NoticeArt& operator=(const NoticeArt&)=delete;
            ~NoticeArt(){if(ready.valid())ready.wait();}
        };
        NoticeArt trophy_art;
        openxmb::integrations::GuideInput guide;
        openxmb::integrations::SessionHotkey session_hotkey;
        GuideTiming guide_edges;
        std::string guide_session_id;
        std::chrono::steady_clock::time_point guide_probe_at{};
        std::unique_ptr<app::game_presentation> presentation;
        std::unique_ptr<app::session_xmb> overlay;
        // Passive host for trophy notices while the game owns focus; open only
        // while a notice is due so gameplay otherwise renders nothing.
        std::unique_ptr<app::session_xmb> notice;
        float notice_scale{1.0F};
        std::uint64_t notice_unavailable{};
        std::shared_ptr<std::vector<openxmb::integrations::OverlayInput>> overlay_input=std::make_shared<std::vector<openxmb::integrations::OverlayInput>>();
        bool overlay_dismiss{},overlay_return{},overlay_return_after_dismiss{},screenshot_status{};
        std::optional<std::chrono::steady_clock::time_point> screenshot_status_started;
        struct PendingCapture {
            std::string session;openxmb::integrations::SessionWindowTarget target;
            std::chrono::steady_clock::time_point due,deadline;
            std::shared_ptr<openxmb::desktop::AsyncJob<openxmb::integrations::SessionCaptureResult>> job;
        };
        std::optional<PendingCapture> pending_capture;
        std::shared_ptr<std::atomic_bool> capture_worker_busy=std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<const openxmb::desktop::Game> highlighted;
        std::future<openxmb::desktop::GameDiscovery> discovery;
        std::filesystem::path discovery_root;
        std::string discovery_profile;
        std::string discovery_runner_path;
        bool discovery_started = false;
        bool toggle_requested{}, shell_is_session_xmb{}, launched_minimized{}, close_library_pending{}, screenshot_requested{}, minimize_requested{};
        std::string known_session_id, pending_notice, hotkey_setting;
        std::string quitting_session;
        std::optional<std::chrono::steady_clock::time_point> window_unavailable_since;
        std::filesystem::path photo_root;
        std::optional<openxmb::integrations::SessionWindowTarget> target;
        std::uintptr_t shell_handle{};
        bool hotkey_registered{};
        std::optional<std::chrono::steady_clock::time_point> shell_focus_requested;
        openxmb::desktop::TrackingQuality tracking{openxmb::desktop::TrackingQuality::unknown};
        std::chrono::steady_clock::time_point window_probe_at{};
        openxmb::desktop::AsyncJob<ArtworkResult> artwork;
        std::set<std::string> artwork_attempted;
        std::chrono::steady_clock::time_point highlighted_at{};
    };
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
        std::optional<openxmb::setup::CivilTime> shell_civil_time() {
            auto now=openxmb::xmb::wall_clock_now();
            const auto zone_id=config::CONFIG.preference("system.timezone");
            if(!zone_id.empty()) {
                if(!config::CONFIG.enabled("system.clock.automatic"))
                    now+=std::chrono::seconds{static_cast<std::int64_t>(std::clamp(config::CONFIG.number("system.clock.offset-seconds",0),-6311433600.0,6311433600.0))};
                const openxmb::setup::Zone zone{zone_id,{},static_cast<int>(std::clamp(config::CONFIG.number("system.timezone.standard-minutes",0),-720.0,840.0)),true,true};
                return openxmb::setup::local_time(zone,now,config::CONFIG.enabled("system.timezone.daylight"));
            }
            const auto raw=std::chrono::system_clock::to_time_t(now);
            std::tm local{};
#if defined(_WIN32)
            if(localtime_s(&local,&raw)!=0)return {};
#else
            if(localtime_r(&raw,&local)==nullptr)return {};
#endif
            return openxmb::setup::CivilTime{local.tm_year+1900,local.tm_mon+1,local.tm_mday,local.tm_hour,local.tm_min,local.tm_sec};
        }

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

        // Visual-harness preview: this many seconds after launch the background
        // behaves as if the music visualiser had opened, so the transition and
        // the visualiser can be captured without a track or a menu path.
        [[nodiscard]] std::optional<double> preview_music_seconds_from_environment()
        {
            return fixed_seconds_from_environment("OPENXMB_PREVIEW_MUSIC_SECONDS");
        }

        [[nodiscard]] std::optional<double> fixed_boot_seconds_from_environment()
        {
            return fixed_seconds_from_environment("OPENXMB_FIXED_BOOT_SECONDS");
        }

        // Visual-harness input script: "seconds:step;seconds:step". A step is a
        // shell action (left, right, up, down, ok, cancel, options, extra), a
        // controller button (start, select, l1, r1) or a right-stick deflection
        // "rs=x,y". Used only for repeatable before/after captures.
        struct harness_step {
            double seconds{};
            std::string step;
        };

        [[nodiscard]] std::vector<harness_step> harness_steps_from_environment()
        {
            std::vector<harness_step> steps;
            const char* raw = std::getenv("OPENXMB_HARNESS_ACTIONS");
            if(raw == nullptr) {
                return steps;
            }
            std::string_view text{raw};
            while(!text.empty()) {
                const auto end = text.find(';');
                const auto entry = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
                const auto colon = entry.find(':');
                if(colon == std::string_view::npos) {
                    continue;
                }
                double seconds{};
                const auto [stop, error] = fast_float::from_chars(entry.data(), entry.data() + colon, seconds,
                    fast_float::chars_format::general);
                if(error != std::errc{} || stop != entry.data() + colon || seconds < 0.0) {
                    spdlog::warn("Ignoring invalid harness step '{}'", entry);
                    continue;
                }
                steps.push_back({seconds, std::string(entry.substr(colon + 1))});
            }
            std::ranges::stable_sort(steps, {}, &harness_step::seconds);
            return steps;
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

    shell::shell(window* window) : phase(window),theme(std::make_unique<theme_state>()),games(std::make_unique<game_state>())
    {
    }

    shell::~shell()
    {
        // Retire only confirmed exits; retained live-child storage survives.
        try {games->retire_rpcs3_config();} catch(...) {}
#ifdef _WIN32
        if(games->hotkey_registered) UnregisterHotKey(reinterpret_cast<HWND>(games->shell_handle),0x4f58);
        if(games->shell_handle) SDL_SetWindowsMessageHook(nullptr,nullptr);
#endif
        games.reset();
        openxmb::music::service().shutdown();
    }

    void shell::preload()
    {
        phase::preload();
        // Noninteractive captures must not consume a controller used by a
        // separate visible preview. Harness events are dispatched explicitly.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,win->config.headless?"0":"1");
        initial_setup_active=(!win->config.headless&&config::CONFIG.initialSetupRequired)||
            std::getenv("OPENXMB_SETUP_PAGE")||truthy_environment_flag("OPENXMB_FORCE_SETUP");
        font_render = std::make_unique<font_renderer>(config::CONFIG.fontPath.string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
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

        create_render_passes();
        renderPassSamples = win->config.sampleCount;
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

        configure_shell_font(*font_render,initial_setup_active);
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

    void shell::create_render_passes()
    {
        renderPassSamples=win->config.sampleCount;
        renderPassFormat=win->swapchainFormat.format;
        renderPassFinalLayout=win->swapchainFinalLayout;
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
    }

    void shell::preload_fixed_components()
    {
        if(fixed_components_loaded) {
            return;
        }

        games->presentation=std::make_unique<app::game_presentation>(*loader);
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
        if((!win->config.headless && config::CONFIG.initialSetupRequired) ||
           std::getenv("OPENXMB_SETUP_PAGE") || truthy_environment_flag("OPENXMB_FORCE_SETUP")) {
            begin_initial_setup();
        } else if(!win->config.headless || force_headless_startup_overlay()) {
            shader_time_zero = std::chrono::steady_clock::now();
            startup_timeline.reset();
            emplace_overlay<app::startup_overlay>();
        }
        reload_trophy_sound();
        fixed_components_loaded = true;
    }

    void shell::prepare(std::vector<vk::Image> swapchainImages, std::vector<vk::ImageView> swapchainViews)
    {
        phase::prepare(swapchainImages, swapchainViews);
        font_render->set_target_extent(win->swapchainExtent);
        image_render->set_target_extent(win->swapchainExtent);
        simple_render->set_target_extent(win->swapchainExtent);

        const auto imageCount = static_cast<std::uint32_t>(swapchainImages.size());
        this->swapchainImages = swapchainImages;

        framebuffers.clear();
        backgroundFramebuffers.clear();
        blurDescriptorSets.clear();
        blurDescriptorPool.reset();
        blurExtraDescriptorPool.reset();
        backgroundAttachmentViews.clear();backgroundMsaaViews.clear();
        renderImages.clear();
        backgroundResolve.clear();
        backgroundDepth.clear();
        blurFrames.clear();

        // recreateSwapchain has already idled the device. Retire framebuffers
        // first, then rebuild every pipeline against the matching render passes
        // before allocating attachments for the new sample count.
        if(renderPassSamples != win->config.sampleCount || renderPassFormat!=win->swapchainFormat.format || renderPassFinalLayout!=win->swapchainFinalLayout) {
            procedural_wave_render->prepare(0);
            create_render_passes();
            font_render->rebuild_pipelines({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            image_render->preload({backgroundRenderPass.get(), shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            simple_render->preload({shellRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            try {
                monthly_background_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
                monthly_background_failed = false;
            } catch(const std::exception& error) {
                monthly_background_failed = true;
                spdlog::error("Monthly-background Vulkan pipeline initialization failed ({}); using the safe clear-colour fallback", error.what());
            }
#if OPENXMB_ENABLE_VIDEO_PLAYER
            video_background_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
#endif
            procedural_wave_render->preload({backgroundRenderPass.get()}, win->config.sampleCount, win->pipelineCache.get());
            renderPassSamples = win->config.sampleCount;
        }
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
            const auto started=std::chrono::steady_clock::now();
            const std::string& lang = config::CONFIG.language;
            openxmb::localization::shell_locale::apply(lang, config::CONFIG.locale_directory);
            // Setup's translated strings and script fallbacks are already
            // resident. Rebuilding hidden menus or font pipelines stalls input.
            if(!initial_setup_active)reload_menu();
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            spdlog::debug("Language '{}' applied in {:.3f} ms; setup={}, font resources retained",lang,elapsed,initial_setup_active);
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
        // The passive trophy host's framebuffer is only the banner: no shell
        // background, blur or dim, and a transparent clear.
        const bool passive_notice=games->notice&&games->notice->active();
        const bool composited=ingame_mode||passive_notice;

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
        const bool background_video_active=!background_video_path.empty() && !composited &&
            (config::CONFIG.backgroundType==config::config::background_type::video || theme->frame.background==background_video_path);
        video_background_render->set_active(background_video_active);
        video_background_render->tick();
#endif
        menu.prepare_icons();
        commandBuffer.begin(vk::CommandBufferBeginInfo());
        font_render->stage_glyphs(commandBuffer);
        if(games->presentation) games->presentation->upload(commandBuffer,frame,win);
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
            const auto local_time = shell_civil_time().value_or(openxmb::setup::CivilTime{});
            const auto local_hour = static_cast<float>(local_time.hour) +
                static_cast<float>(local_time.minute) / 60.0F +
                static_cast<float>(local_time.second) / 3600.0F;
            const auto day_night_hour =
                utils::xmb_effective_day_night_hour(local_hour);
            const auto original_gradient =
                openxmb::xmb::resolve_background_gradient(
                    local_time.month-1, day_night_hour);
            const auto manual_gradient =
                openxmb::xmb::resolve_manual_background_gradient(
                    {themeColour.base_colour.r, themeColour.base_colour.g,
                     themeColour.base_colour.b},
                    day_night_hour);
            auto background_gradient = config::CONFIG.themeOriginalColour
                ? original_gradient
                : manual_gradient;
            // Startup shows the month's standard colour at full daylight and
            // hands over to the user's colour and time of day as the menu
            // reveals (boot.customise), rather than cutting to them.
            const auto boot = startup_sample();
            const auto startup_gradient =
                openxmb::xmb::resolve_background_gradient(local_time.month-1, 12.0F);
            const float customise = static_cast<float>(boot.customise);
            // xmb-web's Original background lets the calendar-driven gradient
            // carry the hue.  The captured ribbon and dust/glints stay close to
            // neutral silver/white so they read over every month instead of
            // forcing the older OpenXMB theme colour (which made the scene feel
            // stuck on June/cyan).
            const double launch_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-shader_time_zero).count();
            const auto preview_music=preview_music_seconds_from_environment();
            const bool preview_scene=preview_music&&launch_seconds>=*preview_music;
            const bool music_scene=preview_scene||std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->music_background();});
            const bool reactive_scene=preview_scene||std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->music_reactive();});
            const bool flow_scene=reactive_scene&&config::CONFIG.preference("music.visualizer","Wave")=="Flow";
            procedural_wave_render->flow_mode=flow_scene;
            procedural_wave_render->visualizer_time=flow_scene?static_cast<float>(config::CONFIG.enabled("power.reduced.motion",false)?36.0:fixed_wave_seconds_from_environment().value_or(-1.0)):-1.0F;
            // Preserve the departing scene's endpoint until its mix reaches
            // zero; switching endpoints on close made the ribbon jump first.
            if(music_scene)procedural_wave_render->music_vertical=reactive_scene?0.065F:0.34F;
            const auto& music_snapshot=openxmb::music::service().snapshot();
            bool reactive_audio=reactive_scene&&music_snapshot.status==openxmb::music::Status::playing&&!config::CONFIG.enabled("power.reduced.motion",false);
            auto spectrum=music_snapshot.spectrum;
            // OPENXMB_PREVIEW_MUSIC_BEAT feeds the preview a synthetic 120 BPM
            // track (kick, drifting mids and hats) for repeatable captures.
            if(preview_scene&&std::getenv("OPENXMB_PREVIEW_MUSIC_BEAT")) {
                const auto beat=std::fmod(launch_seconds*2.0,1.0);
                const auto kick=static_cast<float>(std::exp(-beat*7.0)*(0.6+0.4*std::sin(launch_seconds*0.4)));
                for(std::size_t i=0;i<spectrum.size();++i)
                    spectrum[i]=i<8?0.06F+0.24F*kick:i<16?static_cast<float>(0.12+0.06*std::sin(launch_seconds*1.3+i)):static_cast<float>(0.06+0.04*std::sin(launch_seconds*7.0+i));
                reactive_audio=true;
            }
            procedural_wave_render->set_music_signal(spectrum,reactive_audio,music_snapshot.generation);
            procedural_wave_render->set_music_mode(music_scene);
            const float music_mix=procedural_wave_render->music_mix();background_gradient.music_mix=music_mix;
            if(flow_scene) {background_gradient.top_rgb={0,0,0};background_gradient.bottom_rgb={0,0,0};background_gradient.music_mix=0;}
            glm::vec3 effect_color=glm::mix(
                glm::vec3(startup_gradient.bottom_rgb[0],startup_gradient.bottom_rgb[1],startup_gradient.bottom_rgb[2]),
                glm::vec3(background_gradient.bottom_rgb[0],background_gradient.bottom_rgb[1],background_gradient.bottom_rgb[2]),customise);
            effect_color=glm::mix(glm::min(effect_color*1.6F,glm::vec3(1.0F)),glm::vec3(1.0F),0.35F);
            // The boot wave emerges neutral silver on black and takes on the
            // theme tint as the background exposes.
            if(boot.background_active)
                effect_color=glm::mix(glm::vec3(0.82F),effect_color,std::clamp(static_cast<float>(boot.background_exposure_top),0.0F,1.0F));
            const glm::vec3 original_effect_tint=glm::mix(effect_color,glm::vec3(192.0F/255,108.0F/255,200.0F/255),music_mix);
            // On the still-black screen the boot wave is about half as bright
            // as once the background exposes (RPCS3 boot).
            const float original_effect_brightness = static_cast<float>(boot.wave_gain)*
                (boot.background_active?0.45F+0.55F*std::clamp(static_cast<float>(boot.background_exposure_top),0.0F,1.0F):1.0F);
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
            if(passive_notice) color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
            else if(ingame_mode) {
                const float visibility=games->overlay&&games->overlay->active()?games->overlay->motion().opacity:1.0F;
                color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.65f*visibility});
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

            if(!composited) {
                if(music_scene||music_mix>0.001F||config::CONFIG.backgroundType == config::config::background_type::original) {
                    if(can_render_monthly_background) {
                        try {
                            const std::array boot_background{
                                static_cast<float>(boot.background_exposure_top),
                                static_cast<float>(boot.background_exposure_bottom),
                                customise,
                                boot.background_active ? 1.0F : 0.0F,
                            };
                            monthly_background_render->render(
                                commandBuffer, backgroundRenderPass.get(),
                                background_gradient, boot_background, &startup_gradient);
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
            if(theme->active && !composited) {
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
            if(games->presentation && !composited) {
                gui_renderer game_gui(commandBuffer,frame,backgroundRenderPass.get(),win->swapchainExtent,font_render.get(),image_render.get(),simple_render.get());
                games->presentation->draw_background(game_gui);
            }
            commandBuffer.endRenderPass();
            image_render->setOutputEncoded(false);
        }
        double blur_background_progress = utils::progress(now, last_blur_background_change, blur_background_transition_duration);
        const auto startup_blur_radius = std::max(0.0f, startup_background_blur_px);
        const bool use_startup_blur_background = startup_blur_radius > 0.05f;
        const bool opaque_scene=std::any_of(overlays.begin(),overlays.end(),[](const auto& overlay){return overlay->is_opaque();});
        const auto menu_blur_radius = background_only||passive_notice ? 0.0F : media_motion::backdrop(menu.background_blur_px(),opaque_scene);
        const auto menu_dim_alpha = background_only||passive_notice ? 0.0F : media_motion::backdrop(menu.background_dim_alpha(),opaque_scene);
        const bool use_menu_blur_background = menu_blur_radius > 0.05F;
        const bool use_blur_background = !passive_notice && (blur_background || blur_background_progress < 1.0 ||
            use_startup_blur_background || use_menu_blur_background);
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
        // One compact banner for every surface. Coordinates are framebuffer
        // pixels of the current target extent; scale maps 1080p typography.
        const float notice_alpha=games->trophy_notices.opacity(app::session_xmb::seconds(),config::CONFIG.enabled("power.reduced.motion",false));
        const auto draw_trophy_notice=[&](float x,float y,float w,float h,float scale) {
            const auto* entry=games->trophy_notices.current();
            if(!entry||notice_alpha<=0.0F)return;
            const auto& earned=entry->event.achievement;
            const auto& art=games->trophy_art;
            const bool art_slot=art.serial==entry->serial&&!earned.icon.empty()&&!art.failed;
            // loaded is read only after the upload future has ended.
            const texture* const image=art_slot&&art.image&&!art.ready.valid()&&art.image->loaded?art.image.get():nullptr;
            const auto label=::openxmb::localization::shell_locale::translate("RPCS3 trophy earned");
            const auto grade=::openxmb::localization::shell_locale::translate(trophy_detail::grade(earned.grade));
            trophy_notice_view::draw(renderer,trophy_notice_view::Notice<texture,glm::vec4>{label,grade,trophy_detail::grade_colour(earned.grade,notice_alpha),earned.title,art_slot,image},
                notice_alpha,x,y,w,h,scale);
        };
        if(games->notice&&games->notice->active()) {
            // The passive host's framebuffer is exactly the banner.
            draw_trophy_notice(0,0,static_cast<float>(renderer.frame_size.width),static_cast<float>(renderer.frame_size.height),games->notice_scale);
            return;
        }
        const bool session_transition=ingame_mode&&games->overlay&&games->overlay->active();
        if(session_transition) {
            const auto motion=games->overlay->motion();
            renderer.push_color(glm::vec4(motion.opacity));
            renderer.push_zoom(motion.zoom);
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
        // The crossbar cross-fade belongs to the first message over the menu
        // only. A message stacked on another opaque screen (Information ->
        // icon chooser, name prompt -> result) must not reveal the crossbar,
        // and the screen left behind when the top one closes stays as it is.
        const bool opaque_below_top = has_overlay &&
            std::any_of(overlays.begin(), overlays.end() - 1, [](const auto& overlay) { return overlay->is_opaque(); });
        const bool shell_fade_transition = overlay_transition && top_uses_shell_fade &&
            overlay_fade_direction == transition_direction::in && !opaque_below_top;
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
            const bool pushed_zoom = (top_is_message || fading_out_message) && overlay_fade_zoom < 0.999F;
            if(pushed_zoom) {
                float scale = static_cast<float>(glm::mix(1.0, static_cast<double>(overlay_fade_zoom), dir_progress));
                renderer.push_zoom(scale);
            }
            // A keyboard opened over the crossbar (Internet Search) makes the
            // menu recede and fade while the wave and clock stay unchanged.
            // Captured once: icons ~10% smaller about the screen centre and
            // roughly a third as opaque.
            const float recede = render_menu && has_overlay ? overlays.back()->menu_recede() : 0.0F;
            if(recede > 0.0F) {
                renderer.push_color(glm::vec4(1.0F - 0.65F * recede));
                if(!pushed_zoom) renderer.push_zoom(1.0F - 0.1F * recede);
            }
            // Render the entire XMB UI (menu + time + news) within the zoom scope
            menu.render(renderer);
            if(recede > 0.0F) {
                if(!pushed_zoom) renderer.pop_zoom();
                renderer.pop_color();
            }

            const auto text_color = renderer.text_color;
            renderer.text_color = glm::vec4(1.0F);
            // The clock box disappeared while an options or choice panel was
            // open (Users > Triangle and Settings > Colour captures).
            const float clock_alpha = has_choice_overlay() ? 0.0F : 1.0F - menu.options_visibility();
            renderer.push_color(glm::vec4(config::CONFIG.clockColor, clock_alpha));
            if(!render_status_clock(renderer)) {
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

        if(games->screenshot_status && overlays.empty()) {
            if(!games->screenshot_status_started)games->screenshot_status_started=now;
            if(now-*games->screenshot_status_started<std::chrono::seconds(3)) {
                const auto width=static_cast<float>(renderer.frame_size.width),height=static_cast<float>(renderer.frame_size.height);
                const auto scale=std::min(width/1920.0F,height/1080.0F);
                renderer.draw_rect({0.70F,0.13F},{0.27F,0.06F},{0,0,0,0.65F});
                renderer.draw_text_pixels(::openxmb::localization::shell_locale::translate("Screenshot saved"),width*0.715F,height*0.170F,26*scale,{1,1,1,1},0.7F);
            } else {games->screenshot_status=false;games->screenshot_status_started.reset();}
        }
        if(games->trophy_notices.started()&&!games->screenshot_status&&overlays.empty()&&!old_overlay) {
            const auto width=static_cast<float>(renderer.frame_size.width),height=static_cast<float>(renderer.frame_size.height);
            const auto& frame=openxmb::xmb::trophy_notice_frame;
            draw_trophy_notice(width*frame.x,height*frame.y,width*frame.width,height*frame.height,std::min(width/1920.0F,height/1080.0F));
        }
        bool enable_cursor = false;
        for(unsigned int i=overlay_begin; i < overlays.size(); i++) {
            renderer.text_scale = static_cast<float>(config::CONFIG.textScale) * overlays[i]->typography_scale();
            if(i == overlays.size()-1 && shell_fade_transition &&
               !(overlay_fade_direction == transition_direction::in && overlays[i]->staged_entrance())) {
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
            static_cast<void>(render_status_clock(renderer));
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

        if(session_transition){renderer.pop_zoom();renderer.pop_color();}
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

    void shell::highlight_game(std::shared_ptr<const openxmb::desktop::Game> game) {
        if(!game || !games->highlighted || game->id!=games->highlighted->id) {
            games->highlighted_at=std::chrono::steady_clock::now();
            games->artwork.cancel();
        }
        games->highlighted=std::move(game);
    }

    const texture* shell::game_preview_icon(std::string_view identity) const {
        return games->presentation ? games->presentation->animated_icon(identity) : nullptr;
    }

    result shell::launch_game(const openxmb::desktop::Game& game) {
        if(!openxmb::desktop::steam_game_is_playable(game))return result::success;
        if(games->sessions.active_session()) {
            emplace_overlay<app::message_overlay>("Game running", "Quit the current game before starting another.", std::vector<std::string>{"OK"});
            return result::success;
        }
        try {
            games->retire_rpcs3_config();
            openxmb::desktop::Library library;
            openxmb::desktop::SessionDescriptor descriptor;
            descriptor.identity=game.id; descriptor.title=game.title;
            descriptor.provider=game.platform.empty()?"native":game.platform;
            const auto launch_target=openxmb::desktop::launch_game_target(game);
            const auto plan=openxmb::desktop::plan_game_launch(launch_target,openxmb::desktop::runner_options_from_preferences(config::CONFIG.preferences));
            if(!plan) throw std::runtime_error(plan.message);
            descriptor.runner=plan.runner;
            descriptor.profile_directory=library.active_directory();
            std::filesystem::path profile_directory;
            descriptor.tracking=plan.tracking;
#if defined(_WIN32) || defined(__linux__)
            if(game.runner=="steam" && game.launch_override.mode==openxmb::desktop::GameLaunchMode::provider) {
                openxmb::desktop::ProviderObservationSpec observation;
                observation.executable=game.launch_override.executable;
                observation.installation_directory=game.game_directory.empty()?game.working_directory:game.game_directory;
                const auto id_text=game.content.generic_string();
                const auto id=std::from_chars(id_text.data(),id_text.data()+id_text.size(),observation.steam_app_id);
                if(id.ec==std::errc{}&&id.ptr==id_text.data()+id_text.size()&&observation.steam_app_id&&(!observation.executable.empty()||!observation.installation_directory.empty()))
                    descriptor.observation=std::move(observation);
            }
#endif
            auto arguments=plan.arguments;
            std::map<std::string,std::string> environment;
            std::vector<std::uintptr_t> inherited_handles;
            std::optional<openxmb::desktop::Rpcs3SessionConfig> rpcs3_config;
            if(plan.runner=="rpcs3") {
                profile_directory=std::filesystem::canonical(descriptor.profile_directory);
                openxmb::desktop::Rpcs3SessionRequest request;
                request.executable=std::filesystem::u8path(arguments.front());
                request.private_directory=profile_directory/
                    ("rpcs3-session-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                const auto user=config::CONFIG.preference("trophies.rpcs3.user");
                if(!user.empty())request.user_id=user;
                const auto prepared=openxmb::desktop::prepare_rpcs3_session(request);
                if(prepared) {
                    rpcs3_config=*prepared.config;
                    if(!rpcs3_config->warm_cache_directory.empty())spdlog::debug("RPCS3 managed session reuses its original recompiler cache");
                    else if(!rpcs3_config->cache_fallback_reason.empty())spdlog::warn("RPCS3 managed cache fallback: {}",rpcs3_config->cache_fallback_reason);
                    environment=rpcs3_config->environment_overrides;
                    inherited_handles=rpcs3_config->inherited_handles;
                    arguments.insert(arguments.begin()+1,rpcs3_config->arguments.begin(),rpcs3_config->arguments.end());
                } else if(openxmb::desktop::rpcs3_session_native_fallback(prepared.error)) {
                    spdlog::warn("RPCS3 managed session unavailable; native confirmation remains: {}",prepared.message);
                } else throw std::runtime_error(prepared.message);
            }
            const auto launched=games->sessions.launch(std::move(descriptor),arguments,plan.working_directory,environment,inherited_handles);
            if(!launched) throw std::runtime_error(launched.message);
            if(rpcs3_config)static_cast<void>(openxmb::desktop::retain_rpcs3_session(*rpcs3_config));
            games->rpcs3_config=std::move(rpcs3_config);
            games->rpcs3_session_id=games->rpcs3_config?launched.session_id:std::string{};
            games->session_game=std::make_shared<const openxmb::desktop::Game>(game);
            games->trophy_watch.set_session({});games->trophy_origin.reset();
            games->trophy_sound.stop();games->trophy_notices.clear();
            if(games->rpcs3_config&&!game.game_directory.empty()) {
                games->trophy_origin=openxmb::desktop::TrophyWatchSession{++games->trophy_generation,launched.session_id,
                    profile_directory,game.game_directory,
                    games->rpcs3_config->dev_hdd0,games->rpcs3_config->user_id};
                games->trophy_watch.set_session(games->trophy_origin);
            }
            games->known_session_id=launched.session_id;queue_menu_reload();
            games->tracking=plan.tracking;
            games->photo_root=config::CONFIG.pictures_path();
            games->close_library_pending=true;
            games->shell_is_session_xmb=false;
            if(games->presentation) games->presentation->stop();
            games->highlighted.reset();
            try { library.mark_played(game.id); }
            catch(const std::exception& error) { spdlog::warn("Game started, but library history could not be saved: {}",error.what()); }
            games->launched_minimized=config::CONFIG.preference("games.launch","Minimise")=="Minimise";
            games->minimize_requested=games->launched_minimized;
            return result::success;
        } catch(const std::exception& error) {
            emplace_overlay<app::message_overlay>("Cannot start game",error.what(),std::vector<std::string>{"OK"});
            return result::success;
        }
    }

    void shell::show_game_information(const openxmb::desktop::Game& game) {
        const auto history=games->sessions.history(openxmb::desktop::Library{}.active_directory(),game.id);
        std::string text=game.description;
        if(!game.platform.empty()) text+="\n"+game.platform;
        if(!game.title_id.empty()) text+="\n"+game.title_id;
        text+="\nPlay time: "+std::to_string(history.total_playtime_ms/60000)+" minutes";
        if(history.provider_observed_playtime_ms)text+="\nObserved Steam time: "+std::to_string(history.provider_observed_playtime_ms/60000)+" minutes";
        if(!history.error.empty()) text+="\n"+history.error;
        emplace_overlay<app::message_overlay>(game.title,text,std::vector<std::string>{"OK"});
    }
    void shell::show_game_artwork(const openxmb::desktop::Game& game) {
        emplace_overlay<app::game_artwork>(*loader,game);
    }

    void shell::show_game_launch_settings(const openxmb::desktop::Game& game) {
        emplace_overlay<app::game_launch_settings>(game);
    }

    result shell::launch_application(std::string identity,std::string title,const std::vector<std::string>& argv,const std::filesystem::path& cwd,std::string runner,openxmb::desktop::TrackingQuality tracking) {
        if(has_running_session()) {
            emplace_overlay<app::message_overlay>("Application running","Quit the current game or app before starting another.",std::vector<std::string>{"OK"});return result::success;
        }
        try {
            games->retire_rpcs3_config();
            openxmb::desktop::Library library;
            openxmb::desktop::SessionDescriptor descriptor;
            descriptor.identity=std::move(identity);descriptor.title=std::move(title);descriptor.provider="application";
            descriptor.runner=std::move(runner);descriptor.profile_directory=library.active_directory();
            descriptor.kind=openxmb::desktop::SessionKind::application;descriptor.tracking=tracking;
            const auto launched=games->sessions.launch(std::move(descriptor),argv,cwd);
            if(!launched)throw std::runtime_error(launched.message);
            games->trophy_watch.set_session({});games->trophy_origin.reset();
            games->trophy_sound.stop();games->trophy_notices.clear();
            games->rpcs3_config.reset();games->session_game.reset();
            games->known_session_id=launched.session_id;games->tracking=tracking;
            games->photo_root=config::CONFIG.pictures_path();games->highlighted.reset();
            if(games->presentation)games->presentation->stop();
            games->shell_is_session_xmb=false;games->close_library_pending=true;
            games->launched_minimized=true;games->minimize_requested=true;queue_menu_reload();
        } catch(const std::exception& error) {emplace_overlay<app::message_overlay>("Cannot start application",error.what(),std::vector<std::string>{"OK"});}
        return result::success;
    }

    void shell::open_youtube() {
        openxmb::desktop::YoutubeDetectionOptions options;
        options.data_root=openxmb::desktop::default_data_root();
        const auto override=config::CONFIG.preference("youtube.executable");
        if(!override.empty())options.executable_override=std::filesystem::u8path(override);
        const auto detected=openxmb::desktop::detect_youtube(options);
        if(detected) {
            auto arguments=detected.value->arguments;
            // Native acceptance uses the client's supported portable profile,
            // so it never reads or updates a personal YouTube login/profile.
            if(const auto* profile=std::getenv("OPENXMB_HARNESS_YOUTUBE_PROFILE");profile&&*profile)
                arguments.emplace_back("--portable="+std::string(profile));
            static_cast<void>(launch_application("youtube","YouTube",arguments,detected.value->executable.parent_path(),"youtube",detected.value->tracking));return;
        }
#ifdef _WIN32
        emplace_overlay<app::message_overlay>("Set Up YouTube","Download and set up the controller-friendly YouTube app? You can stay in OpenXMB while it gets ready.",std::vector<std::string>{"Not Now","Set Up"},[this,root=options.data_root](unsigned index) {
            if(index)emplace_overlay<app::youtube_setup>(root);
        });
#else
        emplace_overlay<app::message_overlay>("YouTube","A supported YouTube app was not found. Automatic setup is not available in this build. Select an installed executable in application settings.",std::vector<std::string>{"OK"});
#endif
    }

    void shell::show_youtube_information() {
        openxmb::desktop::YoutubeDetectionOptions options;options.data_root=openxmb::desktop::default_data_root();
        const auto override=config::CONFIG.preference("youtube.executable");if(!override.empty())options.executable_override=std::filesystem::u8path(override);
        const auto detected=openxmb::desktop::detect_youtube(options);
        auto text=std::string(openxmb::desktop::youtube_information);
        if(detected)text+="\nVersion: "+(detected.value->version.empty()?std::string{"Unknown"}:detected.value->version);
        else text+="\nNot installed";
        emplace_overlay<app::message_overlay>("YouTube",text,std::vector<std::string>{"OK"});
    }

    bool shell::input_focused() const {
        if(win->config.headless)return true;
        if(games->overlay && games->overlay->active())return games->overlay->focused();
#ifdef _WIN32
        // Restoring Vulkan presentation can leave SDL's cached focus flag set
        // until its next window event. Use the actual foreground owner so a
        // Guide/shortcut immediately after dismissal opens the XMB again.
        if(games->shell_handle)return GetForegroundWindow()==reinterpret_cast<HWND>(games->shell_handle);
#endif
        return (SDL_GetWindowFlags(win->win.get())&SDL_WINDOW_INPUT_FOCUS)!=0;
    }
    bool shell::has_running_session() const { return games->sessions.active_session().has_value(); }
    bool shell::session_is_application() const {
        const auto active=games->sessions.active_session();
        return active && active->descriptor.kind==openxmb::desktop::SessionKind::application;
    }
    std::string shell::session_title() const {
        const auto active=games->sessions.active_session();
        return active?active->descriptor.title:std::string{};
    }
    void shell::toggle_session_xmb() { games->toggle_requested=!games->toggle_requested; }
    void shell::save_session_screenshot() { spdlog::debug("Session screenshot requested");games->screenshot_requested=true; }
    void shell::quit_session() {
        const auto active=games->sessions.active_session();
        if(!active) return;
        if(active->awaiting_observation){static_cast<void>(games->sessions.quit(active->id));return;}
        if(!active->capabilities.graceful_close) {
            emplace_overlay<app::message_overlay>("Session unavailable","This launcher does not expose the running application. Return to the application to quit it safely.",std::vector<std::string>{"OK"});return;
        }
        const auto title=active->descriptor.kind==openxmb::desktop::SessionKind::application?"Quit App":"Quit Game";
        emplace_overlay<app::message_overlay>(title,"Quit "+active->descriptor.title+"?",std::vector<std::string>{"No","Yes"},[this,id=active->id](unsigned index) {
            if(!index)return;
            const auto stopped=games->sessions.quit(id);
            if(!stopped) games->pending_notice=stopped.message;
            else games->quitting_session=id;
        });
    }

    void shell::after_events() {
        if(std::exchange(input_prompt_refresh,false))reload_button_icons();
        if(std::exchange(games->minimize_requested,false) && !win->config.headless) SDL_MinimizeWindow(win->win.get());
        const auto shell_focused=[&] {return input_focused();};
        if(!win->config.headless && !shell_focused()) {
            last_controller_axis_input={};controller_axis_position={};cursor_joystick_delta={};button_timing.clear();held_scancodes.fill(false);
        }
        const auto close_notice=[&] {
            if(!games->notice || !games->notice->active())return;
            if(const auto closed=games->notice->close();!closed)spdlog::warn("Trophy notice presentation could not close: {}",closed.message);
        };
        // Every return to a focusable XMB or normal window first retires the
        // passive host, whose offscreen presentation would otherwise persist.
        const auto close_overlay=[&] {
            close_notice();
            if(games->overlay && games->overlay->active()) {
                const auto closed=games->overlay->close();
                if(!closed) {games->pending_notice=closed.message;return false;}
            }
            games->overlay_input->clear();games->overlay_dismiss=false;games->overlay_return=false;games->overlay_return_after_dismiss=false;
            set_ingame_mode(false);games->shell_is_session_xmb=false;held_scancodes.fill(false);button_timing.clear();
            return true;
        };
        const auto restore_shell=[&] {
            if(!close_overlay())return;
            SDL_RestoreWindow(win->win.get());SDL_RaiseWindow(win->win.get());
            games->shell_is_session_xmb=shell_focused();
            if(!games->shell_is_session_xmb) games->shell_focus_requested=std::chrono::steady_clock::now();
        };
        // Physical-pixel bounds of the monitor showing the session window.
        const auto game_monitor=[&](const openxmb::integrations::SessionWindowTarget& target)->std::optional<openxmb::integrations::OverlayBounds> {
#ifdef _WIN32
            MONITORINFO info{};info.cbSize=sizeof(info);
            const auto monitor=MonitorFromWindow(reinterpret_cast<HWND>(target.handle),MONITOR_DEFAULTTONEAREST);
            if(GetMonitorInfoW(monitor,&info))
                return openxmb::integrations::OverlayBounds{info.rcMonitor.left,info.rcMonitor.top,
                    static_cast<unsigned>(info.rcMonitor.right-info.rcMonitor.left),static_cast<unsigned>(info.rcMonitor.bottom-info.rcMonitor.top)};
#elif defined(__linux__)
            if(const auto client=openxmb::integrations::session_window_bounds(target)) {
                const auto center_x=client->x+static_cast<int>(client->width/2),center_y=client->y+static_cast<int>(client->height/2);
                for(int display=0;display<SDL_GetNumVideoDisplays();++display) {
                    SDL_Rect bounds{};
                    if(SDL_GetDisplayBounds(display,&bounds)==0&&center_x>=bounds.x&&center_x<bounds.x+bounds.w&&center_y>=bounds.y&&center_y<bounds.y+bounds.h)
                        return openxmb::integrations::OverlayBounds{bounds.x,bounds.y,static_cast<unsigned>(bounds.w),static_cast<unsigned>(bounds.h)};
                }
            }
#else
            static_cast<void>(target);
#endif
            return std::nullopt;
        };
        if(games->shell_focus_requested) {
            if(shell_focused()) {games->shell_is_session_xmb=true;games->shell_focus_requested.reset();}
            else if(std::chrono::steady_clock::now()-*games->shell_focus_requested>std::chrono::seconds(1)) {
                games->shell_focus_requested.reset();
                games->pending_notice="Your system did not give OpenXMB focus. Select its window using the system window switcher.";
            }
        }
        auto active=games->sessions.active_session();
        if(games->trophy_origin) {
            const auto selected_user=config::CONFIG.preference("trophies.rpcs3.user");
            bool current=active&&active->id==games->trophy_origin->session_id&&
                (selected_user.empty()||selected_user==games->trophy_origin->rpcs3_user_id);
            auto events=games->trophy_watch.take_events();
            if(current&&!events.empty())try {
                current=std::filesystem::canonical(openxmb::desktop::Library{}.active_directory())==games->trophy_origin->profile_directory&&
                    (selected_user.empty()||selected_user==games->trophy_origin->rpcs3_user_id);
            } catch(const std::exception&) {current=false;}
            if(!current) {
                games->trophy_watch.set_session({});games->trophy_origin.reset();
                games->trophy_sound.stop();games->trophy_notices.clear();
                if(!active){games->retire_rpcs3_config();games->session_game.reset();}
            } else for(auto& event:events) {
                static_cast<void>(games->trophy_notices.push(std::move(event),*games->trophy_origin));
            }
        } else if(!active) {games->retire_rpcs3_config();games->session_game.reset();}
        // This optional native listener supplements SDL while another process
        // owns focus. Runtime absence keeps the existing shortcut/SDL path.
        if(!win->config.headless && active) {
            const auto now=std::chrono::steady_clock::now();
            if(games->guide_session_id!=active->id) {
                games->guide.stop();games->guide_edges.reset();games->guide_session_id=active->id;games->guide_probe_at={};
            }
            if(!games->guide.running() && now>=games->guide_probe_at) {
                if(games->guide.start())spdlog::debug("Native Guide listener registered for session");
                games->guide_probe_at=now+std::chrono::seconds(2);
            }
            for(const auto& edge:games->guide.drain()) {
                if(games->guide_edges.edge(GuideTiming::Source::native,edge.pressed,edge.steady_timestamp_us))toggle_session_xmb();
            }
        } else if(!games->guide_session_id.empty()) {
            games->guide.stop();games->guide_edges.reset();games->guide_session_id.clear();
        }
        if(active && active->descriptor.kind==openxmb::desktop::SessionKind::application && active->descriptor.runner=="youtube" && active->descriptor.tracking!=openxmb::desktop::TrackingQuality::direct_child &&
           std::chrono::steady_clock::now()-games->window_probe_at>std::chrono::milliseconds(100)) {
            games->window_probe_at=std::chrono::steady_clock::now();
            if(const auto target=openxmb::integrations::find_session_window(active->process_id))
                if(games->sessions.confirm_application_window(active->id,*target)) {games->target=target;games->tracking=openxmb::desktop::TrackingQuality::direct_child;active=games->sessions.active_session();}
        }
        if(active) {
            games->tracking=active->descriptor.tracking;
            games->target=active->window;
        }
        menu.refresh_session_entries(bool(active),active&&active->descriptor.kind==openxmb::desktop::SessionKind::application,
            active?std::string_view(active->descriptor.title):std::string_view{});
        const auto identity=active?active->id:std::string{};
        if(identity!=games->known_session_id) {
            const bool ended=!games->known_session_id.empty() && identity.empty();
            games->known_session_id=identity;
            games->quitting_session.clear();games->window_unavailable_since.reset();
            if(!active)games->target.reset();queue_menu_reload();
            if(ended) {
                games->pending_capture.reset();
                close_overlay();
                if(games->launched_minimized && (games->tracking==openxmb::desktop::TrackingQuality::direct_child || games->tracking==openxmb::desktop::TrackingQuality::provider_observed) && !win->config.headless) restore_shell();
                games->shell_is_session_xmb=false;games->launched_minimized=false;
            }
        }
#ifdef _WIN32
        if(!win->config.headless && !games->shell_handle) {
            SDL_SysWMinfo info{};SDL_VERSION(&info.version);
            if(SDL_GetWindowWMInfo(win->win.get(),&info) && info.subsystem==SDL_SYSWM_WINDOWS) {
                games->shell_handle=reinterpret_cast<std::uintptr_t>(info.info.win.window);
                SDL_SetWindowsMessageHook([](void* context,void* hwnd,unsigned message,Uint64 parameter,Sint64) {
                    auto* host=static_cast<shell*>(context);
                    if(message==WM_HOTKEY && parameter==0x4f58 && reinterpret_cast<std::uintptr_t>(hwnd)==host->games->shell_handle) {
                        spdlog::debug("In-game XMB keyboard shortcut received");
                        host->toggle_session_xmb();
                    }
                },this);
            }
        }
        const auto chord=active?config::CONFIG.preference("session.hotkey","Ctrl+Shift+F1"):std::string{};
        if(games->shell_handle && chord!=games->hotkey_setting) {
            const auto hwnd=reinterpret_cast<HWND>(games->shell_handle);
            if(games->hotkey_registered) UnregisterHotKey(hwnd,0x4f58);
            games->hotkey_registered=false;games->hotkey_setting=chord;
            if(!chord.empty()) {
                unsigned key=VK_F1;
                if(chord=="Ctrl+Shift+F2")key=VK_F2;else if(chord=="Ctrl+Shift+F3")key=VK_F3;else if(chord=="Ctrl+Shift+F4")key=VK_F4;
                games->hotkey_registered=RegisterHotKey(hwnd,0x4f58,MOD_CONTROL|MOD_SHIFT|MOD_NOREPEAT,key)!=FALSE;
                if(!games->hotkey_registered) games->pending_notice="The In-Game XMB shortcut is already in use. Choose another shortcut in Game Settings. Guide may also work when the system allows it.";
            }
        }
#endif
#ifdef __linux__
        if(!win->config.headless && !games->shell_handle) {
            SDL_SysWMinfo info{};SDL_VERSION(&info.version);
            if(SDL_GetWindowWMInfo(win->win.get(),&info)&&info.subsystem==SDL_SYSWM_X11)
                games->shell_handle=static_cast<std::uintptr_t>(info.info.x11.window);
        }
        const auto chord=active&&!win->config.headless?config::CONFIG.preference("session.hotkey","Ctrl+Shift+F1"):std::string{};
        if(chord!=games->hotkey_setting) {
            games->session_hotkey.stop();games->hotkey_setting=chord;
            if(!chord.empty()) {
                unsigned function_key=1;
                if(chord=="Ctrl+Shift+F2")function_key=2;
                else if(chord=="Ctrl+Shift+F3")function_key=3;
                else if(chord=="Ctrl+Shift+F4")function_key=4;
                const auto registered=games->session_hotkey.configure(function_key);
                if(!registered)games->pending_notice=registered.message;
            }
        }
        if(games->session_hotkey.take_presses()%2)toggle_session_xmb();
#endif
        if(active && games->overlay && games->overlay->active() && (!active->capabilities.focus || !active->window)) {
            // Windows are destroyed before their owning process signals exit.
            // Allow normal/manual shutdown to settle before treating a missing
            // window as an unavailable compositor target.
            const auto now=std::chrono::steady_clock::now();
            if(!games->window_unavailable_since)games->window_unavailable_since=now;
            if(games->quitting_session==active->id || now-*games->window_unavailable_since>=std::chrono::seconds(1)) {
                const bool expected=games->quitting_session==active->id;
                restore_shell();
                if(!expected)games->pending_notice="The game window is no longer available. OpenXMB has returned to its normal window.";
            }
        } else games->window_unavailable_since.reset();
        if(games->overlay && games->overlay->active()) {
            if(const auto error=games->overlay->take_error();!error.empty()) {
                spdlog::warn("Transparent XMB unavailable: {}",error);
                restore_shell();queue_menu_reload();
                games->pending_notice="The transparent XMB is unavailable for this window. OpenXMB has switched to its normal window.";
            }
        }
        if(games->overlay && games->overlay->active()) {
            auto inputs=std::exchange(*games->overlay_input,{});
            for(const auto& input:inputs) {
                using enum openxmb::integrations::OverlayInputKind;
                if(input.kind==close || input.kind==focus_lost) {
                    games->overlay_dismiss=true;games->overlay_return=input.kind==close;continue;
                }
                if(input.kind==key_down || input.kind==key_up) {
                    SDL_Keycode code=SDLK_UNKNOWN;
#ifdef _WIN32
                    if(input.virtual_key>='A' && input.virtual_key<='Z')code=static_cast<SDL_Keycode>(input.virtual_key-'A'+'a');
                    else if(input.virtual_key>='0' && input.virtual_key<='9')code=static_cast<SDL_Keycode>(input.virtual_key);
                    else if(input.virtual_key>=VK_F1 && input.virtual_key<=VK_F12)code=SDLK_F1+static_cast<int>(input.virtual_key-VK_F1);
                    else switch(input.virtual_key) {
                        case VK_LEFT:code=SDLK_LEFT;break;case VK_RIGHT:code=SDLK_RIGHT;break;
                        case VK_UP:code=SDLK_UP;break;case VK_DOWN:code=SDLK_DOWN;break;
                        case VK_RETURN:code=(input.scan_code&0x100)?SDLK_KP_ENTER:SDLK_RETURN;break;
                        case VK_ESCAPE:code=SDLK_ESCAPE;break;case VK_TAB:code=SDLK_TAB;break;
                        case VK_SPACE:code=SDLK_SPACE;break;case VK_BACK:code=SDLK_BACKSPACE;break;
                        case VK_DELETE:code=SDLK_DELETE;break;case VK_INSERT:code=SDLK_INSERT;break;
                        case VK_HOME:code=SDLK_HOME;break;case VK_END:code=SDLK_END;break;
                        case VK_PRIOR:code=SDLK_PAGEUP;break;case VK_NEXT:code=SDLK_PAGEDOWN;break;
                        case VK_CAPITAL:code=SDLK_CAPSLOCK;break;
                        default:break;
                    }
#elif defined(__linux__)
                    code=x11_overlay_key(input.virtual_key);
#endif
                    if(code!=SDLK_UNKNOWN) {
                        SDL_Keysym key{};key.sym=code;key.scancode=SDL_GetScancodeFromKey(code);
#ifdef _WIN32
                        if(input.modifiers&MOD_CONTROL)key.mod|=KMOD_CTRL;
                        if(input.modifiers&MOD_SHIFT)key.mod|=KMOD_SHIFT;
                        if(input.modifiers&MOD_ALT)key.mod|=KMOD_ALT;
#elif defined(__linux__)
                        key.mod=x11_overlay_modifiers(input.modifiers);
#endif
                        if(input.kind==key_down)this->key_down(key);else this->key_up(key);
                    }
                } else if(input.kind==mouse_move) {
                    dispatch(event{action::none,events::mouse_move{static_cast<float>(input.x)/win->swapchainExtent.width,
                        static_cast<float>(input.y)/win->swapchainExtent.height,0,0}});
                } else if(input.kind==mouse_down || input.kind==mouse_up) {
#ifdef _WIN32
                    const auto button=to_logical_mouse_button(input.virtual_key==VK_LBUTTON?SDL_BUTTON_LEFT:input.virtual_key==VK_RBUTTON?SDL_BUTTON_RIGHT:SDL_BUTTON_MIDDLE);
#else
                    const auto native_button=x11_overlay_button(input.virtual_key);
                    if(!native_button)continue;
                    const auto button=to_logical_mouse_button(native_button);
#endif
                    if(input.kind==mouse_down)dispatch(event{action::none,events::mouse_button_down{button}});
                    else dispatch(event{action::none,events::mouse_button_up{button}});
                }
            }
        }
        const bool toggle=std::exchange(games->toggle_requested,false);
        if(toggle)spdlog::debug("In-game XMB toggle: session={}, overlay={}, focused={}",bool(active),games->overlay && games->overlay->active(),shell_focused());
        if(toggle)games->pending_capture.reset();
        const bool dismiss=games->overlay_dismiss;
        const bool return_to_game=games->overlay_return || toggle;
        if((toggle || dismiss) && active && !win->config.headless) {
            if(games->overlay && games->overlay->active()) {
                if(toggle&&games->overlay->closing())games->overlay->reopen(config::CONFIG.enabled("power.reduced.motion",false));
                else {
                    games->overlay->dismiss(config::CONFIG.enabled("power.reduced.motion",false));
                    games->overlay_return_after_dismiss=return_to_game;
                }
                games->overlay_dismiss=false;games->overlay_return=false;
            } else if(!shell_focused()) {
                bool opened=false;
                // Promotion: the passive notice gives the offscreen presentation
                // to the interactive XMB, which keeps drawing the same notice.
                close_notice();
                if(active->capabilities.focus && active->window) {
                    games->target=active->window;
                    if(games->target && openxmb::integrations::validate_session_window(*games->target)) {
                        const auto overlay_bounds=game_monitor(*games->target);
                        if(overlay_bounds) {
                            if(!games->overlay)games->overlay=std::make_unique<app::session_xmb>(win);
                            set_ingame_mode(true);
                            const auto prepared=games->overlay->open(*overlay_bounds,
                                [queue=games->overlay_input](const openxmb::integrations::OverlayInput& input) {
                                    if(queue->size()<256)queue->push_back(input);
                                    else if(input.kind==openxmb::integrations::OverlayInputKind::focus_lost || input.kind==openxmb::integrations::OverlayInputKind::close) {
                                        queue->clear();queue->push_back(input);
                                    }
                                },config::CONFIG.enabled("power.reduced.motion",false));
                            if(prepared) {
                                SDL_MinimizeWindow(win->win.get());games->shell_is_session_xmb=true;games->launched_minimized=true;
                                menu.refresh_session_entries(true,active->descriptor.kind==openxmb::desktop::SessionKind::application,active->descriptor.title);
                                menu.show_session(active->descriptor.kind==openxmb::desktop::SessionKind::application);opened=true;
                            } else {set_ingame_mode(false);spdlog::warn("Transparent XMB entry failed: {}",prepared.message);}
                        }
                    }
                }
                if(!opened) {restore_shell();queue_menu_reload();}
            } else if(active->capabilities.focus && active->window) {
                games->target=active->window;
                const auto focus=games->target?openxmb::integrations::focus_session_window(*games->target):openxmb::integrations::Result{false,"No unique window is available for this session. Switch to the game manually."};
                if(focus) {SDL_MinimizeWindow(win->win.get());games->shell_is_session_xmb=false;games->launched_minimized=true;}
                else games->pending_notice=focus.message;
            } else games->pending_notice="Return using your system's window switcher. Window control is unavailable for this session.";
        }
        if(games->overlay&&games->overlay->dismissal_finished()) {
            const bool return_focus=games->overlay_return_after_dismiss;
            if(close_overlay()&&return_focus&&games->target) {
                const auto focused=openxmb::integrations::focus_session_window(*games->target);
                if(!focused){restore_shell();games->pending_notice=focused.message;}
            }
        }
        // RPCS3 trophy notices. A visible shell or in-game XMB draws the front
        // notice itself; otherwise a passive host shows it above the game
        // without focus or input. The host exists only while a notice is due,
        // so gameplay renders no OpenXMB frames once the queue drains.
        {
            games->trophy_sound.set_volume(game_state::audio_percent("audio.master"),game_state::audio_percent("audio.effects"));
            auto& notices=games->trophy_notices;
            const auto now=app::session_xmb::seconds();
            static_cast<void>(notices.expire(now));
            const auto* current=notices.current();
            const auto serial=current?current->serial:std::uint64_t{};
            auto& art=games->trophy_art;
            if(art.serial!=serial) {
                if(art.ready.valid())art.ready.wait();
                // Submitted frames may still sample the retiring artwork.
                if(art.image&&art.image->loaded)win->waitIdle();
                art.image.reset();art.ready={};art.failed=false;art.serial=serial;
                if(current&&!current->event.achievement.icon.empty()) {
                    std::error_code error;
                    if(std::filesystem::is_regular_file(current->event.achievement.icon,error)&&!error) {
                        try {art.image=std::make_unique<texture>(device,allocator);art.ready=loader->loadTexture(art.image.get(),current->event.achievement.icon);}
                        catch(const std::exception& failure) {art.image.reset();art.failed=true;spdlog::warn("Trophy artwork unavailable: {}",failure.what());}
                    } else art.failed=true;
                }
            } else if(art.ready.valid()&&art.ready.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
                try {art.ready.get();} catch(const std::exception& failure) {art.failed=true;spdlog::warn("Trophy artwork unavailable: {}",failure.what());}
                if(art.failed||!art.image||!art.image->loaded){art.failed=true;art.image.reset();}
            }
            const bool shell_visible=win->config.headless||(games->overlay&&games->overlay->active())||shell_focused();
            const bool capture_busy=games->screenshot_requested||games->pending_capture.has_value();
            const bool passive_due=current&&!shell_visible&&!games->shell_focus_requested&&!capture_busy&&serial!=games->notice_unavailable;
            if(!passive_due)close_notice();
            if(current&&shell_visible&&overlays.empty()&&!old_overlay&&!games->screenshot_status)notices.start(now);
            if(passive_due) {
                // An unavailable host drains its notice instead of holding it.
                const auto unavailable=[&](std::string_view reason) {
                    spdlog::warn("Passive trophy notice unavailable: {}",reason);
                    close_notice();games->notice_unavailable=serial;notices.start(now);
                };
                if(!games->notice||!games->notice->active()) {
                    std::optional<openxmb::integrations::OverlayBounds> monitor;
                    if(games->target&&openxmb::integrations::validate_session_window(*games->target))monitor=game_monitor(*games->target);
#ifdef _WIN32
                    if(!monitor&&games->shell_handle) {
                        MONITORINFO info{};info.cbSize=sizeof(info);
                        if(GetMonitorInfoW(MonitorFromWindow(reinterpret_cast<HWND>(games->shell_handle),MONITOR_DEFAULTTONEAREST),&info))
                            monitor=openxmb::integrations::OverlayBounds{info.rcMonitor.left,info.rcMonitor.top,
                                static_cast<unsigned>(info.rcMonitor.right-info.rcMonitor.left),static_cast<unsigned>(info.rcMonitor.bottom-info.rcMonitor.top)};
                    }
#else
                    if(SDL_Rect bounds{};!monitor&&SDL_GetDisplayBounds(std::max(0,SDL_GetWindowDisplayIndex(win->win.get())),&bounds)==0)
                        monitor=openxmb::integrations::OverlayBounds{bounds.x,bounds.y,static_cast<unsigned>(bounds.w),static_cast<unsigned>(bounds.h)};
#endif
                    const auto bounds=monitor?openxmb::xmb::trophy_notice_bounds(monitor->x,monitor->y,monitor->width,monitor->height):std::nullopt;
                    if(!bounds)unavailable("no monitor is available for the running session");
                    else {
                        if(!games->notice)games->notice=std::make_unique<app::session_xmb>(win);
                        games->notice_scale=bounds->scale;
                        const auto opened=games->notice->open({bounds->x,bounds->y,bounds->width,bounds->height},{},
                            config::CONFIG.enabled("power.reduced.motion",false),openxmb::integrations::OverlayInteraction::passive);
                        if(!opened)unavailable(opened.message);
                    }
                } else if(const auto error=games->notice->take_error();!error.empty())unavailable(error);
                else if(games->notice->presented())notices.start(now);
            }
            const bool notice_visible=shell_visible?(overlays.empty()&&!old_overlay&&!games->screenshot_status):
                (games->notice&&games->notice->active()&&games->notice->presented());
            if(openxmb::xmb::trophy_notice_sound_ready(notices,games->trophy_origin,notice_visible,capture_busy,games->notice_unavailable))
                static_cast<void>(games->trophy_sound.play_once(serial));
            else if(!current||!games->trophy_origin||serial==games->notice_unavailable)games->trophy_sound.stop();
        }
        if(games->screenshot_requested&&games->overlay&&games->overlay->active()) {
            games->overlay->dismiss(config::CONFIG.enabled("power.reduced.motion",false));
            games->overlay_return_after_dismiss=true;
        } else if(std::exchange(games->screenshot_requested,false)) {
            if(games->capture_worker_busy->load()) {
                games->pending_notice="The previous screenshot is still finishing. Try again shortly.";
            } else if(!active || !active->capabilities.capture || !active->window || win->config.headless) {
                games->pending_notice="Screenshots require a live, identified application window.";
            } else {
                if(!close_overlay())return;
                games->target=active->window;
                const auto focused=openxmb::integrations::focus_session_window(*games->target);
                if(!focused) {restore_shell();games->pending_notice=focused.message;}
                else {
                    SDL_MinimizeWindow(win->win.get());
                    // Let the game's own event/render loop resume after focus.
                    // The XMB event loop stays responsive while the desktop settles.
                    games->pending_capture=game_state::PendingCapture{active->id,*games->target,
                        std::chrono::steady_clock::now()+std::chrono::milliseconds(100),
                        std::chrono::steady_clock::now()+std::chrono::milliseconds(2500)};
                }
            }
        }
        if(games->pending_capture && std::chrono::steady_clock::now()>=games->pending_capture->due) {
            if(games->pending_capture->job&&!games->pending_capture->job->ready()) {
                if(std::chrono::steady_clock::now()<games->pending_capture->deadline+std::chrono::milliseconds(2500))return;
                games->pending_capture->job->cancel();games->pending_capture.reset();
                restore_shell();games->pending_notice="The screenshot could not be captured in time. Try again shortly.";
                return;
            }
            const auto request=std::exchange(games->pending_capture,{});
            const auto current=games->sessions.active_session();
            if(!current || current->id!=request->session) {
                restore_shell();games->pending_notice="The session changed before the screenshot could be saved.";
            } else {
                openxmb::integrations::SessionCaptureResult captured;
                if(request->job) {
                    try {captured=request->job->get();}
                    catch(const std::exception& error){captured={openxmb::integrations::CaptureState::unavailable,error.what()};}
                } else {
                auto title=current->descriptor.title;
                for(auto& c:title)if(static_cast<unsigned char>(c)<32||std::string_view{"<>:\"/\\|?*"}.find(c)!=std::string_view::npos)c='_';
                if(title.size()>80){std::size_t end=80;while(end>0&&(static_cast<unsigned char>(title[end])&0xc0)==0x80)--end;title.resize(end);}
                while(!title.empty()&&(title.back()=='.'||title.back()==' '))title.pop_back();
                const auto folder=games->photo_root/"Screenshots"/std::filesystem::u8path((current->descriptor.kind==openxmb::desktop::SessionKind::application?"App - ":"Game - ")+title);
                static std::uint64_t serial{};
                const auto stamp=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const auto destination=folder/(std::to_string(stamp)+"-"+std::to_string(++serial)+".png");
                std::error_code folder_error;std::filesystem::create_directories(folder,folder_error);
                if(folder_error)captured={openxmb::integrations::CaptureState::unavailable,"Cannot create the screenshot folder: "+folder_error.message()};
                else {
                    auto pending=*request;
                    pending.job=std::make_shared<openxmb::desktop::AsyncJob<openxmb::integrations::SessionCaptureResult>>();
                    const auto busy=games->capture_worker_busy;busy->store(true);
                    try {
                        pending.job->start([target=request->target,destination,shell_handle=games->shell_handle,busy](std::stop_token cancellation) {
                            struct Completion {std::shared_ptr<std::atomic_bool> busy;~Completion(){busy->store(false);}} completion{busy};
                            return openxmb::integrations::capture_session_window(target,destination,shell_handle,cancellation);
                        });
                        games->pending_capture=std::move(pending);return;
                    } catch(...) {busy->store(false);captured={openxmb::integrations::CaptureState::unavailable,"The screenshot worker could not start. Try again shortly."};}
                }
                }
                if(captured) {
                    games->screenshot_status=true;games->screenshot_status_started.reset();
                    games->shell_is_session_xmb=false;games->launched_minimized=true;
                } else if(captured.state==openxmb::integrations::CaptureState::pending && std::chrono::steady_clock::now()<request->deadline) {
                    games->pending_capture=*request;
                    games->pending_capture->job.reset();
                    games->pending_capture->due=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
                } else {
                    spdlog::warn("Session screenshot unavailable: {}",captured.message);
                    restore_shell();games->pending_notice=captured.message;
                }
            }
        }
    }

    void shell::tick_games() {
        // Discovery/import and full-menu texture rebuilds must not interrupt
        // an in-game frame or consume its entrance animation on the UI thread.
        if(games->sessions.active_session())return;
        if(games->artwork.ready()) {
            try {
                const auto result=games->artwork.get();
                openxmb::desktop::Library current;
                if(current.root()==result.root && current.active_profile().id==result.profile) {
                    auto entries=current.games();
                    const auto found=std::ranges::find(entries,result.game_id,&openxmb::desktop::Game::id);
                    bool changed=false;
                    if(found!=entries.end()) {
                        const auto fill=[&](std::filesystem::path openxmb::desktop::Game::* field,const char* role,const auto& path) {
                            auto& value=*found;
                            if(!(value.*field).empty() || path.empty())return;
                            value.*field=path;value.automatic_artwork[role]=path;changed=true;
                        };
                        fill(&openxmb::desktop::Game::icon,"icon",result.icon);
                        fill(&openxmb::desktop::Game::cover,"cover",result.cover);
                        fill(&openxmb::desktop::Game::background,"background",result.background);
                        if(changed) {current.save_game(std::move(*found));queue_menu_reload();}
                    }
                }
            } catch(const std::exception& error) {spdlog::warn("Automatic artwork could not be applied: {}",error.what());}
        }
        if(games->highlighted && !games->artwork.running() && overlays.empty() && !old_overlay && !games->sessions.active_session() &&
           config::CONFIG.enabled("artwork.steamgriddb.automatic",true) && std::chrono::steady_clock::now()-games->highlighted_at>=std::chrono::milliseconds(350) &&
           (games->highlighted->icon.empty() || games->highlighted->background.empty())) {
            const auto variable=config::CONFIG.preference("artwork.steamgriddb.key_env","STEAMGRIDDB_API_KEY");
            const bool valid=!variable.empty() && variable.size()<=128 && std::ranges::all_of(variable,[](unsigned char c) {return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';});
            const auto* key=valid?std::getenv(variable.c_str()):nullptr;
            if(key && *key) {
                openxmb::desktop::Library library;
                const auto profile=library.active_profile().id;
                const auto root_bytes=library.root().u8string();
                const auto attempt=std::string(root_bytes.begin(),root_bytes.end())+"\n"+profile+"\n"+games->highlighted->id;
                if(games->artwork_attempted.size()<1024 && games->artwork_attempted.insert(attempt).second) {
                    const auto game=*games->highlighted;const auto root=library.root();const std::string secret(key);
                    games->artwork.start([game,root,profile,secret](std::stop_token stop) {
                        using namespace openxmb::desktop;
                        game_state::ArtworkResult output;output.root=root;output.profile=profile;output.game_id=game.id;
                        HttpTransport transport=[stop](HttpRequest request) {request.progress=[stop](std::uint64_t,std::uint64_t) {return !stop.stop_requested();};return http_get(request);};
                        SteamGridDB provider(root/"cache/artwork"/profile,transport);
                        const auto matching=provider.search(game.title,secret);
                        if(!matching || stop.stop_requested())return output;
                        std::uint64_t id{};unsigned exact{};
                        for(const auto& candidate:matching.games)if(candidate.exact_match) {id=candidate.id;++exact;}
                        if(exact!=1)return output;
                        const auto download=[&](SteamGridDBRole role) {
                            if(stop.stop_requested())return std::filesystem::path{};
                            const auto assets=provider.assets(id,role,secret);
                            if(!assets || assets.assets.empty() || stop.stop_requested())return std::filesystem::path{};
                            const auto selected=provider.select(id,assets.assets.front().id,role,{},SteamGridDBOverride::preserve_native,secret);
                            return selected?selected.path:std::filesystem::path{};
                        };
                        if(game.icon.empty()) {
                            output.cover=download(SteamGridDBRole::horizontal_grid);output.icon=output.cover;
                            if(output.icon.empty())output.icon=download(SteamGridDBRole::icon);
                        }
                        if(game.background.empty())output.background=download(SteamGridDBRole::hero);
                        if(stop.stop_requested()) {output.icon.clear();output.cover.clear();output.background.clear();}
                        return output;
                    });
                }
            }
        }
        if(games->presentation) {
            games->presentation->choose(overlays.empty() && !old_overlay && !ingame_mode && !games->sessions.active_session() ? games->highlighted : nullptr);
            games->presentation->tick(win);
        }
        if(!fixed_components_loaded || setup_requested || config::CONFIG.initialSetupRequired) return;
        const auto configured_runner=config::CONFIG.preference("runner.rpcs3.path");
        if(!games->discovery.valid() && configured_runner!=games->discovery_runner_path)games->discovery_started=false;
        if(games->discovery.valid() && games->discovery.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try {
                auto found=games->discovery.get();
                openxmb::desktop::Library current;
                if(current.root()==games->discovery_root && current.active_profile().id==games->discovery_profile &&
                   configured_runner==games->discovery_runner_path) {
                    if(!found.empty()) {static_cast<void>(current.import_games(found));queue_menu_reload();}
                } else games->discovery_started=false;
            } catch(const std::exception& error) { spdlog::warn("Automatic game discovery: {}",error.what()); }
        }
        if(!games->discovery_started && !win->config.headless) {
            try {
                openxmb::desktop::Library current;
                games->discovery_root=current.root(); games->discovery_profile=current.active_profile().id;
                const auto roots=current.game_roots();
                openxmb::desktop::GameDiscoveryOptions options;
                options.rpcs3_executable=std::filesystem::u8path(configured_runner);
                games->discovery_runner_path=configured_runner;
                games->discovery=std::async(std::launch::async,[roots,options] {return openxmb::desktop::discover_games(roots,options);});
                games->discovery_started=true;
            } catch(const std::exception& error) { spdlog::warn("Cannot start game discovery: {}",error.what()); }
        }
    }

    void shell::reload_theme_sounds() {
        const auto load=[&](sdl::mix::unique_chunk& chunk,std::initializer_list<const char*> names){for(const auto* name:names){const auto file=config::CONFIG.resolve_sound(config::CONFIG.asset_directory/"sounds"/name);if(auto* replacement=sdl::mix::LoadWAV(file.string().c_str())){chunk=sdl::mix::unique_chunk{replacement};return;}}};
        load(ok_sound,{"ok.wav"});
        load(confirm_sound,{"NSE.ui.Confirm.wav","NSE.ui.Confirm.ogg","confirm.wav"});
        load(cancel_sound,{"NSE.ui.Cancel.wav","NSE.ui.Cancel.ogg","cancel.wav"});
        load(back_sound,{"NSE.clicker.Cancel.wav","NSE.clicker.Cancel.ogg","back.wav"});
    }
    void shell::reload_trophy_sound() {
        const auto result=games->trophy_sound.configure({config::CONFIG.preference("audio.trophy.mode","Off")=="User-supplied",
            config::CONFIG.preference("audio.trophy.file"),game_state::audio_percent("audio.master"),game_state::audio_percent("audio.effects")});
        if(result.status!=openxmb::desktop::TrophySound::CueStatus::off&&!result.ok())
            spdlog::warn("Trophy sound unavailable: {}",openxmb::desktop::TrophySound::describe(result.status));
    }
    bool shell::set_audio_device(const std::string& name) {
        // Settings is reached after closing foreground playback; retire the
        // background decoder before resetting the shared SDL mixer device.
        win->waitIdle();
        #if OPENXMB_ENABLE_VIDEO_PLAYER
        video_background_render.reset();
        #endif
        games->trophy_sound.device_will_close();
        const bool changed=openxmb::music::service().set_output_device(name);
        static_cast<void>(games->trophy_sound.prepare());
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
        } else if(name == "trophies") {
            emplace_overlay<app::trophy_collection>(*loader);
        } else if(name == "youtube") {
            open_youtube();
        } else if(name == "users" || name == "games" || name == "runners" || name == "custom" || name == "friends" || name == "account") {
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
        reload_trophy_sound();
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
        games->trophy_sound.stop();
        win->waitIdle();
        static_cast<void>(theme_library()->revert());
        theme->active.reset();theme->frame={};theme->textures.clear();theme->library.reset();
        config::CONFIG.reset_to_defaults();
        refresh_desktop_preferences();reload_background();reload_fonts();reload_menu();reload_button_icons();reload_theme_sounds();
        setup_requested = true;
    }

    bool shell::render_status_clock(gui_renderer& renderer) {
        const auto local=shell_civil_time();
        if(!local)return false;
        status_bar_render.render(renderer,{local->day,local->month,local->hour,local->minute,local->second},1.0F,{},ingame_mode);
        return true;
    }

    void shell::begin_initial_setup() {
        if(!initial_setup_active) {initial_setup_active=true;reload_fonts();}
        auto* setup=emplace_overlay<app::initial_setup_overlay>([this](const openxmb::setup::Draft& draft,const openxmb::setup::Zone& zone) {
            const auto previous_preferences=config::CONFIG.preferences;
            const auto previous_language=config::CONFIG.language;
            const auto previous_required=config::CONFIG.initialSetupRequired;
            auto& preferences=config::CONFIG.preferences;
            preferences["user.name"]=draft.user_name;
            preferences["system.region"]=draft.region;
            preferences["system.timezone"]=zone.id;
            preferences["system.timezone.cities"]=zone.cities;
            preferences["system.timezone.standard-minutes"]=std::to_string(zone.standard_minutes);
            preferences["system.timezone.daylight"]=draft.automatic_daylight?"On":"Off";
            preferences["system.clock.automatic"]=draft.automatic_time?"On":"Off";
            preferences["system.clock.offset-seconds"]=std::to_string(draft.clock_offset.count());
            config::CONFIG.setLanguage(draft.language);
            config::CONFIG.initialSetupRequired=false;
            if(!config::CONFIG.save_config_checked()) {
                config::CONFIG.preferences=previous_preferences;
                config::CONFIG.setLanguage(previous_language);
                config::CONFIG.initialSetupRequired=previous_required;
                throw std::runtime_error("Could not save OpenXMB settings. Check that the profile folder is writable, then try again.");
            }
            setup_handoff_pending=true;
        });
        setup->preload(*loader,this);
    }

    void shell::switch_profile(const std::filesystem::path& path) {
        games->trophy_sound.stop();
        win->waitIdle();
        config::CONFIG.switch_profile(path);
        games->trophy_watch.set_session({});games->trophy_origin.reset();
        games->trophy_sound.stop();games->trophy_notices.clear();
        games->highlighted.reset();
        games->artwork.cancel();games->artwork_attempted.clear();
        if(games->presentation) games->presentation->stop();
        // Consume a pending result against its original profile before starting
        // another scan; no worker may write to the newly selected profile.
        if(!games->discovery.valid()) games->discovery_started=false;
        refresh_desktop_preferences();
        theme->library.reset(); theme->active.reset(); theme->textures.clear();
        config::CONFIG.themeIcons.clear(); config::CONFIG.themeSounds.clear();
        if(auto saved = theme_library()->active()) apply_theme(*saved);
        else { reload_background(); reload_fonts(); reload_menu(); }
        reload_button_icons();reload_theme_sounds();
        setup_requested=config::CONFIG.initialSetupRequired;
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
                validated_font=std::make_unique<font_renderer>(path.string(),32,device,allocator,win->swapchainExtent,win->gpuFeatures);
                configure_shell_font(*validated_font,initial_setup_active);
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
        auto replacement=std::make_unique<font_renderer>(path.string(),32,device,allocator,win->swapchainExtent,win->gpuFeatures);
        configure_shell_font(*replacement,initial_setup_active);
        replacement->preload(loader,{shellRenderPass.get()},win->config.sampleCount,win->pipelineCache.get(),nullptr,0x20,0x1ff).get();
        replacement->prepare(win->swapchainImageCount);
        config::CONFIG.setFontPath(path.string());font_render=std::move(replacement);
    }
    void shell::reload_fonts() {
        win->waitIdle();
        auto replacement = std::make_unique<font_renderer>(config::CONFIG.fontPath.string(), 32, device, allocator, win->swapchainExtent, win->gpuFeatures);
        configure_shell_font(*replacement,initial_setup_active);
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
    void shell::render_keycap_fallback(gui_renderer& renderer,float left,float centre,float width,float height,std::string_view label,float font,float alpha) {
        const float fw=static_cast<float>(renderer.frame_size.width),fh=static_cast<float>(renderer.frame_size.height);
        const float scale=std::min(fw/1920.0F,fh/1080.0F);
        auroreengine::simple_params shape{};shape.radius_px=3*scale;shape.edge_falloff_px=0.8F*scale;
        shape.edge_fill=0;shape.size_px={width,height};
        renderer.draw_rect({left/fw,(centre-height*0.5F)/fh},{width/fw,height/fh},{1,1,1,0.85F*alpha},shape);
        const auto metrics=renderer.text_line_metrics_pixels(font);
        renderer.draw_text_pixels(label,left+width*0.5F,centre+(metrics.x-metrics.y)*0.5F,font,{1,1,1,alpha},0.7F,0,true);
    }

    void shell::render_input_instruction(gui_renderer& renderer,std::string_view pattern,float x,float baseline,float size,float alpha) {
        if(get_controller_type()=="none")return;
        const auto translated=openxmb::localization::shell_locale::translate(pattern);
        const float fw=static_cast<float>(renderer.frame_size.width),fh=static_cast<float>(renderer.frame_size.height);
        const float scale=std::min(fw/1920.0F,fh/1080.0F),ox=(fw-1920*scale)*0.5F,oy=(fh-1080*scale)*0.5F;
        const float font=size*scale*static_cast<float>(config::CONFIG.textScale),glyph=font,key_font=font*0.68F;
        const auto metrics=renderer.text_line_metrics_pixels(font);
        const auto control_action=[](openxmb::input::PromptControl control) {
            using enum openxmb::input::PromptControl;
            switch(control){case up:return action::up;case down:return action::down;case left:return action::left;
                case right:return action::right;case ok:return action::ok;case cancel:return action::cancel;
                case extra:return action::extra;case options:return action::options;}
            return action::none;
        };
        struct Run {std::string_view text;action control;float width;};
        std::vector<std::vector<Run>> lines(1);std::vector<float> widths(1);
        const auto append=[&](Run run) {
            if(widths.back()+run.width>1760*scale&&!lines.back().empty()){lines.emplace_back();widths.push_back(0);}
            if(run.control==action::none&&run.text.find_first_not_of(' ')==run.text.npos&&lines.back().empty())return;
            widths.back()+=run.width;lines.back().push_back(run);
        };
        for(const auto& part:openxmb::input::prompt_parts(translated)) {
            if(part.control) {
                const auto control=control_action(*part.control);const auto& label=button_labels[std::to_underlying(control)];
                append({{},control,label.empty()?glyph*get_button_aspect(control):std::max(glyph,renderer.text_width_pixels(label,key_font)+10*scale)});
            } else {
                for(std::size_t start=0;start<part.text.size();) {
                    auto end=part.text.find(' ',start);if(end==part.text.npos)end=part.text.size();else ++end;
                    const auto word=part.text.substr(start,end-start);
                    append({word,action::none,renderer.text_width_pixels(word,font)});start=end;
                }
            }
        }
        for(std::size_t line=0;line<lines.size();++line) {
            float current=ox+x*scale-widths[line]*0.5F;
            const float y=oy+baseline*scale-static_cast<float>(lines.size()-line-1)*font*1.3F;
            for(const auto& run:lines[line]) {
                if(run.control==action::none)renderer.draw_text_pixels(run.text,current,y,font,{1,1,1,alpha},0.7F);
                else {
                    const auto index=std::to_underlying(run.control);const float centre=y-(metrics.x-metrics.y)*0.5F;
                    if(!button_labels[index].empty()) {
                        render_keycap_fallback(renderer,current,centre,run.width,glyph,button_labels[index],key_font,alpha);
                    } else if(const auto* icon=buttonTextures[index].get())
                        renderer.draw_image(*icon,current/fw,(centre-glyph*0.5F)/fh,run.width/fh,glyph/fh,{1,1,1,alpha});
                }
                current+=run.width;
            }
        }
    }

    void shell::reload_button_icons() {
        if(std::ranges::any_of(buttonTextures,[](const auto& icon){return bool(icon);}))win->waitIdle();
        const auto family=get_controller_type();
        for(std::size_t i=1;i<std::to_underlying(action::_length);++i) {
            button_labels[i].clear();buttonTextures[i].reset();if(family=="none")continue;
            const bool keyboard=family=="keyboard";const auto index=input_bindings::glyph_index(config::CONFIG.preferences,!keyboard,i-1);
            const auto code=input_bindings::read(config::CONFIG.preferences,!keyboard,i-1);
            if(keyboard) {
#ifdef __APPLE__
                constexpr bool mac=true;
#else
                constexpr bool mac=false;
#endif
                const auto stem=keyboard_glyphs::stem(code,mac);
                const auto asset=config::CONFIG.asset_directory/"icons/keyboard"/("icon-"+std::string(stem)+".svg");
                if(!stem.empty()&&std::filesystem::is_regular_file(asset)) {
                    buttonTextures[i]=std::make_unique<texture>(device,allocator);loader->loadTexture(buttonTextures[i].get(),asset);continue;
                }
            }
            if(family=="xbox") {
                const auto artwork=input_bindings::xbox_artwork(code);
                if(!artwork.empty()) {buttonTextures[i]=std::make_unique<texture>(device,allocator);loader->loadTexture(buttonTextures[i].get(),config::CONFIG.asset_directory/"icons/controller"/(std::string(artwork)+".png"));continue;}
            }
            if(index<0||family=="default"||keyboard) {
                if(keyboard)button_labels[i]=SDL_GetScancodeName(static_cast<SDL_Scancode>(code));
                else {switch(code){case 0:button_labels[i]="South";break;case 1:button_labels[i]="East";break;case 2:button_labels[i]="West";break;case 3:button_labels[i]="North";break;case 4:button_labels[i]=family=="playstation"?"Select":"View";break;case 6:button_labels[i]=family=="playstation"?"Start":"Menu";break;case 9:button_labels[i]=family=="playstation"?"L1":"LB";break;case 10:button_labels[i]=family=="playstation"?"R1":"RB";break;case 21:button_labels[i]=family=="playstation"?"L2":"LT";break;case 22:button_labels[i]=family=="playstation"?"R2":"RT";break;default:button_labels[i]="Button "+std::to_string(code);}}
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
            if(!input_prompt_family.empty())return input_prompt_family;
            if(win->controllers.empty()) {
                return "keyboard";
            }
            for(const auto& [id, controller] : win->controllers) {
                auto family=controller_prompt_family(controller.get());
                if(family!="default")return family;
            }
            return "default";
        }
        return type;
    }

    void shell::note_input_device(sdl::GameController* controller) {
        if(config::CONFIG.controllerType!="auto")return;
        auto family=controller_prompt_family(controller);
        if(family!=input_prompt_family) {input_prompt_family=std::move(family);input_prompt_refresh=true;}
    }

    void shell::tick() {
        if(std::exchange(games->close_library_pending,false)) {
            win->waitIdle();
            std::erase_if(overlays,[](const auto& value) {return dynamic_cast<programs::library_manager*>(value.get())!=nullptr;});
        }
        if(!games->pending_notice.empty() && input_focused()) {
            spdlog::debug("Session notice: {}",games->pending_notice);
            emplace_overlay<app::message_overlay>("Game Session",std::exchange(games->pending_notice,{}),std::vector<std::string>{"OK"});
        }
        tick_games();
        if(menu_reload_pending && overlays.empty() && !old_overlay && !games->sessions.active_session() &&
           !(games->overlay&&games->overlay->active())) {
            menu_reload_pending=false;
            reload_menu();
        }
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
        const bool focused=input_focused();
        if(!focused) {last_controller_axis_input={};controller_axis_position={};cursor_joystick_delta={};button_timing.clear();}
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

        {
            static const auto harness_steps = harness_steps_from_environment();
            static std::size_t harness_next = 0;
            static const auto harness_started = std::chrono::steady_clock::now();
            const double harness_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - harness_started).count();
            while(harness_next < harness_steps.size() && harness_steps[harness_next].seconds <= harness_seconds) {
                const std::string_view step = harness_steps[harness_next++].step;
                constexpr std::array<std::pair<std::string_view, action>, 8> actions{{
                    {"left", action::left}, {"right", action::right}, {"up", action::up}, {"down", action::down},
                    {"ok", action::ok}, {"cancel", action::cancel}, {"options", action::options}, {"extra", action::extra}}};
                constexpr std::array<std::pair<std::string_view, events::logical_controller_button>, 4> buttons{{
                    {"start", events::logical_controller_button::start}, {"select", events::logical_controller_button::back},
                    {"l1", events::logical_controller_button::leftshoulder}, {"r1", events::logical_controller_button::rightshoulder}}};
                if(step=="quit") {
                    SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);
                } else if(step.starts_with("key-down=")||step.starts_with("key-up=")) {
                    const bool pressed=step.starts_with("key-down=");
                    const auto number=step.substr(pressed?9:7);unsigned scan{};
                    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),scan);
                    if(parsed.ec==std::errc{}&&parsed.ptr==number.data()+number.size()&&scan<SDL_NUM_SCANCODES) {
                        SDL_Keysym key{};key.scancode=static_cast<SDL_Scancode>(scan);key.sym=SDL_GetKeyFromScancode(key.scancode);
                        if(pressed)key_down(key);else key_up(key);
                    }
                } else if(step.starts_with("axis=")) {
                    const auto comma=step.find(',');unsigned axis{};int value{};
                    if(comma!=std::string_view::npos) {
                        const auto a=std::from_chars(step.data()+5,step.data()+comma,axis);
                        const auto v=std::from_chars(step.data()+comma+1,step.data()+step.size(),value);
                        if(a.ec==std::errc{}&&a.ptr==step.data()+comma&&v.ec==std::errc{}&&v.ptr==step.data()+step.size()&&
                           axis<SDL_CONTROLLER_AXIS_MAX&&value>=-32768&&value<=32767)
                            axis_motion(nullptr,static_cast<sdl::GameControllerAxis>(axis),static_cast<int16_t>(value));
                    }
                } else if(step.starts_with("profile=")) {
                    switch_profile(std::filesystem::u8path(step.substr(8)));
                    break;
                } else if(step=="in-game") {
                    set_ingame_mode(true);
                } else if(step=="reset") {
                    reset_settings();
                } else if(step.starts_with("rs=")) {
                    const auto comma = step.find(',');
                    float x{}, y{};
                    if(comma != std::string_view::npos) {
                        fast_float::from_chars(step.data() + 3, step.data() + comma, x);
                        fast_float::from_chars(step.data() + comma + 1, step.data() + step.size(), y);
                    }
                    dispatch<events::joystick_axis>(action::none, events::logical_joystick_index::right, x, y);
                } else if(const auto it = std::ranges::find(actions, step, &std::pair<std::string_view, action>::first); it != actions.end()) {
                    dispatch<events::controller_button_down>(it->second, events::logical_controller_button::invalid);
                } else if(const auto button = std::ranges::find(buttons, step, &std::pair<std::string_view, events::logical_controller_button>::first); button != buttons.end()) {
                    dispatch<events::controller_button_down>(action::none, button->second);
                } else {
                    spdlog::warn("Ignoring unknown harness step '{}'", step);
                }
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
        if(std::exchange(setup_requested,false)) begin_initial_setup();
        if(std::exchange(setup_handoff_pending,false)) {
            initial_setup_active=false;
            reload_language();
            shader_time_zero=std::chrono::steady_clock::now();
            startup_timeline.reset();
            emplace_overlay<app::startup_overlay>();
        }
        if(!(games->overlay && games->overlay->active()))poll_mouse();
        tick_cursor();
    }

    double shell::startup_elapsed_seconds() const {
        if(win->config.headless && force_headless_startup_overlay()) {
            if(const auto fixed = fixed_boot_seconds_from_environment()) return *fixed;
        }
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - shader_time_zero).count();
        if(!config::CONFIG.enabled("startup.intro")) return elapsed+(config::CONFIG.enabled("startup.warning")?openxmb::xmb::BootMilestones::warning_in_seconds:openxmb::xmb::BootMilestones::customised_seconds);
        return elapsed + (win->config.headless && !force_headless_startup_overlay()
            ? openxmb::xmb::BootMilestones::customised_seconds : 0.0);
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
        if(event.action!=action::none)spdlog::trace("Dispatch action={}, dialogs={}, closing={}, background={}",static_cast<unsigned>(event.action),overlays.size(),games->overlay&&games->overlay->closing(),background_only);
        if(games->overlay&&games->overlay->closing())return;
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

        const auto menu_result=menu.on_action(event.action);
        if((menu_result & result::unsupported) && event.action==action::cancel && games->overlay && games->overlay->active()) {
            games->overlay_dismiss=true;games->overlay_return=true;return;
        }
        handle(menu_result);
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
        note_input_device(nullptr);
        spdlog::trace("Key down: symbol={} scancode={}",key.sym,std::to_underlying(key.scancode));
        const auto scan=static_cast<unsigned>(std::to_underlying(key.scancode));
        const bool held=scan<held_scancodes.size()&&held_scancodes[scan];
        if(scan<held_scancodes.size())held_scancodes[scan]=true;
        const auto chord=config::CONFIG.preference("session.hotkey","Ctrl+Shift+F1");
        unsigned shortcut=SDL_SCANCODE_F1;
        if(chord=="Ctrl+Shift+F2")shortcut=SDL_SCANCODE_F2;else if(chord=="Ctrl+Shift+F3")shortcut=SDL_SCANCODE_F3;else if(chord=="Ctrl+Shift+F4")shortcut=SDL_SCANCODE_F4;
        if(!held && has_running_session() && scan==shortcut && (key.mod&KMOD_CTRL) && (key.mod&KMOD_SHIFT)) {toggle_session_xmb();return;}
        for(std::size_t i=0;i<input_bindings::actions.size();++i) if(scan==static_cast<unsigned>(input_bindings::read(config::CONFIG.preferences,false,i))) {
            if(held&&i>=4)return;
            dispatch(event{static_cast<action>(i+1),events::key_down{scan}});return;
        }
        dispatch(event{action::none,events::key_down{scan}});
    }

    void shell::add_controller(sdl::GameController* controller)
    {
        if(config::CONFIG.controllerType == "auto") {
            input_prompt_family.clear();
            reload_button_icons();
        }
    }
    void shell::remove_controller(sdl::GameController* controller)
    {
        static_cast<void>(games->guide_edges.edge(GuideTiming::Source::sdl,false,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())));
        button_timing.clear();
        for(int code=0;code<SDL_CONTROLLER_BUTTON_MAX;++code)
            dispatch(event{action::none,events::controller_button_up{to_logical_button(static_cast<sdl::GameControllerButton>(code))}});
        for(const auto trigger:{events::logical_controller_button::lefttrigger,events::logical_controller_button::righttrigger})
            dispatch(event{action::none,events::controller_button_up{trigger}});
        if(config::CONFIG.controllerType == "auto") {
            input_prompt_family.clear();
            reload_button_icons();
        }
    }
    void shell::button_down(sdl::GameController* controller,sdl::GameControllerButton button) {
        // SDL can still deliver queued device events to an offscreen window.
        // Only explicit harness input (no device handle) belongs in captures.
        if(win->config.headless&&controller)return;
        if(static_cast<int>(button)==SDL_CONTROLLER_BUTTON_GUIDE && has_running_session()) {
            note_input_device(controller);
            if(games->guide_edges.edge(GuideTiming::Source::sdl,true,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())))toggle_session_xmb();
            return;
        }
        if(!input_focused())return;
        note_input_device(controller);
        const auto code=static_cast<unsigned>(button);
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for(std::size_t i=0;i<input_bindings::actions.size();++i)if(static_cast<int>(button)==input_bindings::read(config::CONFIG.preferences,true,i)) {
            if(button_timing.press(code,i<4,seconds))dispatch(event{static_cast<action>(i+1),events::controller_button_down{to_logical_button(button)}});return;
        }
        if(button_timing.press(code,false,seconds))dispatch(event{action::none,events::controller_button_down{to_logical_button(button)}});
    }

    void shell::button_up(sdl::GameController* controller, sdl::GameControllerButton button)
    {
        if(win->config.headless&&controller)return;
        spdlog::trace("Button up: {}", fmt::underlying(button));
        if(static_cast<int>(button)==SDL_CONTROLLER_BUTTON_GUIDE) {
            static_cast<void>(games->guide_edges.edge(GuideTiming::Source::sdl,false,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())));return;
        }
        button_timing.release(static_cast<unsigned>(button));
        if(!input_focused())return;
        dispatch(event{action::none, events::controller_button_up{to_logical_button(button)}});
    }
    void shell::axis_motion(sdl::GameController* controller, sdl::GameControllerAxis axis, int16_t value)
    {
        if(win->config.headless&&controller)return;
        if(!input_focused()) {
            last_controller_axis_input={};controller_axis_position={};cursor_joystick_delta={};button_timing.clear();return;
        }
        spdlog::trace("Axis motion: {} {}", fmt::underlying(axis), value);
        if(std::abs(value)>=controller_axis_input_threshold)note_input_device(controller);

        if(axis==sdl::GameControllerAxisValues::TRIGGERLEFT||axis==sdl::GameControllerAxisValues::TRIGGERRIGHT) {
            const unsigned index=axis==sdl::GameControllerAxisValues::TRIGGERLEFT?0:1;
            if(const auto edge=button_timing.trigger(index,value)) {
                const auto button=static_cast<sdl::GameControllerButton>(21+index);
                if(*edge)button_down(controller,button);else button_up(controller,button);
            }
            return;
        }

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
