module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "openxmb/xmb/wave_data.hpp"

export module openxmb.xmb.captured_wave_renderer;

import dreamrender;
import vulkan_hpp;
import vma;

export namespace openxmb::xmb {

enum class CapturedWaveBlendMode : std::uint8_t {
  boot_alpha_over,
  idle_additive,
};

struct CapturedWaveAssetPaths {
  std::filesystem::path geometry;
  std::filesystem::path idle_sequence;
  std::filesystem::path idle_metadata;
  std::filesystem::path boot_sequence;
  std::filesystem::path boot_metadata;
};

struct CapturedWaveAssetOptions {
  std::uint32_t normal_smoothing_passes{3};
};

struct CapturedWaveParameters {
  float tint_r{0.96F};
  float tint_g{0.97F};
  float tint_b{1.00F};
  float gain{1.0F};

  float y_flip{1.0F};
  float scale_y{0.8F};
  float scale_x{1.0F};
  float fill_alpha{0.15F};

  float silk{1.0F};
  float specular_weight{0.30F};
  float specular_exponent{44.8563F};
  float vertical_fade_low{10.0F};
  float vertical_fade_high{11.0F};

  float vertical_lift_ndc{0.0F};
  bool draw_fill{true};
  bool draw_lines{false};
  std::uint32_t line_passes{7};
  float line_spread_px{5.4F};
  float line_alpha{0.18F};
};

class CapturedWaveRenderer {
public:
  CapturedWaveRenderer(vk::Device device, vma::Allocator allocator,
                       vk::Extent2D framebuffer_extent);

  // Loads through the owned parser contract. Any missing/corrupt input throws
  // std::runtime_error with its asset path and parser detail; this renderer
  // never substitutes procedural geometry.
  void load_assets(const CapturedWaveAssetPaths &paths,
                   CapturedWaveAssetOptions options = {});
  void set_assets(WaveGeometry geometry, IdleWaveSequence idle_sequence,
                  BootWaveSequence boot_sequence,
                  CapturedWaveAssetOptions options = {});

  void preload(const std::vector<vk::RenderPass> &render_passes,
               vk::SampleCountFlagBits sample_count,
               vk::PipelineCache pipeline_cache = {});

  // CPU interpolation followed by a bounded update of the host-visible
  // position buffer. These are intentionally separate from draw recording.
  void update_idle(double elapsed_seconds,
                   const IdleSamplingPolicy &policy = {});
  void update_boot(double normalized_progress);

  void render(vk::CommandBuffer command_buffer, vk::RenderPass render_pass,
              CapturedWaveBlendMode blend_mode,
              const CapturedWaveParameters &parameters = {}) const;

  [[nodiscard]] bool assets_ready() const noexcept { return assets_ready_; }
  [[nodiscard]] bool pipelines_ready() const noexcept {
    return pipelines_ready_;
  }
  [[nodiscard]] std::uint32_t grid_size() const noexcept { return grid_size_; }
  [[nodiscard]] std::size_t vertex_count() const noexcept {
    return scratch_positions_.size();
  }
  [[nodiscard]] std::size_t triangle_index_count() const noexcept {
    return triangle_index_count_;
  }
  [[nodiscard]] std::size_t line_index_count() const noexcept {
    return line_index_count_;
  }

  // Public only so the module's Vulkan layout checks can name these PODs;
  // they are renderer implementation records, not application scene types.
  struct AttributeVertex {
    float normal_x{};
    float normal_y{};
    float normal_z{};
    float uv_u{};
    float uv_v{};
  };

  struct PushConstants {
    std::array<float, 4> tint_and_gain{};
    std::array<float, 4> transform_alpha{};
    std::array<float, 4> material{};
    std::array<float, 4> fade_offset{};
  };

private:
  void upload_positions();

  vk::Device device_;
  vma::Allocator allocator_;
  vk::Extent2D framebuffer_extent_;

