// GPU readback of the production trophy-notice banner renderer. Every value
// drawn here is an explicitly labelled fixture passed straight to the shared
// helper; no unlock event is injected into OpenXMB or any RPCS3 profile.
#include "../../src/app/components/trophy_notice_view.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

import auroreengine;
import glm;
import vulkan_hpp;
import vma;

namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}

constexpr std::string_view fixture_label="RPCS3 trophy earned";
constexpr std::string_view fixture_grade="Gold";
constexpr std::string_view fixture_title="Fixture Trophy";
constexpr std::string_view fixture_long_title="Fixture trophy with a deliberately long title that must be ellipsized before the banner edge";
// Fixture grade tint (sRGB-authored like the shell's gold); not a model lookup.
constexpr float fixture_gold[3]{0.90F,0.72F,0.28F};

struct Case {
    const char* name;unsigned monitor_width,monitor_height;float alpha;
    bool art_slot,art_ready;std::string_view grade,title;int same_as=-1;
};
constexpr std::array cases{
    Case{"1080p monitor, gold, art",1920,1080,1,true,true,fixture_grade,fixture_title},
    Case{"4K monitor, gold, art",3840,2160,1,true,true,fixture_grade,fixture_title},
    Case{"1080p restored",1920,1080,1,true,true,fixture_grade,fixture_title,0},
    Case{"4K half fade",3840,2160,0.5F,true,true,fixture_grade,fixture_title},
    Case{"4K fully faded",3840,2160,0,true,true,fixture_grade,fixture_title},
    Case{"1080p no art slot, no grade",1920,1080,1,false,false,{},fixture_title},
    Case{"1080p art slot, upload pending",1920,1080,1,true,false,fixture_grade,fixture_title},
    Case{"4K long title",3840,2160,1,true,true,fixture_grade,fixture_long_title},
    Case{"4K restored",3840,2160,1,true,true,fixture_grade,fixture_title,1},
};

float srgb(float linear){return linear<=0.0031308F?12.92F*linear:1.055F*std::pow(linear,1.0F/2.4F)-0.055F;}
struct Box {
    unsigned x0=~0u,y0=~0u,x1{},y1{},count{};
    void add(unsigned x,unsigned y){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);++count;}
    [[nodiscard]] float width() const {return count?static_cast<float>(x1-x0+1):0.0F;}
    [[nodiscard]] float height() const {return count?static_cast<float>(y1-y0+1):0.0F;}
};
struct Result {vk::Extent2D extent{};Box blue,white,white_top,white_bottom,gold;std::vector<unsigned char> bytes;};
bool near(float value,float expected,float tolerance){return std::abs(value-expected)<=tolerance;}
bool ratio(float large,float small,float low,float high){return small>0&&large/small>=low&&large/small<=high;}

