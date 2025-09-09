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
#include <cmath>
#include <chrono>
#include <algorithm>

module openxmb.app;
import :blur_layer;
import openxmb.render;
import dreamrender;
import glm;
import vulkan_hpp;
import vma;

namespace app {

struct BlurConstants {
    int axis = 0;
    int size = 20;
};

blur_layer::blur_layer(shell* xmb) :
    srcTexture(xmb->device, xmb->allocator, xmb->win->swapchainExtent,
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
        vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor),
    dstTexture(xmb->device, xmb->allocator, xmb->win->swapchainExtent,
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc,
        vk::Format::eR16G16B16A16Sfloat, vk::SampleCountFlagBits::e1, false, vk::ImageAspectFlagBits::eColor)
{
    auto device = xmb->device;
    {
        std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
            vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
            vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
        };
        vk::DescriptorSetLayoutCreateInfo info({}, bindings);
        descriptorSetLayout = device.createDescriptorSetLayoutUnique(info);
    }
    {
        vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants)};
        vk::PipelineLayoutCreateInfo info({}, descriptorSetLayout.get(), range);
        pipelineLayout = device.createPipelineLayoutUnique(info);
    }
    {
        vk::UniqueShaderModule compShader = render::shaders::blur::comp(device);
        vk::PipelineShaderStageCreateInfo shader({}, vk::ShaderStageFlagBits::eCompute, compShader.get(), "main");
        vk::ComputePipelineCreateInfo info({}, shader, pipelineLayout.get());
        pipeline = device.createComputePipelineUnique({}, info).value;
        debugName(device, pipeline.get(), "Blur Pipeline");
    }
    {
        vk::DescriptorPoolSize size(vk::DescriptorType::eStorageImage, 2);
        vk::DescriptorPoolCreateInfo pool_info({}, 1, size);
        descriptorPool = device.createDescriptorPoolUnique(pool_info);

        vk::DescriptorSetAllocateInfo alloc_info(descriptorPool.get(), 1, &descriptorSetLayout.get());
        descriptorSet = device.allocateDescriptorSets(alloc_info)[0];
    }
    {
        vk::DescriptorImageInfo srcInfo({}, srcTexture.imageView.get(), vk::ImageLayout::eGeneral);
        vk::DescriptorImageInfo dstInfo({}, dstTexture.imageView.get(), vk::ImageLayout::eGeneral);
        std::array<vk::WriteDescriptorSet, 2> writes = {
            vk::WriteDescriptorSet(descriptorSet, 0, 0, 1, vk::DescriptorType::eStorageImage, &srcInfo),
            vk::WriteDescriptorSet(descriptorSet, 1, 0, 1, vk::DescriptorType::eStorageImage, &dstInfo),
        };
        device.updateDescriptorSets(writes, {});
    }
}

void blur_layer::render(dreamrender::gui_renderer& renderer, shell* xmb) {
    auto cmd = renderer.get_command_buffer();
    int frame = renderer.get_frame();
    auto extent = xmb->win->swapchainExtent;

    cmd.endRenderPass();

    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer, {},
        {}, {}, {
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead,
                xmb->win->swapchainFinalLayout, vk::ImageLayout::eTransferSrcOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                xmb->swapchainImages[frame], vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
            vk::ImageMemoryBarrier(
                {}, vk::AccessFlagBits::eTransferWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                srcTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
        });
    // Blit allows format conversion and matches usage (swapchain -> R16G16B16A16)
    cmd.blitImage(xmb->swapchainImages[frame], vk::ImageLayout::eTransferSrcOptimal,
        srcTexture.image, vk::ImageLayout::eTransferDstOptimal,
        vk::ImageBlit(
            vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
            {vk::Offset3D(0, 0, 0), vk::Offset3D(static_cast<int>(extent.width), static_cast<int>(extent.height), 1)},
            vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
            {vk::Offset3D(0, 0, 0), vk::Offset3D(srcTexture.width, srcTexture.height, 1)}
        ),
        vk::Filter::eLinear);

    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
        {}, {}, {}, {
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                srcTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
            vk::ImageMemoryBarrier(
                {}, vk::AccessFlagBits::eShaderWrite,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                dstTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
        });

    int groupCountX = static_cast<int>(std::ceil(srcTexture.width/16.0));
    int groupCountY = static_cast<int>(std::ceil(srcTexture.height/16.0));

    // Smoothly animate blur radius when toggling blur_background
    BlurConstants constants{};
    {
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        auto elapsed = now - xmb->last_blur_background_change;
        double dur_sec = std::chrono::duration<double>(shell::blur_background_transition_duration).count();
        double t_sec = std::chrono::duration<double>(elapsed).count();
        double p = std::clamp(t_sec / std::max(dur_sec, 1e-6), 0.0, 1.0);
        // Ramp up when enabling blur, ramp down when disabling
        double strength = xmb->blur_background ? p : (1.0 - p);
        // Base radius scaled to resolution for consistent look across displays
        double base_px_1080 = 20.0; // radius at 1080p
        double scale = static_cast<double>(extent.height) / 1080.0;
        int radius = static_cast<int>(std::round(base_px_1080 * scale * strength));
        // Clamp to a sane maximum to avoid excessive work on large displays
        constants.size = std::clamp(radius, 0, 64);
    }
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.get());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout.get(), 0, {descriptorSet}, {});

    constants.axis = 0;
    cmd.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
    cmd.dispatch(groupCountX, groupCountY, 1);

    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
        {}, {}, {}, {
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eTransferWrite,
                vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                srcTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
                vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                dstTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            )
        });

    cmd.copyImage(dstTexture.image, vk::ImageLayout::eTransferSrcOptimal,
        srcTexture.image, vk::ImageLayout::eTransferDstOptimal, {
            vk::ImageCopy(
                vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                vk::Offset3D(0, 0, 0),
                vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                vk::Offset3D(0, 0, 0),
                vk::Extent3D(extent, 1)
            )
        });

    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
        {}, {}, {}, {
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                srcTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderWrite,
                vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                dstTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
        });

    constants.axis = 1;
    cmd.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eCompute, 0, sizeof(BlurConstants), &constants);
    cmd.dispatch(groupCountX, groupCountY, 1);

    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eFragmentShader,
        {}, {}, {}, {
            vk::ImageMemoryBarrier(
                vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eShaderRead,
                vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                dstTexture.image, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
            ),
        });

    vk::ClearValue color(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    cmd.beginRenderPass(vk::RenderPassBeginInfo(xmb->shellRenderPass.get(), xmb->framebuffers[frame].get(),
        vk::Rect2D({0, 0}, extent), color), vk::SubpassContents::eInline);
    vk::Viewport viewport(0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height), 0.0f, 1.0f);
    vk::Rect2D scissor({0,0}, extent);
    cmd.setViewport(0, viewport);
    cmd.setScissor(0, scissor);

    renderer.draw_image_sized(dstTexture.imageView.get(), 0.0f, 0.0f,
        static_cast<int>(extent.width), static_cast<int>(extent.height));
}

}
