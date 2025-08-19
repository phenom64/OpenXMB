module;

#include <array>
#include <cmath>

module xmbshell.app;
import :blur_layer;

import xmbshell.render;
import dreamrender;
import glm;
import vulkan_hpp;
import vma;

namespace app {

struct BlurConstants {
    int axis = 0;
    int size = 20;
};

blur_layer::blur_layer(xmbshell* xmb) :
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

void blur_layer::render(dreamrender::gui_renderer& renderer, xmbshell* xmb) {
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
    cmd.blitImage(xmb->swapchainImages[frame], vk::ImageLayout::eTransferSrcOptimal,
        srcTexture.image, vk::ImageLayout::eTransferDstOptimal,
        vk::ImageBlit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
            {vk::Offset3D(0, 0, 0), vk::Offset3D(static_cast<int>(extent.width), static_cast<int>(extent.height), 1)},
            vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), {vk::Offset3D(0, 0, 0), vk::Offset3D(srcTexture.width, srcTexture.height, 1)}),
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

    BlurConstants constants{};
    constants.size = static_cast<int>(20); // TODO: smooth transition
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