class notice_phase final:public auroreengine::phase {
public:
    notice_phase(auroreengine::window* window,const std::string& font):phase(window),
        icon(device,allocator,16,16),
        image(device,{595,97},window->gpuFeatures),
        text(font,32,device,allocator,{595,97},window->gpuFeatures),
        shape(device,allocator,{595,97},window->gpuFeatures){image.setAllocator(allocator);}
    ~notice_phase()override{try{win->waitIdle();}catch(...) {}}
    void preload()override{
        phase::preload();
        // The passive host's colour target: cleared transparent, single sample.
        const vk::AttachmentDescription color({},vk::Format::eB8G8R8A8Srgb,vk::SampleCountFlagBits::e1,vk::AttachmentLoadOp::eClear,vk::AttachmentStoreOp::eStore,vk::AttachmentLoadOp::eDontCare,vk::AttachmentStoreOp::eDontCare,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferSrcOptimal);
        const vk::AttachmentReference ref(0,vk::ImageLayout::eColorAttachmentOptimal);
        const vk::SubpassDescription subpass({},vk::PipelineBindPoint::eGraphics,{},ref);
        const std::array dependencies{
            vk::SubpassDependency(vk::SubpassExternal,0,vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eColorAttachmentOutput,{},vk::AccessFlagBits::eColorAttachmentWrite),
            vk::SubpassDependency(0,vk::SubpassExternal,vk::PipelineStageFlagBits::eColorAttachmentOutput,vk::PipelineStageFlagBits::eTransfer,vk::AccessFlagBits::eColorAttachmentWrite,vk::AccessFlagBits::eTransferRead)};
        pass=device.createRenderPassUnique(vk::RenderPassCreateInfo({},color,subpass,dependencies));
        image.preload({pass.get()},vk::SampleCountFlagBits::e1);shape.preload({pass.get()},vk::SampleCountFlagBits::e1);
        // Mirrors the shell's resident atlas range; fixture strings are preloaded
        // so first-frame glyph staging timing is outside this test.
        text.set_preload_texts({std::string(fixture_label),std::string(fixture_grade),std::string(fixture_title),std::string(fixture_long_title),"\xE2\x80\xA6"});
        add_task(text.preload(loader,{pass.get()},vk::SampleCountFlagBits::e1,{},nullptr,0x20,0x1ff));
        // Fixture trophy icon: opaque pure blue, distinct from text and grade ink.
        add_task(loader->loadTexture(&icon,auroreengine::LoaderFunction{[](std::uint8_t* output,std::size_t size){
            require(size>=16*16*4,"icon upload capacity");
            for(std::size_t at=0;at<16*16*4;at+=4){output[at]=0;output[at+1]=0;output[at+2]=255;output[at+3]=255;}}}));
    }
    void prepare(std::vector<vk::Image> images,std::vector<vk::ImageView> views)override{
        phase::prepare(images,views);window_images=std::move(images);slots=static_cast<unsigned>(window_images.size());
        install(0);
    }
    void init()override{
        win->waitIdle();
        require(icon.loaded,"fixture icon did not upload");
        atlas=text.get_atlas();require(atlas&&atlas->loaded,"resident font atlas did not load");atlas_image=atlas->image;
    }
    void after_events()override{
        if(completed>=cases.size()||completed==configured)return;
        win->waitIdle();install(completed);
    }
    void render(int frame,vk::Semaphore available,vk::Semaphore finished,vk::Fence fence)override{
        const auto& fixture=cases.at(completed);
        auto command=commandBuffers.at(frame);command.begin(vk::CommandBufferBeginInfo());
        require(text.get_atlas()==atlas&&atlas->image==atlas_image,"target resize discarded the resident font atlas");
        text.stage_glyphs(command);
        const vk::ClearValue clear(vk::ClearColorValue(std::array<float,4>{0,0,0,0}));
        command.beginRenderPass(vk::RenderPassBeginInfo(pass.get(),framebuffer.get(),vk::Rect2D({0,0},extent),clear),vk::SubpassContents::eInline);
        // As the shell sets before render_gui.
        command.setViewport(0,vk::Viewport(0,0,static_cast<float>(extent.width),static_cast<float>(extent.height),0,1));command.setScissor(0,vk::Rect2D({0,0},extent));
        {
            auroreengine::gui_renderer gui(command,frame,pass.get(),extent,&text,&image,&shape);
            const app::trophy_notice_view::Notice<auroreengine::texture,glm::vec4> notice{fixture_label,fixture.grade,
                glm::vec4(fixture_gold[0],fixture_gold[1],fixture_gold[2],fixture.alpha),fixture.title,fixture.art_slot,fixture.art_ready?&icon:nullptr};
            // The passive host's call: its framebuffer is exactly the banner.
            app::trophy_notice_view::draw(gui,notice,fixture.alpha,0,0,static_cast<float>(extent.width),static_cast<float>(extent.height),scale);
        }
        command.endRenderPass();image.finish(frame);text.finish(frame);shape.finish(frame);
        const vk::BufferImageCopy copy(0,0,0,vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor,0,0,1),{},vk::Extent3D(extent.width,extent.height,1));
        command.copyImageToBuffer(target->image,vk::ImageLayout::eTransferSrcOptimal,readback.get(),copy);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{},{},vk::BufferMemoryBarrier(vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead,vk::QueueFamilyIgnored,vk::QueueFamilyIgnored,readback.get(),0,vk::WholeSize),{});
        const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor,0,1,0,1);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,vk::PipelineStageFlagBits::eTransfer,{},{},{},vk::ImageMemoryBarrier({},vk::AccessFlagBits::eTransferWrite,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,vk::QueueFamilyIgnored,vk::QueueFamilyIgnored,window_images.at(frame),range));
        command.clearColorImage(window_images.at(frame),vk::ImageLayout::eTransferDstOptimal,vk::ClearColorValue(std::array<float,4>{0,0,0,0}),range);
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eBottomOfPipe,{},{},{},vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite,{},vk::ImageLayout::eTransferDstOptimal,win->swapchainFinalLayout,vk::QueueFamilyIgnored,vk::QueueFamilyIgnored,window_images.at(frame),range));
        command.end();const vk::PipelineStageFlags wait=vk::PipelineStageFlagBits::eTransfer;submit_graphics(vk::SubmitInfo(1,&available,&wait,1,&command,1,&finished),fence);
        // The frame's readback and every texture it sampled stay alive until
        // this fence; targets are replaced only after a device idle.
        require(device.waitForFences(fence,true,10000000000ull)==vk::Result::eSuccess,"render/readback timed out");
        allocator.invalidateAllocation(readback_memory.get(),0,vk::WholeSize);
        check(fixture);++completed;
    }
    unsigned completed{};