  std::uint32_t grid_size_{};
  IdleWaveSequence idle_sequence_;
  BootWaveSequence boot_sequence_;
  std::vector<WavePosition> scratch_positions_;

  vma::UniqueBuffer position_buffer_;
  vma::UniqueAllocation position_allocation_;
  vma::UniqueBuffer attribute_buffer_;
  vma::UniqueAllocation attribute_allocation_;
  vma::UniqueBuffer triangle_index_buffer_;
  vma::UniqueAllocation triangle_index_allocation_;
  vma::UniqueBuffer line_index_buffer_;
  vma::UniqueAllocation line_index_allocation_;
  std::size_t triangle_index_count_{};
  std::size_t line_index_count_{};

  vk::UniquePipelineLayout pipeline_layout_;
  dreamrender::UniquePipelineMap boot_fill_pipelines_;
  dreamrender::UniquePipelineMap boot_line_pipelines_;
  dreamrender::UniquePipelineMap idle_fill_pipelines_;
  dreamrender::UniquePipelineMap idle_line_pipelines_;

  bool assets_ready_{};
  bool pipelines_ready_{};
};

} // namespace openxmb::xmb

namespace openxmb::xmb {
namespace {

static_assert(sizeof(WavePosition) == sizeof(float) * 4);
static_assert(sizeof(CapturedWaveRenderer::AttributeVertex) ==
              sizeof(float) * 5);
static_assert(sizeof(CapturedWaveRenderer::PushConstants) ==
              sizeof(float) * 16);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kCapturedWaveVertexBytes[] = {
#embed "shaders/captured_wave.vert.spv"
};
constexpr char kCapturedWaveFragmentBytes[] = {
#embed "shaders/captured_wave.frag.spv"
};
#pragma clang diagnostic pop

constexpr std::array kCapturedWaveVertexShader =
    dreamrender::convert<std::to_array(kCapturedWaveVertexBytes),
                         std::uint32_t>();
constexpr std::array kCapturedWaveFragmentShader =
    dreamrender::convert<std::to_array(kCapturedWaveFragmentBytes),
                         std::uint32_t>();

[[nodiscard]] std::string describe_wave_error(std::string_view operation,
                                              const WaveError &error) {
  auto result = std::string("OpenXMB captured wave ") + std::string(operation);
  if (!error.source.empty()) {
    result += " [" + error.source + "]";
  }
  if (!error.detail.empty()) {
    result += ": " + error.detail;
  }
  if (error.byte_offset != 0) {
    result += " (byte offset " + std::to_string(error.byte_offset) + ")";
  }
  return result;
}

void require_finite(float value, std::string_view field) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("OpenXMB captured wave contains a non-finite " +
                             std::string(field));
  }
}

