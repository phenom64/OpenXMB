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

#include <chrono>
#include <cstdint>
#include <vector>

export module openxmb.render:wave_renderer;

import dreamrender;
import :shaders;

import glm;
import spdlog;
import vulkan_hpp;
import vma;

namespace render {

struct push_constants {
    glm::vec4 color;
    float time;
};

void generate_grid(int N, std::vector<glm::vec3> &vertices, std::vector<uint16_t> &indices)
{
    for(int j=0; j<=N; ++j)
    {
        for(int i=0; i<=N; ++i)
        {
            float x = 2.0f*((float)i/(float)N)-1.0f;
            float y = 2.0f*((float)j/(float)N)-1.0f;
            float z = 0.0f;
            vertices.push_back(glm::vec3(x, y, z));
        }
    }

    for(int j=0; j<N; ++j)
    {
        for(int i=0; i<N; ++i)
        {
            int row1 = j * (N+1);
            int row2 = (j+1) * (N+1);

            // triangle 1
            indices.push_back(row1+i);
            indices.push_back(row1+i+1);
            indices.push_back(row2+i+1);

            // triangle 2
            indices.push_back(row1+i);
            indices.push_back(row2+i+1);
            indices.push_back(row2+i);
        }
    }
}

const static auto startTime = std::chrono::high_resolution_clock::now();

export class wave_renderer {
    public:
        static constexpr int grid_quality = 128;
        glm::vec3 waveColor = {0.5, 0.5, 0.5};
        float speed = 1.0;

        wave_renderer(vk::Device device, vma::Allocator allocator, vk::Extent2D frameSize) : device(device), allocator(allocator), frameSize(frameSize),
            aspectRatio(static_cast<double>(frameSize.width)/frameSize.height) {}
        ~wave_renderer() = default;

        void preload(const std::vector<vk::RenderPass>& renderPasses, vk::SampleCountFlagBits sampleCount, vk::PipelineCache pipelineCache = {})
        {
            {
                std::vector<glm::vec3> vertices;
                std::vector<uint16_t> indices;
                generate_grid(grid_quality, vertices, indices);
                vertexCount = vertices.size();
                indexCount = indices.size();

                spdlog::debug("Grid made of {} vertices and {} indices", vertexCount, indexCount);

                {
                    std::tie(vertexBuffer, vertexAllocation) = allocator.createBufferUnique(
                        vk::BufferCreateInfo({}, vertices.size() * sizeof(vertices[0]), vk::BufferUsageFlagBits::eVertexBuffer),
                        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));

                    allocator.copyMemoryToAllocation(vertices.data(), vertexAllocation.get(), 0, vertices.size()*sizeof(glm::vec3));
                }
                {
                    std::tie(indexBuffer, indexAllocation) = allocator.createBufferUnique(
                        vk::BufferCreateInfo({}, indices.size() * sizeof(indices[0]), vk::BufferUsageFlagBits::eIndexBuffer),
                        vma::AllocationCreateInfo({}, vma::MemoryUsage::eCpuToGpu));

                    allocator.copyMemoryToAllocation(indices.data(), indexAllocation.get(), 0, indices.size()*sizeof(uint16_t));
                }
            }
            {
                vk::PushConstantRange range(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(push_constants));
                vk::PipelineLayoutCreateInfo layout_info({}, {}, range);
                pipelineLayout = device.createPipelineLayoutUnique(layout_info);
            }
            {
                vk::VertexInputBindingDescription input_binding(0, sizeof(glm::vec3), vk::VertexInputRate::eVertex);
                vk::VertexInputAttributeDescription input_attribute(0, 0, vk::Format::eR32G32B32Sfloat, 0);

                vk::PipelineVertexInputStateCreateInfo vertex_input({}, input_binding, input_attribute);
                vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, vk::PrimitiveTopology::eTriangleList);
                vk::PipelineTessellationStateCreateInfo tesselation({}, {});

                vk::Viewport v{};
                vk::Rect2D s{};
                vk::PipelineViewportStateCreateInfo viewport({}, v, s);

                vk::PipelineRasterizationStateCreateInfo rasterization({}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise, false, 0.0f, 0.0f, 0.0f, 1.0f);
                vk::PipelineMultisampleStateCreateInfo multisample({}, sampleCount);
                vk::PipelineDepthStencilStateCreateInfo depthStencil({}, false, false);

                vk::PipelineColorBlendAttachmentState attachment(true, vk::BlendFactor::eOne, vk::BlendFactor::eOne, vk::BlendOp::eAdd, vk::BlendFactor::eSrcAlpha, vk::BlendFactor::eOneMinusSrcAlpha, vk::BlendOp::eAdd);
                attachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
                vk::PipelineColorBlendStateCreateInfo colorBlend({}, false, vk::LogicOp::eClear, attachment);

                std::array<vk::DynamicState, 2> dynamicStates{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
                vk::PipelineDynamicStateCreateInfo dynamic({}, dynamicStates);

                {
                    vk::UniqueShaderModule vertexShader = shaders::wave_renderer::vert(device);
                    vk::UniqueShaderModule fragmentShader = shaders::wave_renderer::frag(device);
                    std::array<vk::PipelineShaderStageCreateInfo, 2> shaders = {
                        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, vertexShader.get(), "main"),
                        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fragmentShader.get(), "main")
                    };

                    vk::GraphicsPipelineCreateInfo pipeline_info({}, shaders, &vertex_input,
                        &input_assembly, &tesselation, &viewport, &rasterization, &multisample, &depthStencil, &colorBlend, &dynamic, pipelineLayout.get(), {});
                    pipelines = dreamrender::createPipelines(device, pipelineCache, pipeline_info, renderPasses, "Wave Renderer Pipeline");
                }
            }
        }
        void prepare(int imageCount) {}
        void finish(int frame) {}

        void render(vk::CommandBuffer cmd, int frame, vk::RenderPass renderPass) {
            auto time = std::chrono::high_resolution_clock::now() - startTime;
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time);
            auto partialSeconds = std::chrono::duration<float>(time-seconds);

            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipelines[renderPass].get());

            push_constants push{
                .color=glm::vec4(waveColor, 1.0),
                .time=(static_cast<float>(seconds.count()) + partialSeconds.count())*speed
            };
            cmd.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(push_constants), &push);

            cmd.bindVertexBuffers(0, vertexBuffer.get(), {0});
            cmd.bindIndexBuffer(indexBuffer.get(), 0, vk::IndexType::eUint16);
            cmd.drawIndexed(indexCount, 1, 0, 0, 0);
        }
    private:
        vk::Device device;
        vma::Allocator allocator;

        vk::Extent2D frameSize;
        double aspectRatio;

        unsigned int vertexCount{};
        vma::UniqueBuffer vertexBuffer;
        vma::UniqueAllocation vertexAllocation;

        unsigned int indexCount{};
        vma::UniqueBuffer indexBuffer;
        vma::UniqueAllocation indexAllocation;

        vk::UniquePipelineLayout pipelineLayout;
        dreamrender::UniquePipelineMap pipelines;
};

}