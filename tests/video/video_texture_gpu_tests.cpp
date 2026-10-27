#define VMA_IMPLEMENTATION
#include <vulkan/vulkan.hpp>
namespace vk {using detail::resultCheck;using detail::createResultValueType;}
#include <vk_mem_alloc.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include "../../src/render/video_color.hpp"
#include "../../src/render/video_texture_impl.hpp"

void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::atomic<unsigned> validation_errors{};
VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* message,void*) {
  if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
  std::cerr<<message->pMessage<<'\n';return VK_FALSE;
}
struct validation_scope {
  VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};
  ~validation_scope(){if(messenger) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);}
};
namespace video=openxmb::video;
video::frame fixture(int width,int height,bool nv12,video::color_matrix matrix,bool full) {
  video::frame frame;frame.width=width;frame.height=height;frame.matrix=matrix;frame.full_range=full;
  frame.format=nv12?video::pixel_format::nv12:video::pixel_format::yuv420p;
  for(int i=0;i<(nv12?2:3);++i) {
    auto& plane=frame.planes[i];plane.width=i?(width+1)/2:width;plane.height=i?(height+1)/2:height;
    plane.stride=plane.width*(nv12&&i==1?2:1)+3;
    plane.bytes.resize(static_cast<std::size_t>(plane.stride)*plane.height,0xee);
    for(int y=0;y<plane.height;++y)for(int x=0;x<plane.width;++x) {
      const auto offset=static_cast<std::size_t>(y)*plane.stride+x*(nv12&&i==1?2:1);
      if(!i)plane.bytes[offset]=static_cast<std::uint8_t>(16+(x*17+y*11)%220);
      else {plane.bytes[offset]=static_cast<std::uint8_t>((i==1?76:194)+(x+y)%20);if(nv12)plane.bytes[offset+1]=static_cast<std::uint8_t>(194+(x+y)%20);}
    }
  }
  return frame;
}
int main(int argc,char** argv) {
  try {
    check(argc==2,"Usage: video_texture_gpu_tests shader.spv");
    std::ifstream file(argv[1],std::ios::binary|std::ios::ate);check(bool(file),"Cannot read shader");
    const auto bytes=static_cast<std::size_t>(file.tellg());std::vector<std::uint32_t> code(bytes/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);
    const vk::ApplicationInfo app("OpenXMB video texture test",1,"OpenXMB",1,VK_API_VERSION_1_1);
    std::vector<const char*> layers;
    for(const auto& layer:vk::enumerateInstanceLayerProperties())if(std::string_view(layer.layerName.data())=="VK_LAYER_KHRONOS_validation")layers.push_back("VK_LAYER_KHRONOS_validation");
    const std::array extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkValidationFeatureEnableEXT synchronize=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation_features.enabledValidationFeatureCount=1;validation_features.pEnabledValidationFeatures=&synchronize;
    vk::InstanceCreateInfo instance_info({},&app,layers,extensions);
    if(!layers.empty()) instance_info.pNext=&validation_features;
    auto instance=vk::createInstanceUnique(instance_info);
    validation_scope messages;messages.instance=instance.get();
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=validation;
    const auto create_debug=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance.get(),"vkCreateDebugUtilsMessengerEXT"));
    check(create_debug&&create_debug(instance.get(),&debug,nullptr,&messages.messenger)==VK_SUCCESS,"Cannot enable validation messages");
    vk::PhysicalDevice physical{};std::uint32_t family{};
    for(const auto candidate:instance->enumeratePhysicalDevices()) {
      const auto families=candidate.getQueueFamilyProperties();
      for(unsigned i=0;i<families.size();++i)if((families[i].queueFlags&(vk::QueueFlagBits::eGraphics|vk::QueueFlagBits::eCompute))==(vk::QueueFlagBits::eGraphics|vk::QueueFlagBits::eCompute)) {physical=candidate;family=i;break;}
      if(physical)break;
    }
    check(bool(physical),"No graphics/compute Vulkan device");
    const float priority=1;const vk::DeviceQueueCreateInfo queue_info({},family,1,&priority);
    auto device=physical.createDeviceUnique(vk::DeviceCreateInfo({},queue_info));
    const auto queue=device->getQueue(family,0);
    vma::AllocatorCreateInfo alloc_info({},physical,device.get());alloc_info.instance=instance.get();alloc_info.vulkanApiVersion=VK_API_VERSION_1_1;
    auto allocator=vma::createAllocatorUnique(alloc_info);
    auto pool=device->createCommandPoolUnique(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,family));
    auto buffers=device->allocateCommandBuffers(vk::CommandBufferAllocateInfo(pool.get(),vk::CommandBufferLevel::ePrimary,1));const auto command=buffers[0];
    std::cout<<"GPU: "<<physical.getProperties().deviceName<<"; validation layer: "<<(!layers.empty())<<'\n';
    const auto shader=[&]{return device->createShaderModuleUnique(vk::ShaderModuleCreateInfo({},bytes,code.data()));};
    const auto wait=[&]{device->waitIdle();};
    const bool supported=render::video_texture::supports_native(physical,family);check(supported,"Device cannot exercise the GPU-native path");
    render::video_texture texture(device.get(),allocator.get(),supported,shader,wait);
    render::video_texture fallback(device.get(),allocator.get(),false,shader,wait);
    int cases=0;
    const auto run=[&](render::video_texture& upload,const video::frame& frame) {
      const auto size=static_cast<std::size_t>(frame.width)*frame.height*4;
      auto [read_buffer,read_allocation]=allocator->createBufferUnique(vk::BufferCreateInfo({},size,vk::BufferUsageFlagBits::eTransferDst,vk::SharingMode::eExclusive),vma::AllocationCreateInfo({},vma::MemoryUsage::eGpuToCpu));
      command.reset();command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
      check(upload.upload(command,cases%3,3,frame),"Upload rejected frame");check(bool(upload.view()),"Missing retained sampling view");
      const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor,0,1,0,1);
      command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,vk::PipelineStageFlagBits::eTransfer,{},{},{},
        vk::ImageMemoryBarrier(vk::AccessFlagBits::eMemoryWrite|vk::AccessFlagBits::eMemoryRead,vk::AccessFlagBits::eTransferRead,vk::ImageLayout::eShaderReadOnlyOptimal,vk::ImageLayout::eTransferSrcOptimal,vk::QueueFamilyIgnored,vk::QueueFamilyIgnored,upload.image(),range));
      command.copyImageToBuffer(upload.image(),vk::ImageLayout::eTransferSrcOptimal,read_buffer.get(),vk::BufferImageCopy(0,0,0,vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),vk::Offset3D{},vk::Extent3D(frame.width,frame.height,1)));
      command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eFragmentShader,{},{},{},vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferRead,vk::AccessFlagBits::eShaderRead,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eShaderReadOnlyOptimal,vk::QueueFamilyIgnored,vk::QueueFamilyIgnored,upload.image(),range));
      command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},vk::MemoryBarrier(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead),{},{});
      command.end();queue.submit(vk::SubmitInfo({}, {}, command));queue.waitIdle();
      const auto* actual=static_cast<const std::uint8_t*>(allocator->mapMemory(read_allocation.get()));allocator->invalidateAllocation(read_allocation.get(),0,size);
      const auto expected=render::video_detail::rgba_fallback(frame);
      for(std::size_t i=0;i<size;++i) if(std::abs(int(actual[i])-int(expected[i]))>1)throw std::runtime_error("GPU conversion differs at byte "+std::to_string(i)+": "+std::to_string(actual[i])+" versus "+std::to_string(expected[i]));
      allocator->unmapMemory(read_allocation.get());++cases;
    };
    for(auto matrix:{video::color_matrix::bt601,video::color_matrix::bt709,video::color_matrix::bt2020})for(bool full:{false,true})for(bool nv12:{false,true}) {
      auto frame=fixture(17,19,nv12,matrix,full);run(texture,frame);run(texture,frame);run(fallback,frame);
    }
    auto small=fixture(5,3,false,video::color_matrix::bt709,false);run(texture,small);
    auto rgba=small;rgba.format=video::pixel_format::rgba;rgba.rgba=render::video_detail::rgba_fallback(small);run(texture,rgba);
    run(texture,fixture(7,5,true,video::color_matrix::bt2020,true));
    check(texture.stats().gpu_frames==26&&texture.stats().rgba_frames==1,"GPU/fallback path statistics wrong");
    check(fallback.stats().cpu_fallback_frames==12,"CPU native fallback did not run");
    // Three submitted frames share plane/output images but retain separate
    // staging slots. Synchronization validation checks the producer hazards.
    const auto asynchronous=device->allocateCommandBuffers(vk::CommandBufferAllocateInfo(pool.get(),vk::CommandBufferLevel::ePrimary,3));
    const auto concurrent=fixture(7,5,true,video::color_matrix::bt2020,true);
    for(int i=0;i<3;++i) {
      asynchronous[i].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
      check(texture.upload(asynchronous[i],i,3,concurrent),"In-flight slot upload failed");
      asynchronous[i].end();queue.submit(vk::SubmitInfo({}, {}, asynchronous[i]));++cases;
    }
    // Reconfiguration must wait for all three submissions before retiring views.
    const auto large=fixture(1920,1080,false,video::color_matrix::bt709,false);
    auto before=texture.stats().uploaded_bytes;run(texture,large);
    const auto native_bytes=texture.stats().uploaded_bytes-before;
    auto large_rgba=large;large_rgba.rgba=render::video_detail::rgba_fallback(large);large_rgba.format=video::pixel_format::rgba;
    before=texture.stats().uploaded_bytes;run(texture,large_rgba);
    const auto rgba_bytes=texture.stats().uploaded_bytes-before;
    check(native_bytes==3110400&&rgba_bytes==8294400,"1080p upload byte reduction wrong");
    std::cout<<"1080p transfer bytes: native="<<native_bytes<<" RGBA="<<rgba_bytes<<" (62.5% fewer bytes)\n";
    std::cout<<"video_texture_gpu_tests: "<<cases<<" real GPU upload/readback cases passed (601/709/2020, full/limited, planar/NV12, odd/padded planes, reuse/reconfigure, RGBA and CPU fallback)\n";
    std::cout<<"native uploads="<<texture.stats().uploaded_bytes<<" bytes; record="<<texture.stats().upload_record_ms<<"ms\n";
    texture.reset();fallback.reset();check(validation_errors==0,"Vulkan validation reported errors");
    std::cout<<"Vulkan validation errors: "<<validation_errors<<'\n';return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