[[nodiscard]] std::vector<CapturedWaveRenderer::AttributeVertex>
make_attributes(const WaveGeometry &geometry, std::uint32_t smoothing_passes) {
  const auto count = geometry.vertices.size();
  const auto grid = static_cast<std::size_t>(geometry.grid_size);
  std::vector<float> normal_x(count);
  std::vector<float> normal_y(count);
  std::vector<float> normal_z(count);

  for (std::size_t index = 0; index < count; ++index) {
    normal_x[index] = geometry.vertices[index].normal.x;
    normal_y[index] = geometry.vertices[index].normal.y;
    normal_z[index] = geometry.vertices[index].normal.z;
    require_finite(normal_x[index], "normal.x");
    require_finite(normal_y[index], "normal.y");
    require_finite(normal_z[index], "normal.z");
    require_finite(geometry.vertices[index].uv.u, "uv.u");
    require_finite(geometry.vertices[index].uv.v, "uv.v");
  }

  const auto blur_axis = [grid](const std::vector<float> &source,
                                bool horizontal) {
    std::vector<float> output(source.size());
    for (std::size_t row = 0; row < grid; ++row) {
      for (std::size_t column = 0; column < grid; ++column) {
        float sum = 0.0F;
        for (int offset = -1; offset <= 1; ++offset) {
          const auto sample_row =
              horizontal ? row
                         : static_cast<std::size_t>(std::clamp(
                               static_cast<std::ptrdiff_t>(row) + offset,
                               std::ptrdiff_t{0},
                               static_cast<std::ptrdiff_t>(grid - 1)));
          const auto sample_column =
              horizontal ? static_cast<std::size_t>(std::clamp(
                               static_cast<std::ptrdiff_t>(column) + offset,
                               std::ptrdiff_t{0},
                               static_cast<std::ptrdiff_t>(grid - 1)))
                         : column;
          sum += source[sample_row * grid + sample_column];
        }
        output[row * grid + column] = sum / 3.0F;
      }
    }
    return output;
  };

  for (std::uint32_t pass = 0; pass < smoothing_passes; ++pass) {
    normal_x = blur_axis(blur_axis(normal_x, true), false);
    normal_y = blur_axis(blur_axis(normal_y, true), false);
    normal_z = blur_axis(blur_axis(normal_z, true), false);
  }

  std::vector<CapturedWaveRenderer::AttributeVertex> attributes(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto length = std::sqrt(normal_x[index] * normal_x[index] +
                                  normal_y[index] * normal_y[index] +
                                  normal_z[index] * normal_z[index]);
    const auto divisor =
        length > std::numeric_limits<float>::epsilon() ? length : 1.0F;
    attributes[index] = {
        .normal_x = normal_x[index] / divisor,
        .normal_y = normal_y[index] / divisor,
        .normal_z = normal_z[index] / divisor,
        .uv_u = geometry.vertices[index].uv.u,
        .uv_v = geometry.vertices[index].uv.v,
    };
  }
  return attributes;
}

[[nodiscard]] vk::PipelineColorBlendAttachmentState
blend_attachment(CapturedWaveBlendMode mode) {
  const auto destination_color = mode == CapturedWaveBlendMode::idle_additive
                                     ? vk::BlendFactor::eOne
                                     : vk::BlendFactor::eOneMinusSrcAlpha;
  vk::PipelineColorBlendAttachmentState attachment(
      true, vk::BlendFactor::eSrcAlpha, destination_color, vk::BlendOp::eAdd,
      vk::BlendFactor::eOne, vk::BlendFactor::eOneMinusSrcAlpha,
      vk::BlendOp::eAdd);
  attachment.colorWriteMask =
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  return attachment;
}

} // namespace

CapturedWaveRenderer::CapturedWaveRenderer(vk::Device device,
                                           vma::Allocator allocator,
                                           vk::Extent2D framebuffer_extent)
    : device_(device), allocator_(allocator),
      framebuffer_extent_(framebuffer_extent) {
  if (!device_ || !allocator_) {
    throw std::invalid_argument("OpenXMB captured wave renderer requires a "
                                "Vulkan device and VMA allocator");
  }
  if (framebuffer_extent_.width == 0 || framebuffer_extent_.height == 0) {
    throw std::invalid_argument(
        "OpenXMB captured wave renderer requires a non-zero framebuffer extent");
  }
}

void CapturedWaveRenderer::load_assets(const CapturedWaveAssetPaths &paths,
                                       CapturedWaveAssetOptions options) {
  auto geometry = load_wave_geometry(paths.geometry);
  if (!geometry) {
    throw std::runtime_error(
        describe_wave_error("geometry load failed", geometry.error()));
  }
  auto idle = load_idle_wave_sequence(paths.idle_sequence, paths.idle_metadata);
  if (!idle) {
    throw std::runtime_error(
        describe_wave_error("idle sequence load failed", idle.error()));
  }
  auto boot = load_boot_wave_sequence(paths.boot_sequence, paths.boot_metadata);
  if (!boot) {
    throw std::runtime_error(
        describe_wave_error("boot sequence load failed", boot.error()));
  }
  set_assets(std::move(geometry).value(), std::move(idle).value(),
             std::move(boot).value(), options);
}

