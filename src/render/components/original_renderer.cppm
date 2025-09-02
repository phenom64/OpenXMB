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
#include <vector>

export module openxmb.render:original_renderer;

import dreamrender;
import :shaders;

import glm;
import spdlog;
import vulkan_hpp;

namespace render {

export class original_renderer {
  public:
    original_renderer(vk::Device device, vk::Extent2D frameSize)
      : device(device), frameSize(frameSize) {}
    ~original_renderer() = default;

    void preload(const std::vector<vk::RenderPass>& renderPasses,
                 vk::SampleCountFlagBits sampleCount,
                 vk::PipelineCache pipelineCache = {})
    {
        vk::PushConstantRange range(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PushConsts));
        vk::PipelineLayoutCreateInfo layout_info({}, {}, range);
        pipelineLayout = device.createPipelineLayoutUnique(layout_info);

        vk::UniqueShaderModule vertexShader = shaders::original_bg::vert(device);
        vk::UniqueShaderModule fragmentShader = shaders::original_bg::frag(device);

        std::array<vk::PipelineShaderStageCreateInfo,2> stages = {
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, vertexShader.get(), "main"),
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fragmentShader.get(), "main")
        };

        vk::PipelineVertexInputStateCreateInfo vertex_input({},{},{});
        vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, vk::PrimitiveTopology::eTriangleList);
        vk::Viewport v{}; vk::Rect2D s{};
        vk::PipelineViewportStateCreateInfo viewport({}, v, s);
        vk::PipelineRasterizationStateCreateInfo rasterization({}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise, false, 0.0f, 0.0f, 0.0f, 1.0f);
        vk::PipelineMultisampleStateCreateInfo multisample({}, sampleCount);
        vk::PipelineDepthStencilStateCreateInfo depthStencil({}, false, false);
        vk::PipelineColorBlendAttachmentState attachment(false);
        attachment.colorWriteMask = vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend({}, false, vk::LogicOp::eClear, attachment);
        std::array<vk::DynamicState, 2> dynamicStates{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic({}, dynamicStates);

        vk::GraphicsPipelineCreateInfo pipeline_info({}, stages, &vertex_input, &input_assembly, {}, &viewport, &rasterization, &multisample, &depthStencil, &colorBlend, &dynamic, pipelineLayout.get(), {});
        pipelines = dreamrender::createPipelines(device, pipelineCache, pipeline_info, renderPasses, "Original Background Pipeline");
    }

    void prepare(int /*imageCount*/) {}

    struct PushConsts {
        glm::vec4 tint;
        glm::vec2 resolution;
        float time;
        float brightness;
    };

    void render(vk::CommandBuffer cmd, int frame, vk::RenderPass renderPass, glm::vec3 baseColor, float brightness, float time) {
        auto it = pipelines.find(renderPass);
        if(it == pipelines.end()) return;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, it->second.get());
        PushConsts pc{ glm::vec4(baseColor, 1.0f), glm::vec2(frameSize.width, frameSize.height), time, brightness };
        cmd.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(PushConsts), &pc);
        vk::Viewport viewport(0.0f, 0.0f, static_cast<float>(frameSize.width), static_cast<float>(frameSize.height), 0.0f, 1.0f);
        vk::Rect2D scissor({0,0}, frameSize);
        cmd.setViewport(0, viewport);
        cmd.setScissor(0, scissor);
        cmd.draw(3,1,0,0);
    }

  private:
    vk::Device device;
    vk::Extent2D frameSize;
    vk::UniquePipelineLayout pipelineLayout;
    dreamrender::UniquePipelineMap pipelines;
};

}
