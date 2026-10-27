#pragma once
// Private implementation shared by the module and native Vulkan tests.
// The includer supplies standard headers, video_color.hpp and Vulkan/VMA types.
namespace render {
class video_texture {
public:
  struct statistics {
    std::uint64_t gpu_frames{},rgba_frames{},cpu_fallback_frames{},uploaded_bytes{};
    double upload_record_ms{};
  };
  video_texture(vk::Device device,vma::Allocator allocator,bool native_supported,
      std::function<vk::UniqueShaderModule()> shader,std::function<void()> wait)
    :device_(device),allocator_(allocator),native_supported_(native_supported),shader_(std::move(shader)),wait_(std::move(wait)) {}
  ~video_texture() {try {reset();}catch(...) {}}
  video_texture(const video_texture&)=delete;
  video_texture& operator=(const video_texture&)=delete;
  static bool supports_native(vk::PhysicalDevice physical,std::uint32_t family) {
    const auto queues=physical.getQueueFamilyProperties();
    if(family>=queues.size()||!(queues[family].queueFlags&vk::QueueFlagBits::eCompute)) return false;
    const auto limits=physical.getProperties().limits;
    if(limits.maxComputeWorkGroupInvocations<256||limits.maxComputeWorkGroupSize[0]<16||limits.maxComputeWorkGroupSize[1]<16) return false;
    for(const auto format:{vk::Format::eR8Unorm,vk::Format::eR8G8Unorm})
      if(!(physical.getFormatProperties(format).optimalTilingFeatures&vk::FormatFeatureFlagBits::eSampledImage)) return false;
    const auto output=physical.getFormatProperties(vk::Format::eR8G8B8A8Unorm).optimalTilingFeatures;
    return bool(output&vk::FormatFeatureFlagBits::eStorageImage)&&bool(physical.getFormatProperties(vk::Format::eR8G8B8A8Srgb).optimalTilingFeatures&vk::FormatFeatureFlagBits::eSampledImage);
  }
  bool upload(vk::CommandBuffer command,int index,unsigned count,const openxmb::video::frame& picture) {
    if(index<0||static_cast<unsigned>(index)>=count||count==0) return false;
    const auto began=std::chrono::steady_clock::now();
    const auto packing=video_detail::layout(picture);
    const bool native=picture.format!=openxmb::video::pixel_format::rgba;
    bool gpu=native&&native_supported_;
    const auto wanted=gpu?picture.format:openxmb::video::pixel_format::rgba;
    if(!output_||width_!=picture.width||height_!=picture.height||format_!=wanted||staging_.size()!=count) {
      reset();
      try {allocate(picture.width,picture.height,wanted,count,packing.size);}
      catch(const std::exception&) {
        if(!gpu) throw;
        native_supported_=false;gpu=false;reset();
        allocate(picture.width,picture.height,openxmb::video::pixel_format::rgba,count,static_cast<std::size_t>(picture.width)*picture.height*4);
      }
    }
    auto& staging=staging_[index];
    if(!gpu) {
      const auto* data=picture.rgba.data();const auto size=static_cast<std::size_t>(picture.width)*picture.height*4;
      std::vector<std::uint8_t> fallback;
      if(native) {fallback=video_detail::rgba_fallback(picture);data=fallback.data();++stats_.cpu_fallback_frames;}
      allocator_.copyMemoryToAllocation(data,staging.allocation.get(),0,size);
      transition(command,*output_,initialized_?vk::ImageLayout::eShaderReadOnlyOptimal:vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,
        initialized_?vk::AccessFlags(vk::AccessFlagBits::eShaderRead):vk::AccessFlags{},vk::AccessFlagBits::eTransferWrite,
        initialized_?vk::PipelineStageFlagBits::eFragmentShader:vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer);
      copy(command,staging.buffer.get(),*output_,0,width_,height_);
      transition(command,*output_,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eShaderRead,vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eFragmentShader);
      ++stats_.rgba_frames;stats_.uploaded_bytes+=size;
    } else {
      for(int i=0;i<packing.count;++i) {
        const auto& plane=picture.planes[i];
        if(static_cast<std::size_t>(plane.stride)==packing.row_bytes[i])
          allocator_.copyMemoryToAllocation(plane.bytes.data(),staging.allocation.get(),packing.offsets[i],packing.row_bytes[i]*packing.heights[i]);
        else for(int row=0;row<packing.heights[i];++row)
          allocator_.copyMemoryToAllocation(plane.bytes.data()+static_cast<std::size_t>(row)*plane.stride,staging.allocation.get(),packing.offsets[i]+row*packing.row_bytes[i],packing.row_bytes[i]);
        transition(command,*planes_[i],initialized_?vk::ImageLayout::eShaderReadOnlyOptimal:vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,
          initialized_?vk::AccessFlags(vk::AccessFlagBits::eShaderRead):vk::AccessFlags{},vk::AccessFlagBits::eTransferWrite,
          initialized_?vk::PipelineStageFlagBits::eComputeShader:vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer);
        copy(command,staging.buffer.get(),*planes_[i],packing.offsets[i],plane.width,plane.height);
        transition(command,*planes_[i],vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eShaderReadOnlyOptimal,
          vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eShaderRead,vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eComputeShader);
        stats_.uploaded_bytes+=packing.row_bytes[i]*packing.heights[i];
      }
      transition(command,*output_,initialized_?vk::ImageLayout::eShaderReadOnlyOptimal:vk::ImageLayout::eUndefined,vk::ImageLayout::eGeneral,
        initialized_?vk::AccessFlags(vk::AccessFlagBits::eShaderRead):vk::AccessFlags{},vk::AccessFlagBits::eShaderWrite,
        initialized_?vk::PipelineStageFlagBits::eFragmentShader:vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eComputeShader);
      command.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline_.get());
      command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipeline_layout_.get(),0,descriptor_,{});
      const auto parameters=video_detail::parameters(picture);
      command.pushConstants(pipeline_layout_.get(),vk::ShaderStageFlagBits::eCompute,0,sizeof(parameters),&parameters);
      command.dispatch((width_+15)/16,(height_+15)/16,1);
      transition(command,*output_,vk::ImageLayout::eGeneral,vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::AccessFlagBits::eShaderWrite,vk::AccessFlagBits::eShaderRead,vk::PipelineStageFlagBits::eComputeShader,vk::PipelineStageFlagBits::eFragmentShader);
      ++stats_.gpu_frames;
    }
    initialized_=true;
    stats_.upload_record_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    return true;
  }
  void reset() {
    if(output_||!staging_.empty()) wait_();
    descriptor_pool_.reset();descriptor_=nullptr;sampling_view_.reset();
    for(auto& plane:planes_) plane.reset();output_.reset();staging_.clear();initialized_=false;
  }
  vk::ImageView view() const {return sampling_view_.get();}
  vk::Image image() const {return output_?output_->image.get():vk::Image{};}
  int width() const {return width_;}
  int height() const {return height_;}
  bool ready() const {return initialized_;}
  bool gpu_conversion() const {return initialized_&&format_!=openxmb::video::pixel_format::rgba;}
  const statistics& stats() const {return stats_;}
  bool native_enabled() const {return native_supported_;}