void CapturedWaveRenderer::set_assets(WaveGeometry geometry,
                                      IdleWaveSequence idle_sequence,
                                      BootWaveSequence boot_sequence,
                                      CapturedWaveAssetOptions options) {
  const auto expected_vertices = static_cast<std::size_t>(geometry.grid_size) *
                                 static_cast<std::size_t>(geometry.grid_size);
  if (geometry.grid_size != kWaveGridSize ||
      geometry.vertices.size() != expected_vertices) {
    throw std::runtime_error(
        "OpenXMB captured wave geometry must be the authoritative 128x128 grid");
  }
  if (idle_sequence.grid_size != geometry.grid_size ||
      idle_sequence.frames.empty()) {
    throw std::runtime_error("OpenXMB captured wave idle sequence is missing or "
                             "does not match the geometry grid");
  }
  if (boot_sequence.grid_size != geometry.grid_size ||
      boot_sequence.frames.empty()) {
    throw std::runtime_error("OpenXMB captured wave boot sequence is missing or "
                             "does not match the geometry grid");
  }

  scratch_positions_.resize(expected_vertices);
  for (std::size_t index = 0; index < expected_vertices; ++index) {
    const auto &position = geometry.vertices[index].clip;
    require_finite(position.x, "clip.x");
    require_finite(position.y, "clip.y");
    require_finite(position.z, "clip.z");
    require_finite(position.w, "clip.w");
    if (std::abs(position.w) <= std::numeric_limits<float>::epsilon()) {
      throw std::runtime_error(
          "OpenXMB captured wave geometry contains clip.w == 0");
    }
    scratch_positions_[index] = position;
  }

  const auto attributes =
      make_attributes(geometry, options.normal_smoothing_passes);
  auto triangle_indices_result = generate_wave_grid_indices(
      geometry.grid_size, WaveIndexTopology::triangles);
  if (!triangle_indices_result) {
    throw std::runtime_error(describe_wave_error(
        "triangle index generation failed", triangle_indices_result.error()));
  }
  auto line_indices_result =
      generate_wave_grid_indices(geometry.grid_size, WaveIndexTopology::lines);
  if (!line_indices_result) {
    throw std::runtime_error(describe_wave_error("line index generation failed",
                                                 line_indices_result.error()));
  }
  auto triangle_indices = std::move(triangle_indices_result).value();
  auto line_indices = std::move(line_indices_result).value();

  const auto triangle_validation = validate_wave_grid_indices(
      triangle_indices, geometry.grid_size, WaveIndexTopology::triangles);
  if (!triangle_validation) {
    throw std::runtime_error(describe_wave_error(
        "triangle index validation failed", triangle_validation.error()));
  }
  const auto line_validation = validate_wave_grid_indices(
      line_indices, geometry.grid_size, WaveIndexTopology::lines);
  if (!line_validation) {
    throw std::runtime_error(describe_wave_error("line index validation failed",
                                                 line_validation.error()));
  }

  if (triangle_indices.size() > std::numeric_limits<std::uint32_t>::max() ||
      line_indices.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(
        "OpenXMB captured wave index count exceeds Vulkan draw limits");
  }

  std::tie(position_buffer_, position_allocation_) =
      allocator_.createBufferUnique(
          vk::BufferCreateInfo({},
                               scratch_positions_.size() * sizeof(WavePosition),
                               vk::BufferUsageFlagBits::eVertexBuffer),
          vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
  std::tie(attribute_buffer_, attribute_allocation_) =
      allocator_.createBufferUnique(
          vk::BufferCreateInfo({}, attributes.size() * sizeof(AttributeVertex),
                               vk::BufferUsageFlagBits::eVertexBuffer),
          vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
  std::tie(triangle_index_buffer_, triangle_index_allocation_) =
      allocator_.createBufferUnique(
          vk::BufferCreateInfo({},
                               triangle_indices.size() * sizeof(std::uint32_t),
                               vk::BufferUsageFlagBits::eIndexBuffer),
          vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
  std::tie(line_index_buffer_, line_index_allocation_) =
      allocator_.createBufferUnique(
          vk::BufferCreateInfo({}, line_indices.size() * sizeof(std::uint32_t),
                               vk::BufferUsageFlagBits::eIndexBuffer),
          vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));

  allocator_.copyMemoryToAllocation(
      attributes.data(), attribute_allocation_.get(), 0,
      attributes.size() * sizeof(AttributeVertex));
  allocator_.flushAllocation(attribute_allocation_.get(), 0,
                             attributes.size() * sizeof(AttributeVertex));
  allocator_.copyMemoryToAllocation(
      triangle_indices.data(), triangle_index_allocation_.get(), 0,
      triangle_indices.size() * sizeof(std::uint32_t));
  allocator_.flushAllocation(triangle_index_allocation_.get(), 0,
                             triangle_indices.size() * sizeof(std::uint32_t));
  allocator_.copyMemoryToAllocation(
      line_indices.data(), line_index_allocation_.get(), 0,
      line_indices.size() * sizeof(std::uint32_t));
  allocator_.flushAllocation(line_index_allocation_.get(), 0,
                             line_indices.size() * sizeof(std::uint32_t));

  upload_positions();
  grid_size_ = geometry.grid_size;
  triangle_index_count_ = triangle_indices.size();
  line_index_count_ = line_indices.size();
  idle_sequence_ = std::move(idle_sequence);
  boot_sequence_ = std::move(boot_sequence);
  assets_ready_ = true;
}

void CapturedWaveRenderer::preload(
    const std::vector<vk::RenderPass> &render_passes,
    vk::SampleCountFlagBits sample_count, vk::PipelineCache pipeline_cache) {
  if (render_passes.empty()) {
    throw std::invalid_argument(
        "OpenXMB captured wave preload requires at least one render pass");
  }

  const vk::PushConstantRange push_range(vk::ShaderStageFlagBits::eVertex |
                                             vk::ShaderStageFlagBits::eFragment,
                                         0, sizeof(PushConstants));
  pipeline_layout_ = device_.createPipelineLayoutUnique(
      vk::PipelineLayoutCreateInfo({}, {}, push_range));

  const std::array bindings{
      vk::VertexInputBindingDescription(0, sizeof(WavePosition),
                                        vk::VertexInputRate::eVertex),
      vk::VertexInputBindingDescription(1, sizeof(AttributeVertex),
                                        vk::VertexInputRate::eVertex),
  };
  const std::array attributes{
      vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32B32A32Sfloat,
                                          0),
      vk::VertexInputAttributeDescription(
          1, 1, vk::Format::eR32G32B32Sfloat,
          static_cast<std::uint32_t>(offsetof(AttributeVertex, normal_x))),
      vk::VertexInputAttributeDescription(
          2, 1, vk::Format::eR32G32Sfloat,
          static_cast<std::uint32_t>(offsetof(AttributeVertex, uv_u))),
  };
  const vk::PipelineVertexInputStateCreateInfo vertex_input({}, bindings,
                                                            attributes);
  const vk::Viewport empty_viewport{};
  const vk::Rect2D empty_scissor{};
  const vk::PipelineViewportStateCreateInfo viewport_state({}, empty_viewport,
                                                           empty_scissor);
  const vk::PipelineRasterizationStateCreateInfo rasterization(
      {}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone,
      vk::FrontFace::eCounterClockwise, false, 0.0F, 0.0F, 0.0F, 1.0F);
  const vk::PipelineMultisampleStateCreateInfo multisample({}, sample_count);
  const vk::PipelineDepthStencilStateCreateInfo depth_stencil({}, false, false);
  const std::array dynamic_states{vk::DynamicState::eViewport,
                                  vk::DynamicState::eScissor};
  const vk::PipelineDynamicStateCreateInfo dynamic({}, dynamic_states);

  const auto vertex_shader =
      dreamrender::createShader(device_, kCapturedWaveVertexShader);
  const auto fragment_shader =
      dreamrender::createShader(device_, kCapturedWaveFragmentShader);
  const std::array stages{
      vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex,
                                        vertex_shader.get(), "main"),
      vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment,
                                        fragment_shader.get(), "main"),
  };

  const auto make_pipelines = [&](vk::PrimitiveTopology topology,
                                  CapturedWaveBlendMode blend_mode,
                                  const char *debug_name) {
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, topology);
    const auto attachment = blend_attachment(blend_mode);
    const vk::PipelineColorBlendStateCreateInfo color_blend(
        {}, false, vk::LogicOp::eClear, attachment);
    const vk::GraphicsPipelineCreateInfo pipeline_info(
        {}, stages, &vertex_input, &input_assembly, nullptr, &viewport_state,
        &rasterization, &multisample, &depth_stencil, &color_blend, &dynamic,
        pipeline_layout_.get(), {});
    return dreamrender::createPipelines(device_, pipeline_cache, pipeline_info,
                                        render_passes, debug_name);
  };

  boot_fill_pipelines_ = make_pipelines(vk::PrimitiveTopology::eTriangleList,
                                        CapturedWaveBlendMode::boot_alpha_over,
                                        "OpenXMB Captured Wave Boot Fill");
  boot_line_pipelines_ = make_pipelines(vk::PrimitiveTopology::eLineList,
                                        CapturedWaveBlendMode::boot_alpha_over,
                                        "OpenXMB Captured Wave Boot Lines");
  idle_fill_pipelines_ = make_pipelines(vk::PrimitiveTopology::eTriangleList,
                                        CapturedWaveBlendMode::idle_additive,
                                        "OpenXMB Captured Wave Idle Fill");
  idle_line_pipelines_ = make_pipelines(vk::PrimitiveTopology::eLineList,
                                        CapturedWaveBlendMode::idle_additive,
                                        "OpenXMB Captured Wave Idle Lines");
  pipelines_ready_ = true;
}