private:
    void install(unsigned index){
        const auto bounds=openxmb::xmb::trophy_notice_bounds(0,0,cases.at(index).monitor_width,cases.at(index).monitor_height);
        require(bounds.has_value(),"fixture monitor has no notice bounds");
        const vk::Extent2D size{bounds->width,bounds->height};scale=bounds->scale;configured=index;
        if(target&&size==extent)return;
        mapping.reset();framebuffer.reset();target.reset();readback.reset();readback_memory.reset();extent=size;
        target=std::make_unique<auroreengine::texture>(device,allocator,extent,vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc,vk::Format::eB8G8R8A8Srgb,vk::SampleCountFlagBits::e1,false);
        framebuffer=device.createFramebufferUnique(vk::FramebufferCreateInfo({},pass.get(),target->imageView.get(),extent.width,extent.height,1));
        std::tie(readback,readback_memory)=allocator.createBufferUnique(vk::BufferCreateInfo({},static_cast<vk::DeviceSize>(extent.width)*extent.height*4,vk::BufferUsageFlagBits::eTransferDst),vma::AllocationCreateInfo({},vma::MemoryUsage::eGpuToCpu));
        mapping=std::make_unique<vma::MemoryMapping>(allocator,readback_memory.get());
        // Same retained renderers as the shell's swapchain recreation.
        const auto cache=text.textCacheStatistics();
        text.set_target_extent(extent);image.set_target_extent(extent);shape.set_target_extent(extent);
        text.prepare(static_cast<int>(slots));image.prepare(static_cast<int>(slots));shape.prepare(static_cast<int>(slots));
        const auto retained=text.textCacheStatistics();
        require(cache.entries==retained.entries&&cache.glyphs==retained.glyphs,"target resize discarded the shaped-text cache");
    }
    void check(const Case& fixture){
        const auto name=std::string(fixture.name)+": ";
        const auto* data=static_cast<const unsigned char*>(mapping->get());
        Result result;result.extent=extent;result.bytes.assign(data,data+static_cast<std::size_t>(extent.width)*extent.height*4);
        const float w=static_cast<float>(extent.width),h=static_cast<float>(extent.height);
        const float pad=h*0.12F,icon_size=static_cast<float>(std::lround(h-2.0F*pad));
        const float text_x=fixture.art_slot?2.0F*pad+(h-2.0F*pad):w*(0.015F/openxmb::xmb::trophy_notice_frame.width);
        const bool art=fixture.art_slot&&fixture.art_ready;
        if(fixture.alpha<=0.0F){
            require(std::ranges::all_of(result.bytes,[](unsigned char b){return b==0;}),name+"a fully faded notice drew pixels");
            report(fixture,result);results.push_back(std::move(result));return;
        }
        const int panel_alpha=static_cast<int>(std::lround(0.8F*fixture.alpha*255.0F));
        const float peak=srgb(fixture.alpha)*255.0F;
        for(unsigned y=0;y<extent.height;++y)for(unsigned x=0;x<extent.width;++x){
            const auto* p=data+(static_cast<std::size_t>(y)*extent.width+x)*4;
            const int b=p[0],g=p[1],r=p[2],a=p[3];
            const auto at=" at "+std::to_string(x)+","+std::to_string(y)+" rgba("+std::to_string(r)+","+std::to_string(g)+","+std::to_string(b)+","+std::to_string(a)+")";
            require(a>=panel_alpha-2,name+"banner pixel is not covered by the black panel"+at);
            require(std::max({r,g,b})<=static_cast<int>(peak)+4,name+"pixel brighter than the notice opacity allows"+at);
            const int high=std::max({r,g,b}),low=std::min({r,g,b});
            if(high-low<=16){
                if(low>=0.8F*peak){result.white.add(x,y);(y<h*0.5F?result.white_top:result.white_bottom).add(x,y);}
                continue;
            }
            if(b>r+16&&b>g+16&&r<=16&&g<=16){
                require(art&&x+2>=pad&&x<=pad+icon_size+2&&y+2>=pad&&y<=pad+icon_size+2,name+"blue outside the fixture icon slot"+at);
                if(b>=0.8F*peak)result.blue.add(x,y);
                continue;
            }
            if(r>=g&&g>=b&&r>b+16){
                require(!fixture.grade.empty()&&y<h*0.5F&&x>text_x,name+"grade-coloured pixel outside the label row"+at);
                if(r>=0.7F*peak&&b<0.75F*r&&g>0.6F*r)result.gold.add(x,y);
                continue;
            }
            throw std::runtime_error(name+"unexpected colour ("+std::to_string(r)+","+std::to_string(g)+","+std::to_string(b)+") outside banner content"+at+": extra drawing");
        }
        // Pure panel: top/bottom rows and the right margin stay black at 0.8 opacity.
        const auto panel=[&](unsigned x,unsigned y){const auto* p=data+(static_cast<std::size_t>(y)*extent.width+x)*4;return p[0]==0&&p[1]==0&&p[2]==0&&std::abs(p[3]-panel_alpha)<=1;};
        for(unsigned x=0;x<extent.width;++x)require(panel(x,0)&&panel(x,extent.height-1),name+"panel edge row is not the black panel");
        for(unsigned y=0;y<extent.height;++y)require(panel(extent.width-1,y),name+"panel right edge is not the black panel");
        require(result.white_top.count>20*scale*scale&&result.white_bottom.count>20*scale*scale,name+"label or title text produced no glyph pixels");
        require(result.white.x0+1.5F>=text_x&&result.white.x0<=text_x+4.0F*scale,name+"text does not start at the expected column "+std::to_string(text_x)+" (ink "+std::to_string(result.white.x0)+")");
        require(result.white_bottom.x1<=w-pad+1.0F,name+"title overran the banner padding");
        if(fixture.title==fixture_long_title)require(result.white_bottom.x1>=0.85F*(w-pad),name+"long fixture title was not laid out to the padded edge");
        else require(result.white_bottom.x1<0.6F*w,name+"short fixture title is wider than expected");
        if(art){
            require(result.blue.count>0,name+"fixture icon produced no pixels");
            require(near(static_cast<float>(result.blue.x0),std::round(pad),1.5F)&&near(static_cast<float>(result.blue.y0),std::round(pad),1.5F),name+"icon origin is not the padded corner");
            require(near(result.blue.width(),icon_size,2.0F)&&near(result.blue.height(),icon_size,2.0F),name+"icon size "+std::to_string(result.blue.width())+" is not "+std::to_string(icon_size));
        }else require(result.blue.count==0,name+"icon drawn without a ready texture");
        if(fixture.grade.empty())require(result.gold.count==0,name+"grade ink drawn without a grade");
        else require(result.gold.count>10*scale*scale&&result.gold.x0>result.white_top.x1&&result.gold.y1<h*0.5F,name+"grade ink missing or not right of the label");
        report(fixture,result);
        if(fixture.same_as>=0){
            const auto& previous=results.at(static_cast<std::size_t>(fixture.same_as));
            require(previous.extent==extent&&previous.bytes==result.bytes,name+"restored target did not reproduce the earlier banner exactly");
        }
        if(&fixture==&cases[1]){
            const auto& base=results.at(0);
            require(ratio(result.white.width(),base.white.width(),1.85F,2.15F)&&ratio(result.white.height(),base.white.height(),1.8F,2.2F),name+"text did not scale with the 4K banner");
            require(ratio(result.blue.width(),base.blue.width(),1.9F,2.1F),name+"icon did not scale with the 4K banner");
            require(ratio(result.gold.width(),base.gold.width(),1.8F,2.2F),name+"grade did not scale with the 4K banner");
            require(ratio(static_cast<float>(result.white.count),static_cast<float>(base.white.count),3.0F,5.0F),name+"text ink area did not scale with the 4K banner");
        }
        results.push_back(std::move(result));
    }
    void report(const Case& fixture,const Result& result) const {
        std::cout<<fixture.name<<": "<<extent.width<<'x'<<extent.height<<" scale="<<scale<<" alpha="<<fixture.alpha
            <<" text="<<result.white.count<<" ["<<result.white.x0<<'-'<<result.white.x1<<"] icon="<<result.blue.width()<<'x'<<result.blue.height()
            <<" grade="<<result.gold.count<<" passed\n";
    }
    auroreengine::texture icon;auroreengine::image_renderer image;auroreengine::font_renderer text;auroreengine::simple_renderer shape;
    vk::UniqueRenderPass pass;std::unique_ptr<auroreengine::texture> target;vk::UniqueFramebuffer framebuffer;
    vma::UniqueBuffer readback;vma::UniqueAllocation readback_memory;std::unique_ptr<vma::MemoryMapping> mapping;
    std::vector<vk::Image> window_images;vk::Extent2D extent{};float scale{1};unsigned slots{},configured{};
    const auroreengine::texture* atlas{};vk::Image atlas_image{};std::vector<Result> results;
};
}

int main(int argc,char** argv)try{
    require(argc==2,"Supply the packaged Play-Regular.ttf path");
    auroreengine::window_config config;config.name="openxmb-trophy-notice-render";config.headless=true;config.width=32;config.height=32;
    config.headless_frames=cases.size();config.headless_output_dir.clear();config.sampleCount=vk::SampleCountFlagBits::e1;
    auroreengine::window window{config};window.init();auto* phase=new notice_phase(&window,argv[1]);window.set_phase(phase);window.loop();window.waitIdle();
    require(phase->completed==cases.size(),"fixture did not render every banner case");
    std::cout<<"Trophy notice banner GPU render passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