private:
  struct image_resource {vma::UniqueAllocation allocation;vma::UniqueImage image;vk::UniqueImageView view;};
  struct staging_resource {vma::UniqueAllocation allocation;vma::UniqueBuffer buffer;};
  vk::Device device_;vma::Allocator allocator_;
  bool native_supported_,initialized_{};
  std::function<vk::UniqueShaderModule()> shader_;
  std::function<void()> wait_;
  vk::UniqueDescriptorSetLayout descriptor_layout_;
  vk::UniquePipelineLayout pipeline_layout_;
  vk::UniquePipeline pipeline_;
  vk::UniqueSampler sampler_;
  std::unique_ptr<image_resource> output_;
  std::array<std::unique_ptr<image_resource>,3> planes_;
  vk::UniqueImageView sampling_view_;
  vk::UniqueDescriptorPool descriptor_pool_;
  vk::DescriptorSet descriptor_{};
  std::vector<staging_resource> staging_;
  int width_{},height_{};
  openxmb::video::pixel_format format_{openxmb::video::pixel_format::rgba};
  statistics stats_{};
  std::unique_ptr<image_resource> make_image(int width,int height,vk::Format format,vk::ImageUsageFlags usage,bool mutable_format=false) {
    auto image=std::make_unique<image_resource>();
    vk::ImageCreateInfo info(mutable_format?vk::ImageCreateFlags(vk::ImageCreateFlagBits::eMutableFormat):vk::ImageCreateFlags{},vk::ImageType::e2D,format,
      vk::Extent3D(width,height,1),1,1,vk::SampleCountFlagBits::e1,vk::ImageTiling::eOptimal,usage,vk::SharingMode::eExclusive);
    std::tie(image->image,image->allocation)=allocator_.createImageUnique(info,vma::AllocationCreateInfo({},vma::MemoryUsage::eGpuOnly));
    image->view=device_.createImageViewUnique(vk::ImageViewCreateInfo({},image->image.get(),vk::ImageViewType::e2D,format,{},vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)));
    return image;
  }
  void material() {
    if(pipeline_) return;
    std::array<vk::DescriptorSetLayoutBinding,4> bindings;
    for(unsigned i=0;i<4;++i) bindings[i]=vk::DescriptorSetLayoutBinding(i,i?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,1,vk::ShaderStageFlagBits::eCompute);
    descriptor_layout_=device_.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({},bindings));
    const vk::PushConstantRange push(vk::ShaderStageFlagBits::eCompute,0,sizeof(video_detail::conversion));
    pipeline_layout_=device_.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({},descriptor_layout_.get(),push));
    auto shader=shader_();
    pipeline_=device_.createComputePipelineUnique({},vk::ComputePipelineCreateInfo({},vk::PipelineShaderStageCreateInfo({},vk::ShaderStageFlagBits::eCompute,shader.get(),"main"),pipeline_layout_.get())).value;
    sampler_=device_.createSamplerUnique(vk::SamplerCreateInfo({},vk::Filter::eNearest,vk::Filter::eNearest,vk::SamplerMipmapMode::eNearest,vk::SamplerAddressMode::eClampToEdge,vk::SamplerAddressMode::eClampToEdge,vk::SamplerAddressMode::eClampToEdge));
  }
  void allocate(int width,int height,openxmb::video::pixel_format format,unsigned count,std::size_t bytes) {
    using openxmb::video::pixel_format;
    const bool native=format!=pixel_format::rgba;
    width_=width;height_=height;format_=format;
    auto usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc;
    if(native) usage|=vk::ImageUsageFlagBits::eStorage;
    output_=make_image(width,height,vk::Format::eR8G8B8A8Unorm,usage,true);
    vk::ImageViewUsageCreateInfo sampled_usage(vk::ImageUsageFlagBits::eSampled);
    vk::ImageViewCreateInfo sampled_info({},output_->image.get(),vk::ImageViewType::e2D,vk::Format::eR8G8B8A8Srgb,{},vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1));
    sampled_info.pNext=&sampled_usage;
    sampling_view_=device_.createImageViewUnique(sampled_info);
    if(native) {
      material();
      for(int i=0;i<(format==pixel_format::nv12?2:3);++i) planes_[i]=make_image(i?(width+1)/2:width,i?(height+1)/2:height,
        i==1&&format==pixel_format::nv12?vk::Format::eR8G8Unorm:vk::Format::eR8Unorm,vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst);
      const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage,1),vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler,3)};
      descriptor_pool_=device_.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({},1,sizes));
      descriptor_=device_.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(descriptor_pool_.get(),descriptor_layout_.get()))[0];
      const std::array images{vk::DescriptorImageInfo({},output_->view.get(),vk::ImageLayout::eGeneral),
        vk::DescriptorImageInfo(sampler_.get(),planes_[0]->view.get(),vk::ImageLayout::eShaderReadOnlyOptimal),
        vk::DescriptorImageInfo(sampler_.get(),planes_[1]->view.get(),vk::ImageLayout::eShaderReadOnlyOptimal),
        vk::DescriptorImageInfo(sampler_.get(),planes_[format==pixel_format::nv12?1:2]->view.get(),vk::ImageLayout::eShaderReadOnlyOptimal)};
      std::array<vk::WriteDescriptorSet,4> writes;
      for(unsigned i=0;i<4;++i) writes[i]=vk::WriteDescriptorSet(descriptor_,i,0,1,i?vk::DescriptorType::eCombinedImageSampler:vk::DescriptorType::eStorageImage,&images[i]);
      device_.updateDescriptorSets(writes,{});
    }
    const auto capacity=native?bytes:static_cast<std::size_t>(width)*height*4;
    staging_.resize(count);
    for(auto& staging:staging_) std::tie(staging.buffer,staging.allocation)=allocator_.createBufferUnique(
      vk::BufferCreateInfo({},capacity,vk::BufferUsageFlagBits::eTransferSrc,vk::SharingMode::eExclusive),vma::AllocationCreateInfo({},vma::MemoryUsage::eCpuToGpu));
  }
  static void transition(vk::CommandBuffer command,const image_resource& image,vk::ImageLayout old_layout,vk::ImageLayout next,
      vk::AccessFlags source,vk::AccessFlags destination,vk::PipelineStageFlags before,vk::PipelineStageFlags after) {
    command.pipelineBarrier(before,after,{},{},{},vk::ImageMemoryBarrier(source,destination,old_layout,next,vk::QueueFamilyIgnored,vk::QueueFamilyIgnored,
      image.image.get(),vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,0,1,0,1)));
  }
  static void copy(vk::CommandBuffer command,vk::Buffer buffer,const image_resource& image,std::size_t offset,int width,int height) {
    command.copyBufferToImage(buffer,image.image.get(),vk::ImageLayout::eTransferDstOptimal,
      vk::BufferImageCopy(offset,0,0,vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),vk::Offset3D{},vk::Extent3D(width,height,1)));
  }
};
}