void CapturedWaveRenderer::update_idle(double elapsed_seconds,
                                       const IdleSamplingPolicy &policy) {
  if (!assets_ready_) {
    throw std::logic_error(
        "OpenXMB captured wave idle update requested before assets were loaded");
  }
  const auto result = sample_idle_wave_into(idle_sequence_, elapsed_seconds,
                                            scratch_positions_, policy);
  if (!result) {
    throw std::runtime_error(
        describe_wave_error("idle interpolation failed", result.error()));
  }
  upload_positions();
}

void CapturedWaveRenderer::update_boot(double normalized_progress) {
  if (!assets_ready_) {
    throw std::logic_error(
        "OpenXMB captured wave boot update requested before assets were loaded");
  }
  const auto result = sample_boot_wave_into(boot_sequence_, normalized_progress,
                                            scratch_positions_);
  if (!result) {
    throw std::runtime_error(
        describe_wave_error("boot interpolation failed", result.error()));
  }
  upload_positions();
}

void CapturedWaveRenderer::upload_positions() {
  if (!position_allocation_) {
    throw std::logic_error("OpenXMB captured wave position upload requested "
                           "without a GPU allocation");
  }
  const auto byte_count = scratch_positions_.size() * sizeof(WavePosition);
  allocator_.copyMemoryToAllocation(scratch_positions_.data(),
                                    position_allocation_.get(), 0, byte_count);
  allocator_.flushAllocation(position_allocation_.get(), 0, byte_count);
}

