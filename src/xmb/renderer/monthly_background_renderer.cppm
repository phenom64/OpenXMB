module;

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "openxmb/xmb/background.hpp"

export module openxmb.xmb.monthly_background_renderer;

import dreamrender;
import vulkan_hpp;

export namespace openxmb::xmb {

class MonthlyBackgroundRenderer {
public:
  MonthlyBackgroundRenderer(vk::Device device, vk::Extent2D framebuffer_extent)
      : device_(device), framebuffer_extent_(framebuffer_extent) {}

  void preload(const std::vector<vk::RenderPass> &render_passes,
               vk::SampleCountFlagBits sample_count,
               vk::PipelineCache pipeline_cache = {});
  void render(vk::CommandBuffer command_buffer, vk::RenderPass render_pass,
              const BackgroundGradient &gradient,
              std::array<float, 4> boot = {1.0F, 1.0F, 1.0F, 0.0F}) const;

  [[nodiscard]] bool pipelines_ready() const noexcept {
    return pipelines_ready_;
  }

  struct PushConstants {
    std::array<float, 4> top_and_night{};
    std::array<float, 4> bottom_and_reserved{};
    std::array<float, 4> boot{};
  };

private:
  vk::Device device_;
  vk::Extent2D framebuffer_extent_;
  vk::UniquePipelineLayout pipeline_layout_;
  dreamrender::UniquePipelineMap pipelines_;
  bool pipelines_ready_{};
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

static_assert(sizeof(MonthlyBackgroundRenderer::PushConstants) ==
              sizeof(float) * 12);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kMonthlyBackgroundVertexBytes[] = {
#embed "shaders/monthly_background.vert.spv"
};
constexpr char kMonthlyBackgroundFragmentBytes[] = {
#embed "shaders/monthly_background.frag.spv"
};
#pragma clang diagnostic pop

constexpr std::array kMonthlyBackgroundVertexShader =
    dreamrender::convert<std::to_array(kMonthlyBackgroundVertexBytes),
                         std::uint32_t>();
constexpr std::array kMonthlyBackgroundFragmentShader =
    dreamrender::convert<std::to_array(kMonthlyBackgroundFragmentBytes),
                         std::uint32_t>();

} // namespace

void MonthlyBackgroundRenderer::preload(
    const std::vector<vk::RenderPass> &render_passes,
    vk::SampleCountFlagBits sample_count, vk::PipelineCache pipeline_cache) {
  if (render_passes.empty()) {
    throw std::invalid_argument(
        "OpenXMB monthly background preload requires a render pass");
  }

  const vk::PushConstantRange push_range(vk::ShaderStageFlagBits::eFragment, 0,
                                         sizeof(PushConstants));
  pipeline_layout_ = device_.createPipelineLayoutUnique(
      vk::PipelineLayoutCreateInfo({}, {}, push_range));

  const auto vertex_shader =
      dreamrender::createShader(device_, kMonthlyBackgroundVertexShader);
  const auto fragment_shader =
      dreamrender::createShader(device_, kMonthlyBackgroundFragmentShader);
  const std::array stages{
      vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex,
                                        vertex_shader.get(), "main"),
      vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment,
                                        fragment_shader.get(), "main"),
  };

  const vk::PipelineVertexInputStateCreateInfo vertex_input{};
  const vk::PipelineInputAssemblyStateCreateInfo input_assembly(
      {}, vk::PrimitiveTopology::eTriangleList);
  const vk::Viewport empty_viewport{};
  const vk::Rect2D empty_scissor{};
  const vk::PipelineViewportStateCreateInfo viewport_state({}, empty_viewport,
                                                           empty_scissor);
  const vk::PipelineRasterizationStateCreateInfo rasterization(
      {}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone,
      vk::FrontFace::eCounterClockwise, false, 0.0F, 0.0F, 0.0F, 1.0F);
  const vk::PipelineMultisampleStateCreateInfo multisample({}, sample_count);
  const vk::PipelineDepthStencilStateCreateInfo depth_stencil({}, false, false);

  vk::PipelineColorBlendAttachmentState attachment{};
  attachment.blendEnable = false;
  attachment.colorWriteMask =
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  const vk::PipelineColorBlendStateCreateInfo color_blend(
      {}, false, vk::LogicOp::eClear, attachment);
  const std::array dynamic_states{vk::DynamicState::eViewport,
                                  vk::DynamicState::eScissor};
  const vk::PipelineDynamicStateCreateInfo dynamic({}, dynamic_states);

  const vk::GraphicsPipelineCreateInfo pipeline_info(
      {}, stages, &vertex_input, &input_assembly, nullptr, &viewport_state,
      &rasterization, &multisample, &depth_stencil, &color_blend, &dynamic,
      pipeline_layout_.get(), {});
  pipelines_ = dreamrender::createPipelines(
      device_, pipeline_cache, pipeline_info, render_passes,
      "OpenXMB Monthly Background");
  pipelines_ready_ = true;
}

void MonthlyBackgroundRenderer::render(
    vk::CommandBuffer command_buffer, vk::RenderPass render_pass,
    const BackgroundGradient &gradient, std::array<float, 4> boot) const {
  if (!pipelines_ready_) {
    throw std::logic_error(
        "OpenXMB monthly background draw requested before pipeline preload");
  }
  const auto pipeline = pipelines_.find(render_pass);
  if (pipeline == pipelines_.end()) {
    throw std::runtime_error(
        "OpenXMB monthly background has no pipeline for the active render pass");
  }

  const vk::Viewport viewport(
      0.0F, 0.0F, static_cast<float>(framebuffer_extent_.width),
      static_cast<float>(framebuffer_extent_.height), 0.0F, 1.0F);
  const vk::Rect2D scissor({0, 0}, framebuffer_extent_);
  command_buffer.setViewport(0, viewport);
  command_buffer.setScissor(0, scissor);
  command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                              pipeline->second.get());

  const PushConstants constants{
      .top_and_night = {gradient.top_rgb[0], gradient.top_rgb[1],
                        gradient.top_rgb[2], gradient.night_day_blend},
      .bottom_and_reserved = {gradient.bottom_rgb[0], gradient.bottom_rgb[1],
                              gradient.bottom_rgb[2], 0.0F},
      .boot = boot,
  };
  command_buffer.pushConstants(pipeline_layout_.get(),
                               vk::ShaderStageFlagBits::eFragment, 0,
                               sizeof(constants), &constants);
  command_buffer.draw(3, 1, 0, 0);
}

} // namespace openxmb::xmb
