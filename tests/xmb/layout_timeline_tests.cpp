#include "openxmb/xmb/boot_timeline.hpp"
#include "openxmb/xmb/identity.hpp"
#include "openxmb/xmb/layout.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

import openxmb.xmb.root_scene;

namespace {

using namespace openxmb::xmb;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void require_near(double actual, double expected, double tolerance,
                  std::string_view message) {
  if (std::abs(actual - expected) > tolerance) {
    throw std::runtime_error(std::string(message) + ": expected " +
                             std::to_string(expected) + ", got " +
                             std::to_string(actual));
  }
}

void test_layout_transforms() {
  const auto identity = make_layout_transform({1920, 1080});
  require_near(identity.scale_x, 1.0, 1e-12, "identity scale x");
  require_near(identity.scale_y, 1.0, 1e-12, "identity scale y");
  require(identity.point_to_framebuffer({566.0, 289.0}) == Point{566.0, 289.0},
          "identity anchor transform");

  const auto four_three = make_layout_transform({1024, 768});
  require_near(four_three.scale_x, 1024.0 / 1920.0, 1e-12, "4:3 scale");
  require_near(four_three.offset_x, 0.0, 1e-12, "4:3 x offset");
  require_near(four_three.offset_y, 96.0, 1e-12, "4:3 letterbox offset");
  const auto four_three_center =
      four_three.point_to_framebuffer({960.0, 540.0});
  require_near(four_three_center.x, 512.0, 1e-9, "4:3 center x");
  require_near(four_three_center.y, 384.0, 1e-9, "4:3 center y");

  const auto ultrawide = make_layout_transform({2560, 1080});
  require_near(ultrawide.scale_x, 1.0, 1e-12, "21:9 scale");
  require_near(ultrawide.offset_x, 320.0, 1e-12, "21:9 pillarbox offset");
  const auto ultrawide_anchor = ultrawide.point_to_framebuffer({566.0, 289.0});
  require_near(ultrawide_anchor.x, 886.0, 1e-9, "21:9 active category x");
  require_near(ultrawide_anchor.y, 289.0, 1e-9, "21:9 category y");

  const auto portrait = make_layout_transform({1080, 1920});
  require_near(portrait.scale_x, 0.5625, 1e-12, "portrait scale");
  require_near(portrait.offset_x, 0.0, 1e-12, "portrait x offset");
  require_near(portrait.offset_y, 656.25, 1e-12, "portrait letterbox offset");
  const auto portrait_center = portrait.point_to_framebuffer({960.0, 540.0});
  require_near(portrait_center.x, 540.0, 1e-9, "portrait center x");
  require_near(portrait_center.y, 960.0, 1e-9, "portrait center y");
  const auto round_trip =
      portrait.point_to_logical(portrait.point_to_framebuffer({683.0, 515.0}));
  require_near(round_trip.x, 683.0, 1e-9, "layout round trip x");
  require_near(round_trip.y, 515.0, 1e-9, "layout round trip y");
}

void test_boot_timeline() {
  static_assert(kStartupIdentity == "Syndromatic Limited Bharat Britannia");
  static_assert(kStartupIdentity.find("PS3") == std::string_view::npos);

  const auto wave_start = sample_boot_timeline(0.8);
  require_near(wave_start.wave_boot_time_seconds, 0.0, 1e-12,
               "wave start time");
  require_near(wave_start.wave_gain, 0.0, 1e-12, "wave start gain");

  require_near(sample_boot_timeline(2.2).identity_opacity, 0.0, 1e-12,
               "identity starts at 2.2s");
  const auto identity_full = sample_boot_timeline(4.0);
  require_near(identity_full.identity_opacity, 1.0, 1e-12,
               "identity full at 4.0s");
  require_near(identity_full.identity_blur_px, 0.0, 1e-12,
               "identity sharp at 4.0s");
  require_near(sample_boot_timeline(5.8).identity_opacity, 1.0, 1e-12,
               "identity fade starts at 5.8s");
  require_near(sample_boot_timeline(6.8).identity_opacity, 0.0, 1e-12,
               "identity gone at 6.8s");

  require_near(sample_boot_timeline(7.665).warning_opacity, 1.0, 1e-12,
               "warning hard cut in");
  require_near(sample_boot_timeline(12.53).warning_opacity, 0.0, 1e-12,
               "warning hard cut out");
  require(sample_boot_timeline(8.0).warning_backdrop_blur_px > 6.49,
          "warning backdrop reaches 6.5px blur");

  require_near(sample_boot_timeline(15.9).ui_reveal, 0.0, 1e-12,
               "UI reveal starts at 15.9s");
  const auto complete = sample_boot_timeline(16.9);
  require_near(complete.ui_reveal, 1.0, 1e-12, "UI reveal completes at 16.9s");
  require(complete.complete, "boot completes at 16.9s");

  double previous_wave = 0.0;
  double previous_ui = 0.0;
  for (int tick = 0; tick <= 1690; ++tick) {
    const auto sample = sample_boot_timeline(static_cast<double>(tick) / 100.0);
    require(sample.wave_gain + 1e-12 >= previous_wave,
            "wave gain is monotonic");
    require(sample.ui_reveal + 1e-12 >= previous_ui, "UI reveal is monotonic");
    previous_wave = sample.wave_gain;
    previous_ui = sample.ui_reveal;
  }

  BootTimeline timeline;
  require(timeline.sample(0.0).effect == BootEffect::play_startup_cue,
          "first tick requests startup cue");
  require(timeline.sample(0.1).effect == BootEffect::none,
          "startup cue is once-only");
  const auto skipped = timeline.skip(3.0);
  require(skipped.complete && skipped.skipped,
          "skip transitions to complete state");
  require_near(skipped.identity_opacity, 0.0, 1e-12, "skip hides identity");
  require_near(skipped.warning_opacity, 0.0, 1e-12, "skip hides warning");
  require_near(skipped.ui_reveal, 1.0, 1e-12, "skip reveals UI");
}

void test_root_scene() {
  auto state = make_users_root_scene();
  const auto initial = sample_root_scene(state, 0.0);
  const auto &users = initial.categories[0];
  require(users.definition.semantic_id == "root.category.users",
          "stable Users category ID");
  require_near(users.center.x, 566.0, 1e-12, "active category x");
  require_near(users.center.y, 279.0, 1e-12, "active category y lift");
  require_near(users.icon_extent, 168.0, 1e-12, "active category extent");
  require_near(users.label_y, 342.0, 1e-12, "category label baseline");
  require_near(users.label_size, 20.0, 1e-12, "category label size");

  require_near(initial.categories[1].center.x, 769.0, 1e-12, "category pitch");
  require_near(initial.categories[1].center.y, 289.0, 1e-12,
               "inactive category baseline");
  require_near(initial.categories[1].icon_extent, 117.0, 1e-12,
               "inactive category extent");
  require_near(initial.categories[1].alpha, 0.9, 1e-12, "near category alpha");
  require_near(initial.categories[3].alpha, 0.25, 1e-12, "far category alpha");

  require(initial.users_items[0].definition.semantic_id ==
              "users.item.turn-off-system",
          "stable Turn Off System ID");
  require(initial.users_items[1].definition.semantic_id ==
              "users.item.create-new-user",
          "stable Create New User ID");
  require(initial.users_items[2].definition.semantic_id ==
              "users.item.current-user",
          "stable current-user ID");
  require_near(initial.users_items[0].icon_center.y, 515.0, 1e-12,
               "focused item y");
  require_near(initial.users_items[1].icon_center.y, 682.0, 1e-12,
               "focused-to-next geometry");
  require_near(initial.users_items[2].icon_center.y, 762.0, 1e-12,
               "item pitch");
  require_near(initial.users_items[0].label_anchor.x, 683.0, 1e-12,
               "item label x");
  require_near(initial.users_items[0].icon_extent, 180.0, 1e-12,
               "focused icon base extent");
  require_near(initial.users_items[0].label_size, 34.0, 1e-12,
               "focused label size");
  require_near(initial.users_items[1].alpha, 0.48, 1e-12,
               "inactive item alpha");
  require_near(initial.users_items[0].scale_tier, 1.3, 1e-12,
               "focused scale tier");

  require(select_users_item(state, 1, 10.0), "select second Users item");
  const auto halfway = sample_root_scene(state, 10.1);
  require_near(halfway.item_transition_progress, 0.5, 1e-9,
               "item transition linear progress");
  // easeOutCubic(0.5) == 0.875: old focused Y 515 -> new above-slot Y 120.
  require_near(halfway.users_items[0].icon_center.y, 169.375, 1e-6,
               "old item eased anchor");
  require_near(halfway.users_items[1].icon_center.y, 535.875, 1e-6,
               "new item eased anchor");
  const auto settled = sample_root_scene(state, 10.2);
  require_near(settled.users_items[0].icon_center.y, 120.0, 1e-9,
               "above-item settled anchor");
  require_near(settled.users_items[1].icon_center.y, 515.0, 1e-9,
               "new focus settled anchor");

  const auto hidden =
      sample_root_scene(state, 10.2, {.ui = 0.0, .label = 0.0, .icons = 0.0});
  require_near(hidden.categories[0].alpha, 0.0, 1e-12, "boot hides categories");
  require_near(hidden.users_items[1].alpha, 0.0, 1e-12, "boot hides items");
}

std::string read_text(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("unable to read shader interface fixture: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void test_shader_interface() {
  const auto source_root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
  const auto vertex =
      read_text(source_root / "shaders/captured_wave.vert");
  const auto fragment =
      read_text(source_root / "shaders/captured_wave.frag");
  const auto renderer =
      read_text(source_root / "src/xmb/renderer/captured_wave_renderer.cppm");

  require(vertex.contains("layout(location = 0) in vec4 in_clip"),
          "shader clip attribute matches renderer location 0 / vec4");
  require(vertex.contains("layout(location = 1) in vec3 in_normal"),
          "shader normal attribute matches renderer location 1 / vec3");
  require(vertex.contains("layout(location = 2) in vec2 in_uv"),
          "shader UV attribute matches renderer location 2 / vec2");
  require(
      vertex.contains("layout(push_constant) uniform CapturedWaveParameters"),
      "vertex shader declares captured-wave push block");
  require(
      fragment.contains("layout(push_constant) uniform CapturedWaveParameters"),
      "fragment shader declares matching push block");
  require(renderer.contains("sizeof(CapturedWaveRenderer::PushConstants)") &&
              renderer.contains("sizeof(float) * 16"),
          "renderer fixes shader push interface at 64 bytes");
  require(renderer.contains("line_passes{7}"),
          "renderer exposes seven reference line passes");
  require(renderer.contains("line_spread_px{5.4F}"),
          "renderer exposes 5.4px line spread");
  require(renderer.contains("CapturedWaveBlendMode::boot_alpha_over"),
          "renderer exposes boot alpha-over mode");
  require(renderer.contains("CapturedWaveBlendMode::idle_additive"),
          "renderer exposes idle additive mode");
}

} // namespace

int main() {
  try {
    test_layout_transforms();
    test_boot_timeline();
    test_root_scene();
    test_shader_interface();
    std::cout << "OpenXMB layout/timeline/root/shader tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "OpenXMB test failure: " << error.what() << '\n';
    return 1;
  }
}