void CapturedWaveRenderer::render(
    vk::CommandBuffer command_buffer, vk::RenderPass render_pass,
    CapturedWaveBlendMode blend_mode,
    const CapturedWaveParameters &parameters) const {
  if (!assets_ready_) {
    throw std::logic_error(
        "OpenXMB captured wave draw requested before assets were loaded");
  }
  if (!pipelines_ready_) {
    throw std::logic_error(
        "OpenXMB captured wave draw requested before pipeline preload");
  }
  if (!parameters.draw_fill && !parameters.draw_lines) {
    return;
  }
  if (parameters.draw_lines && parameters.line_passes == 0) {
    throw std::invalid_argument(
        "OpenXMB captured wave line_passes must be non-zero");
  }

  const auto &fill_pipelines =
      blend_mode == CapturedWaveBlendMode::boot_alpha_over
          ? boot_fill_pipelines_
          : idle_fill_pipelines_;
  const auto &line_pipelines =
      blend_mode == CapturedWaveBlendMode::boot_alpha_over
          ? boot_line_pipelines_
          : idle_line_pipelines_;
  const auto fill_pipeline = fill_pipelines.find(render_pass);
  const auto line_pipeline = line_pipelines.find(render_pass);
  if ((parameters.draw_fill && fill_pipeline == fill_pipelines.end()) ||
      (parameters.draw_lines && line_pipeline == line_pipelines.end())) {
    throw std::runtime_error(
        "OpenXMB captured wave has no pipeline for the active render pass");
  }

  const vk::Viewport viewport(
      0.0F, 0.0F, static_cast<float>(framebuffer_extent_.width),
      static_cast<float>(framebuffer_extent_.height), 0.0F, 1.0F);
  const vk::Rect2D scissor({0, 0}, framebuffer_extent_);
  command_buffer.setViewport(0, viewport);
  command_buffer.setScissor(0, scissor);

  const std::array buffers{position_buffer_.get(), attribute_buffer_.get()};
  constexpr std::array<vk::DeviceSize, 2> offsets{0, 0};
  command_buffer.bindVertexBuffers(0, buffers, offsets);

  const auto make_push = [&](float alpha, float offset_y) {
    return PushConstants{
        .tint_and_gain = {parameters.tint_r, parameters.tint_g,
                          parameters.tint_b, parameters.gain},
        .transform_alpha = {parameters.y_flip, parameters.scale_y,
                            parameters.scale_x, alpha},
        .material = {parameters.silk, parameters.specular_weight,
                     parameters.specular_exponent,
                     parameters.vertical_fade_low},
        .fade_offset = {parameters.vertical_fade_high, 0.0F, offset_y, 0.0F},
    };
  };

  const auto push = [&](const PushConstants &constants) {
    command_buffer.pushConstants(pipeline_layout_.get(),
                                 vk::ShaderStageFlagBits::eVertex |
                                     vk::ShaderStageFlagBits::eFragment,
                                 0, sizeof(constants), &constants);
  };

  if (parameters.draw_fill) {
    command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                fill_pipeline->second.get());
    const auto constants =
        make_push(parameters.fill_alpha, parameters.vertical_lift_ndc);
    push(constants);
    command_buffer.bindIndexBuffer(triangle_index_buffer_.get(), 0,
                                   vk::IndexType::eUint32);
    command_buffer.drawIndexed(
        static_cast<std::uint32_t>(triangle_index_count_), 1, 0, 0, 0);
  }

  if (parameters.draw_lines) {
    command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                line_pipeline->second.get());
    command_buffer.bindIndexBuffer(line_index_buffer_.get(), 0,
                                   vk::IndexType::eUint32);

    const auto pass_count = parameters.line_passes;
    const auto alpha =
        parameters.line_alpha * (2.6F / static_cast<float>(pass_count));
    const auto ndc_per_pixel =
        2.0F / static_cast<float>(framebuffer_extent_.height);
    for (std::uint32_t pass = 0; pass < pass_count; ++pass) {
      const auto spread =
          pass_count > 1
              ? static_cast<float>(pass) / static_cast<float>(pass_count - 1) -
                    0.5F
              : 0.0F;
      const auto constants = make_push(
          alpha, parameters.vertical_lift_ndc +
                     spread * parameters.line_spread_px * ndc_per_pixel);
      push(constants);
      command_buffer.drawIndexed(static_cast<std::uint32_t>(line_index_count_),
                                 1, 0, 0, 0);
    }
  }
}

} // namespace openxmb::xmb
