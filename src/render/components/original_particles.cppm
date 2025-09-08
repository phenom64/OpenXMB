/* This file is a part of the OpenXMB desktop experience project.
 * Copyright (C) 2025 Syndromatic Ltd. All rights reserved
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
#include <cstdint>
#include <random>
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
    static constexpr uint32_t kParticles = 768; // tunable; keep modest for mobile GPUs

    particles_renderer(vk::Device device, vma::Allocator allocator, vk::Extent2D frameSize)
      : device(device), allocator(allocator), frameSize(frameSize), aspectRatio(static_cast<double>(frameSize.width)/frameSize.height) {}
    ~particles_renderer() = default;

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

      std::tie(indexBuffer, indexAlloc) = allocator.createBufferUnique(
        vk::BufferCreateInfo({}, idx.size()*sizeof(idx[0]), vk::BufferUsageFlagBits::eIndexBuffer),
        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
      allocator.copyMemoryToAllocation(idx.data(), indexAlloc.get(), 0, idx.size()*sizeof(idx[0]));

      // Instance buffer: per-particle 2D seeds in [0,1)
      std::vector<glm::vec2> seeds(kParticles);
      std::mt19937 rng(0xC001BEEF);
      std::uniform_real_distribution<float> U(0.0f, 1.0f);
      for(auto& s : seeds) s = glm::vec2(U(rng), U(rng));
      std::tie(instanceVB, instanceVBAlloc) = allocator.createBufferUnique(
        vk::BufferCreateInfo({}, seeds.size()*sizeof(seeds[0]), vk::BufferUsageFlagBits::eVertexBuffer),
        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));
      allocator.copyMemoryToAllocation(seeds.data(), instanceVBAlloc.get(), 0, seeds.size()*sizeof(seeds[0]));

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
        vk::VertexInputBindingDescription(1, sizeof(glm::vec2), vk::VertexInputRate::eInstance)  // seed
      };
      std::array<vk::VertexInputAttributeDescription,2> attrs = {
        vk::VertexInputAttributeDescription(0, 0, vk::Format::eR32G32Sfloat, 0), // inPos
        vk::VertexInputAttributeDescription(1, 1, vk::Format::eR32G32Sfloat, 0)  // inSeed
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
      glm::vec4 tint;        // base tint
      glm::vec2 resolution;  // width,height
      float time;            // seconds
      float brightness;      // 0..1 scales sprite alpha/size
    };

    void render(vk::CommandBuffer cmd, int frame, vk::RenderPass renderPass, glm::vec3 tint, float brightness, float time) {
      auto it = pipelines.find(renderPass);
      if(it == pipelines.end()) return;
      cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, it->second.get());

      // viewport/scissor set by caller
      std::array<vk::Buffer,2> vbs{quadVB.get(), instanceVB.get()};
      std::array<vk::DeviceSize,2> offs{0,0};
      cmd.bindVertexBuffers(0, vbs.size(), vbs.data(), offs.data());
      cmd.bindIndexBuffer(indexBuffer.get(), 0, vk::IndexType::eUint16);

      Push pc{ glm::vec4(tint, 1.0f), glm::vec2(frameSize.width, frameSize.height), time, brightness };
      cmd.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push), &pc);
      cmd.drawIndexed(6, kParticles, 0, 0, 0);
    }

  private:
    vk::Device device;
    vma::Allocator allocator;
    vk::Extent2D frameSize;
    double aspectRatio;

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
