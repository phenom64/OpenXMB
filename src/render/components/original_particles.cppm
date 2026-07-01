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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <random>
#include <stdexcept>
#include <tuple>
#include <vector>

export module openxmb.render:particles_renderer;

import dreamrender;
import :shaders;

import glm;
import spdlog;
import vulkan_hpp;
import vma;

namespace render {

// Additive particle sprite renderer for the Original background
export class particles_renderer {
  public:
    static constexpr uint32_t kParticles = 1400; // xmb-web's base Original-background cloud density

    struct ParticleInstance {
      glm::vec4 home_age;    // xyz = eye-space home, w = phase/lifetime
      glm::vec4 params;      // xy = deterministic seeds, z = edge flag, w = size scale
      glm::vec4 tint_spin;   // rgb = metallic edge tint, a = normal spin phase 0
      glm::vec4 spin_misc;   // x = normal spin phase 1, y/z = spin rates
    };

    particles_renderer(vk::Device device, vma::Allocator allocator, vk::Extent2D frameSize)
      : device(device), allocator(allocator), frameSize(frameSize), aspectRatio(static_cast<double>(frameSize.width)/frameSize.height) {}
    ~particles_renderer() = default;

    void load_particle_cloud(const std::filesystem::path& path)
    {
      std::ifstream stream(path, std::ios::binary);
      if(!stream) {
        throw std::runtime_error("cannot open particle cloud " + path.string());
      }
      stream.seekg(0, std::ios::end);
      const auto end = stream.tellg();
      if(end <= 0 || static_cast<std::uint64_t>(end) % (sizeof(float) * 3u) != 0u) {
        throw std::runtime_error("particle cloud has invalid byte size " + path.string());
      }
      stream.seekg(0, std::ios::beg);
      std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
      stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      if(stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw std::runtime_error("particle cloud read was truncated " + path.string());
      }

      std::vector<glm::vec3> loaded;
      loaded.reserve(bytes.size() / (sizeof(float) * 3u));
      auto read_le_float = [&](std::size_t offset) {
        const auto b0 = static_cast<std::uint32_t>(bytes[offset + 0]);
        const auto b1 = static_cast<std::uint32_t>(bytes[offset + 1]);
        const auto b2 = static_cast<std::uint32_t>(bytes[offset + 2]);
        const auto b3 = static_cast<std::uint32_t>(bytes[offset + 3]);
        const auto bits = b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u);
        return std::bit_cast<float>(bits);
      };
      for(std::size_t offset = 0; offset < bytes.size(); offset += sizeof(float) * 3u) {
        const auto x = read_le_float(offset + 0u);
        const auto y = read_le_float(offset + 4u);
        const auto z = read_le_float(offset + 8u);
        if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
          throw std::runtime_error("particle cloud contains non-finite coordinates " + path.string());
        }
        loaded.emplace_back(x, y, z);
      }
      particleCloud = std::move(loaded);
      spdlog::info("Loaded xmb-web particle cloud: {} eye-space points from {}",
                   particleCloud.size(), path.string());
    }

    void preload(const std::vector<vk::RenderPass>& renderPasses,
                 vk::SampleCountFlagBits sampleCount,
                 vk::PipelineCache pipelineCache = {})
    {
      // Quad vertices in NDC-local space (unit sprite expanded in VS)
      const std::array<glm::vec2, 4> quad = {
        glm::vec2(-0.5f, -0.5f), glm::vec2(0.5f, -0.5f),
        glm::vec2(-0.5f,  0.5f), glm::vec2(0.5f,  0.5f)
      };
      const std::array<uint16_t, 6> idx = {0,1,2,2,1,3};

      std::tie(quadVB, quadVBAlloc) = allocator.createBufferUnique(
        vk::BufferCreateInfo({}, quad.size()*sizeof(quad[0]), vk::BufferUsageFlagBits::eVertexBuffer),
        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
      allocator.copyMemoryToAllocation(quad.data(), quadVBAlloc.get(), 0, quad.size()*sizeof(quad[0]));
      allocator.flushAllocation(quadVBAlloc.get(), 0, quad.size()*sizeof(quad[0]));

      std::tie(indexBuffer, indexAlloc) = allocator.createBufferUnique(
        vk::BufferCreateInfo({}, idx.size()*sizeof(idx[0]), vk::BufferUsageFlagBits::eIndexBuffer),
        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
      allocator.copyMemoryToAllocation(idx.data(), indexAlloc.get(), 0, idx.size()*sizeof(idx[0]));
      allocator.flushAllocation(indexAlloc.get(), 0, idx.size()*sizeof(idx[0]));

      // Instance buffer: deterministic xmb-web-style particle homes. When the
      // local compatibility pack is present these homes are sampled from the
      // firmware-extracted eye-space PARTICLE_CLOUD. Clean builds keep a
      // narrow analytic wave-band fallback instead of failing Original outright.
      const auto particles = make_instances();
      std::tie(instanceVB, instanceVBAlloc) = allocator.createBufferUnique(
        vk::BufferCreateInfo({}, particles.size()*sizeof(particles[0]), vk::BufferUsageFlagBits::eVertexBuffer),
        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
      allocator.copyMemoryToAllocation(particles.data(), instanceVBAlloc.get(), 0, particles.size()*sizeof(particles[0]));
      allocator.flushAllocation(instanceVBAlloc.get(), 0, particles.size()*sizeof(particles[0]));

      // Pipeline layout: push-constants only
      vk::PushConstantRange range(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push));
      pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, nullptr, range));

      // Pipeline state
      vk::UniqueShaderModule vs = shaders::original_particles::vert(device);
      vk::UniqueShaderModule fs = shaders::original_particles::frag(device);
      std::array<vk::PipelineShaderStageCreateInfo,2> stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, vs.get(), "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fs.get(), "main")
      };

      std::array<vk::VertexInputBindingDescription,2> binds = {
        vk::VertexInputBindingDescription(0, sizeof(glm::vec2), vk::VertexInputRate::eVertex),   // quad
        vk::VertexInputBindingDescription(1, sizeof(ParticleInstance), vk::VertexInputRate::eInstance)
      };
      std::array<vk::VertexInputAttributeDescription,5> attrs = {
        vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat, 0), // inPos
        vk::VertexInputAttributeDescription(1, 1, vk::Format::eR32G32B32A32Sfloat, offsetof(ParticleInstance, home_age)),
        vk::VertexInputAttributeDescription(2, 1, vk::Format::eR32G32B32A32Sfloat, offsetof(ParticleInstance, params)),
        vk::VertexInputAttributeDescription(3, 1, vk::Format::eR32G32B32A32Sfloat, offsetof(ParticleInstance, tint_spin)),
        vk::VertexInputAttributeDescription(4, 1, vk::Format::eR32G32B32A32Sfloat, offsetof(ParticleInstance, spin_misc))
      };
      vk::PipelineVertexInputStateCreateInfo vertexInput({}, binds, attrs);
      vk::PipelineInputAssemblyStateCreateInfo inputAsm({}, vk::PrimitiveTopology::eTriangleList);
      vk::Viewport v{};
      vk::Rect2D s{};
      vk::PipelineViewportStateCreateInfo viewport({}, v, s);
      vk::PipelineRasterizationStateCreateInfo rast({}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
      vk::PipelineMultisampleStateCreateInfo ms({}, sampleCount);
      vk::PipelineDepthStencilStateCreateInfo ds({}, false, false);

      // Additive blending (RGB), preserve destination alpha
      vk::PipelineColorBlendAttachmentState att(true,
        vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendOp::eAdd,
        vk::BlendFactor::eZero, vk::BlendFactor::eOne, vk::BlendOp::eAdd);
      att.colorWriteMask = vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA;
      vk::PipelineColorBlendStateCreateInfo blend({}, false, vk::LogicOp::eClear, att);

      std::array<vk::DynamicState,2> dynStates{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
      vk::PipelineDynamicStateCreateInfo dyn({}, dynStates);

      vk::GraphicsPipelineCreateInfo gp({}, stages, &vertexInput, &inputAsm, nullptr, &viewport, &rast, &ms, &ds, &blend, &dyn, pipelineLayout.get());
      pipelines = dreamrender::createPipelines(device, pipelineCache, gp, renderPasses, "Original Particles Pipeline");
    }

    void prepare(int /*imageCount*/) {}

    struct Push {
      glm::vec4 tint;                        // retained for shader ABI evolution
      glm::vec4 resolution_time_brightness;  // xy=width/height, z=time, w=brightness
      glm::vec4 particle_params;             // x=night/day blend, yzw reserved
    };

    void render(vk::CommandBuffer cmd, int frame, vk::RenderPass renderPass, glm::vec3 tint, float brightness, float time, float nightBlend) {
      auto it = pipelines.find(renderPass);
      if(it == pipelines.end()) return;
      cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, it->second.get());

      // viewport/scissor set by caller
      std::array<vk::Buffer,2> vbs{quadVB.get(), instanceVB.get()};
      std::array<vk::DeviceSize,2> offs{0,0};
      cmd.bindVertexBuffers(0, vbs.size(), vbs.data(), offs.data());
      cmd.bindIndexBuffer(indexBuffer.get(), 0, vk::IndexType::eUint16);

      Push pc{
        glm::vec4(tint, 1.0f),
        glm::vec4(frameSize.width, frameSize.height, time, brightness),
        glm::vec4(nightBlend, 0.0f, 0.0f, 0.0f)
      };
      cmd.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push), &pc);
      cmd.drawIndexed(6, kParticles, 0, 0, 0);
    }

  private:
    [[nodiscard]] std::vector<ParticleInstance> make_instances() const
    {
      std::vector<ParticleInstance> particles(kParticles);
      std::mt19937 rng(0xC001BEEF);
      std::uniform_real_distribution<float> U(0.0f, 1.0f);
      auto random = [&] { return U(rng); };
      auto random_signed = [&] { return random() - 0.5f; };

      constexpr float fx = 1.12820041f;
      constexpr float fy = 2.00568986f;
      constexpr float pi = 3.14159265358979323846f;

      for(auto& particle : particles) {
        const bool edge = random() < 0.12f;
        glm::vec3 home{};
        glm::vec3 tint{1.0f};
        float bigScale = 1.0f;
        if(edge) {
          const bool left = random() < 0.5f;
          home.x = left ? (-5.6f - random() * 1.1f) : (5.1f + random() * 1.1f);
          home.y = random_signed() * 0.7f;
          home.z = -4.8f - random() * 1.4f;
          const auto tintChoice = random();
          if(tintChoice < 0.34f) {
            tint = {1.0f, 0.80f, 0.38f};
          } else if(tintChoice < 0.67f) {
            tint = {0.86f, 0.89f, 0.97f};
          }
          bigScale = 1.9f + random() * 1.5f;
        } else if(!particleCloud.empty()) {
          const auto& cloud = particleCloud[static_cast<std::size_t>(
              random() * static_cast<float>(particleCloud.size())) %
              particleCloud.size()];
          home.x = cloud.x + random_signed() * 1.6f;
          home.y = cloud.y * 0.45f + random_signed() * 0.16f;
          home.z = cloud.z + random_signed() * 1.4f;
        } else {
          const float ndcX = -1.08f + random() * 2.16f;
          const float ndcY = random_signed() * 0.28f - 0.05f;
          const float w = 6.8f + random_signed() * 1.2f;
          home.x = ndcX * w / fx;
          home.y = ndcY * w / fy;
          home.z = 2.0f - w;
        }

        particle.home_age = glm::vec4(home, random());
        particle.params = glm::vec4(random(), random(), edge ? 1.0f : 0.0f, bigScale);
        particle.tint_spin = glm::vec4(tint, random() * pi * 2.0f);
        particle.spin_misc = glm::vec4(
          random() * pi * 2.0f,
          (0.5f + random()) * 2.74f,
          (0.5f + random()) * 2.74f,
          0.0f);
      }
      return particles;
    }

    vk::Device device;
    vma::Allocator allocator;
    vk::Extent2D frameSize;
    double aspectRatio;
    std::vector<glm::vec3> particleCloud;

    vma::UniqueBuffer quadVB;
    vma::UniqueAllocation quadVBAlloc;
    vma::UniqueBuffer indexBuffer;
    vma::UniqueAllocation indexAlloc;
    vma::UniqueBuffer instanceVB;
    vma::UniqueAllocation instanceVBAlloc;

    vk::UniquePipelineLayout pipelineLayout;
    dreamrender::UniquePipelineMap pipelines;
};

}
