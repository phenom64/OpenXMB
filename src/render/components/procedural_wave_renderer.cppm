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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <string_view>
#include <tuple>
#include <vector>

#include "openxmb/xmb/procedural_wave.hpp"

export module openxmb.render:procedural_wave_renderer;

import auroreengine;
import :shaders;
import glm;
import vulkan_hpp;
import vma;

namespace render {

export class procedural_wave_renderer {
public:
    procedural_wave_renderer(vk::Device device, vma::Allocator allocator, vk::Extent2D extent)
        : device_(device), allocator_(allocator), extent_(extent) {}

    void preload(const std::vector<vk::RenderPass>& renderPasses,
                 vk::SampleCountFlagBits sampleCount, vk::PipelineCache cache={}) {
        const vk::DescriptorSetLayoutBinding binding(0,vk::DescriptorType::eStorageBuffer,1,
            vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment);
        descriptorLayout_=device_.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({},binding));
        const vk::PushConstantRange range(vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment,0,sizeof(Push));
        pipelineLayout_=device_.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({},descriptorLayout_.get(),range));

        auto vertex=shaders::procedural_wave::vert(device_);
        auto fragment=shaders::procedural_wave::frag(device_);
        std::array<vk::PipelineShaderStageCreateInfo,2> stages{
            vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eVertex,vertex.get(),"main"),
            vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eFragment,fragment.get(),"main")};
        vk::PipelineVertexInputStateCreateInfo input{};
        vk::PipelineInputAssemblyStateCreateInfo assembly({},vk::PrimitiveTopology::eTriangleList);
        vk::Viewport viewport{}; vk::Rect2D scissor{};
        vk::PipelineViewportStateCreateInfo viewportState({},viewport,scissor);
        vk::PipelineRasterizationStateCreateInfo raster({},false,false,vk::PolygonMode::eFill,
            vk::CullModeFlagBits::eNone,vk::FrontFace::eCounterClockwise,false,0,0,0,1);
        vk::PipelineMultisampleStateCreateInfo samples({},sampleCount);
        vk::PipelineDepthStencilStateCreateInfo depth({},false,false);
        vk::PipelineColorBlendAttachmentState blend(true,vk::BlendFactor::eSrcAlpha,
            vk::BlendFactor::eOneMinusSrcAlpha,vk::BlendOp::eAdd,vk::BlendFactor::eOne,
            vk::BlendFactor::eOneMinusSrcAlpha,vk::BlendOp::eAdd);
        blend.colorWriteMask=vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|
            vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo color({},false,vk::LogicOp::eClear,blend);
        const std::array<vk::DynamicState,2> states{vk::DynamicState::eViewport,vk::DynamicState::eScissor};
        vk::PipelineDynamicStateCreateInfo dynamic({},states);
        vk::GraphicsPipelineCreateInfo pipeline({},stages,&input,&assembly,nullptr,&viewportState,
            &raster,&samples,&depth,&color,&dynamic,pipelineLayout_.get());
        wavePipelines_=auroreengine::createPipelines(device_,cache,pipeline,renderPasses,"Procedural Ribbon");
        blend.srcColorBlendFactor=vk::BlendFactor::eOne;
        blend.dstColorBlendFactor=vk::BlendFactor::eOne;
        color.setAttachments(blend);
        particlePipelines_=auroreengine::createPipelines(device_,cache,pipeline,renderPasses,"Procedural Sparkles");

        constexpr auto grid=openxmb::xmb::kProceduralWaveGrid;
        std::vector<std::uint16_t> indices;
        indices.reserve((grid-1)*(grid-1)*6);
        for(std::uint32_t row=0;row+1<grid;++row) for(std::uint32_t col=0;col+1<grid;++col) {
            const auto a=static_cast<std::uint16_t>(row*grid+col);
            const auto b=static_cast<std::uint16_t>(a+1),c=static_cast<std::uint16_t>(a+grid),d=static_cast<std::uint16_t>(c+1);
            indices.insert(indices.end(),{a,b,d,a,d,c});
        }
        indexCount_=static_cast<std::uint32_t>(indices.size());
        std::tie(indexBuffer_,indexAllocation_)=allocator_.createBufferUnique(
            vk::BufferCreateInfo({},indices.size()*sizeof(indices[0]),vk::BufferUsageFlagBits::eIndexBuffer),
            vma::AllocationCreateInfo({},vma::MemoryUsage::eCpuToGpu));
        allocator_.copyMemoryToAllocation(indices.data(),indexAllocation_.get(),0,indices.size()*sizeof(indices[0]));
        allocator_.flushAllocation(indexAllocation_.get(),0,indices.size()*sizeof(indices[0]));
    }

    void prepare(int imageCount) {
        frames_.clear();
        descriptorPool_.reset();
        if(imageCount<=0 || !descriptorLayout_) return;
        const auto count=static_cast<std::uint32_t>(imageCount);
        const vk::DescriptorPoolSize size(vk::DescriptorType::eStorageBuffer,count);
        descriptorPool_=device_.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({},count,size));
        const std::vector<vk::DescriptorSetLayout> layouts(count,descriptorLayout_.get());
        const auto sets=device_.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(descriptorPool_.get(),layouts));
        frames_.resize(count);
        for(std::size_t i=0;i<count;++i) {
            auto& frame=frames_[i];
            frame.set=sets[i];
            std::tie(frame.buffer,frame.allocation)=allocator_.createBufferUnique(
                vk::BufferCreateInfo({},sizeof(openxmb::xmb::ProceduralWaveFrame),vk::BufferUsageFlagBits::eStorageBuffer),
                vma::AllocationCreateInfo({},vma::MemoryUsage::eCpuToGpu));
            const vk::DescriptorBufferInfo info(frame.buffer.get(),0,sizeof(openxmb::xmb::ProceduralWaveFrame));
            const vk::WriteDescriptorSet write(frame.set,0,0,vk::DescriptorType::eStorageBuffer,{},info);
            device_.updateDescriptorSets(write,{});
        }
    }

    void finish(int) noexcept {}

    bool set_parameter(std::string_view group,std::string_view key,float value) noexcept {
        if(group=="spline") {
            bool changed=false;
            const bool accepted=waveSettings_.set(key,value,&changed);
            if(changed) sampler_.reset_history();
            return accepted;
        }
        if(group!="particle" || !std::isfinite(value)) return false;
        if(key=="count") { particleSettings_.count=static_cast<std::uint32_t>(std::clamp(value,0.0F,4096.0F)); return true; }
        if(key=="opacity") particleSettings_.opacity=std::clamp(value,0.0F,1.0F);
        else if(key=="sizeBase") particleSettings_.sizeBase=std::clamp(value,0.0F,64.0F);
        else if(key=="sizeVar") particleSettings_.sizeVar=std::clamp(value,0.0F,64.0F);
        else if(key=="flowSpeed") particleSettings_.flowSpeed=std::clamp(value,-10.0F,10.0F);
        else if(key=="posY") particleSettings_.posY=std::clamp(value,-2.0F,2.0F);
        else if(key=="spreadY") particleSettings_.spreadY=std::clamp(value,0.0F,5.0F);
        else return false;
        return true;
    }
    void reset_parameters() noexcept { waveSettings_={}; particleSettings_={}; sampler_.reset_history(); }

    void set_music_mode(bool enabled) noexcept {
        const auto now=std::chrono::steady_clock::now();
        const float elapsed=std::clamp(std::chrono::duration<float>(now-music_updated_).count(),0.0F,0.85F);
        music_updated_=now;
        if(enabled!=music_target_) {music_target_=enabled;return;}
        const float target=music_target_?1.0F:0.0F;
        music_blend_+=std::clamp(target-music_blend_,-elapsed/0.85F,elapsed/0.85F);
    }
    float music_mix() const noexcept {return music_blend_*music_blend_*(3.0F-2.0F*music_blend_);}
    bool waves_enabled{true};
    bool particles_enabled{true};
    glm::vec3 particle_color{1.0F};
    glm::vec2 motion{};

    void render(vk::CommandBuffer command,int frameIndex,vk::RenderPass renderPass,
                glm::vec3 tint,float brightness,float seconds) {
        if(frames_.empty() || frameIndex<0 || extent_.width==0 || extent_.height==0) return;
        const auto waveIt=wavePipelines_.find(renderPass),particleIt=particlePipelines_.find(renderPass);
        if(waveIt==wavePipelines_.end() || particleIt==particlePipelines_.end()) return;
        auto& frame=frames_[static_cast<std::size_t>(frameIndex)%frames_.size()];
        const float blend=music_blend_*music_blend_*(3.0F-2.0F*music_blend_);
        auto settings=waveSettings_;auto particles=particleSettings_;
        const auto toward=[blend](float value,float music){return value+(music-value)*blend;};
        settings.posY=toward(settings.posY,-0.02F);settings.tilt=toward(settings.tilt,0.12F);
        settings.thickness=toward(settings.thickness,2.8F);settings.rowSpread=toward(settings.rowSpread,1.3F);
        settings.fill=toward(settings.fill,0.6F);settings.edgeSoft=toward(settings.edgeSoft,0.8F);
        settings.bandAmplitude=toward(settings.bandAmplitude,0.07F);settings.bandFrequency=toward(settings.bandFrequency,1.0F);
        settings.opacity=toward(settings.opacity,0.4F);settings.brightness=toward(settings.brightness,0.85F);
        particles.count=static_cast<std::uint32_t>(toward(static_cast<float>(particles.count),260));
        particles.posY=toward(particles.posY,0.4F);particles.spreadY=toward(particles.spreadY,1.5F);
        particles.sizeBase=toward(particles.sizeBase,2.4F);particles.sizeVar=toward(particles.sizeVar,2.0F);particles.opacity=toward(particles.opacity,0.8F);
        sampler_.update(static_cast<double>(seconds)+36.0,settings,particles);
        const auto& data=sampler_.frame();
        allocator_.copyMemoryToAllocation(&data,frame.allocation.get(),0,sizeof(data));
        allocator_.flushAllocation(frame.allocation.get(),0,sizeof(data));
        const vk::Viewport viewport(0,0,static_cast<float>(extent_.width),static_cast<float>(extent_.height),0,1);
        const vk::Rect2D scissor({0,0},extent_);
        command.setViewport(0,viewport); command.setScissor(0,scissor);
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,pipelineLayout_.get(),0,frame.set,{});
        Push push{glm::vec4(tint,1),glm::vec4(extent_.width,extent_.height,seconds+100.0F,std::max(0.0F,brightness)),
                  glm::vec4(glm::mix(particle_color,glm::vec3(1.0F,238.0F/255,252.0F/255),blend),1),glm::vec4(0,motion.x,motion.y,waves_enabled?1.0F:0.0F)};
        constexpr auto stages=vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment;
        if(waves_enabled) {
            command.bindPipeline(vk::PipelineBindPoint::eGraphics,waveIt->second.get());
            command.pushConstants(pipelineLayout_.get(),stages,0,sizeof(push),&push);
            command.bindIndexBuffer(indexBuffer_.get(),0,vk::IndexType::eUint16);
            command.drawIndexed(indexCount_,1,0,0,0);
        }
        if(particles_enabled && particleSettings_.count>0) {
            push.mode_motion.x=1;
            command.bindPipeline(vk::PipelineBindPoint::eGraphics,particleIt->second.get());
            command.pushConstants(pipelineLayout_.get(),stages,0,sizeof(push),&push);
            command.draw(6,std::min(particles.count,4096u),0,0);
        }
    }

private:
    struct Push { glm::vec4 tint,resolution_time_gain,particle_tint,mode_motion; };
    struct Frame { vma::UniqueBuffer buffer; vma::UniqueAllocation allocation; vk::DescriptorSet set; };
    vk::Device device_;
    vma::Allocator allocator_;
    vk::Extent2D extent_;
    vk::UniqueDescriptorSetLayout descriptorLayout_;
    vk::UniquePipelineLayout pipelineLayout_;
    vk::UniqueDescriptorPool descriptorPool_;
    auroreengine::UniquePipelineMap wavePipelines_,particlePipelines_;
    std::vector<Frame> frames_;
    vma::UniqueBuffer indexBuffer_;
    vma::UniqueAllocation indexAllocation_;
    std::uint32_t indexCount_{};
    bool music_target_{};
    float music_blend_{};
    std::chrono::steady_clock::time_point music_updated_=std::chrono::steady_clock::now();
    openxmb::xmb::ProceduralWaveSettings waveSettings_;
    openxmb::xmb::ProceduralParticleSettings particleSettings_;
    openxmb::xmb::ProceduralWaveSampler sampler_;
};

} // namespace render
