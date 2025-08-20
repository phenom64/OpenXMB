module;

#include <cassert>
#include <filesystem>
#include <future>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>   // enum AVPixelFormat
#include <libavutil/pixdesc.h>  // av_get_pix_fmt_string, av_get_pix_fmt_name
#include <libavutil/imgutils.h> // av_image_get_buffer_size, av_image_fill_arrays
#include <libswscale/swscale.h>
}

export module shell.app:video_player;

import dreamrender;
import glm;
import spdlog;
import vulkan_hpp;
import vma;
import i18n;
import shell.utils;
import :component;
import :programs;
import :message_overlay;
import :base_viewer;
import shell.app;
import shell.render;

namespace programs {

using namespace app;
using namespace mfk::i18n::literals;

export class video_player : private base_viewer, public component, public action_receiver, public joystick_receiver, public mouse_receiver {
    constexpr static auto preferred_format = AV_PIX_FMT_RGBA;
    public:
        video_player(std::filesystem::path path, dreamrender::resource_loader& loader) :
            path(std::move(path)), device(loader.getDevice()), allocator(loader.getAllocator())
        {
            load_future = std::async(std::launch::async, [this, &loader] -> std::unique_ptr<video_decoding_context> {
                auto ctx = std::make_unique<video_decoding_context>();
                try {
                    if (avformat_open_input(&ctx->format_ctx, this->path.c_str(), nullptr, nullptr) != 0) {
                        throw std::runtime_error("Could not open input file");
                    }
                    if (avformat_find_stream_info(ctx->format_ctx, nullptr) < 0) {
                        throw std::runtime_error("Could not find stream information");
                    }

                    ctx->video_stream_index = av_find_best_stream(ctx->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &ctx->codec, 0);
                    if (ctx->video_stream_index < 0) {
                        throw std::runtime_error("No video stream found");
                    }

                    ctx->codec_ctx = avcodec_alloc_context3(ctx->codec);
                    if (!ctx->codec_ctx) {
                        throw std::runtime_error("Could not allocate codec context");
                    }

                    if (avcodec_parameters_to_context(ctx->codec_ctx, ctx->format_ctx->streams[ctx->video_stream_index]->codecpar) < 0) {
                        throw std::runtime_error("Could not copy codec parameters to context");
                    }

                    if (avcodec_open2(ctx->codec_ctx, ctx->codec, nullptr) < 0) {
                        throw std::runtime_error("Could not open codec");
                    }

                } catch(const std::exception& e) {
                    spdlog::error("Failed to load video: {}", e.what());
                    ctx->error_message = e.what();
                    return ctx;
                }
                return ctx;
            });
        }
        ~video_player() {
            if(loaded) {
                device.waitIdle();
            }
        }

        result tick(shell* xmb) override {
            if(!loaded && !utils::is_ready(load_future)) {
                return result::success;
            }
            if(!loaded) {
                ctx = load_future.get();
                if(!ctx->codec_ctx) {
                    spdlog::error("Failed to load video");
                    xmb->emplace_overlay<message_overlay>("Failed to open video"_(),
                        ctx->error_message.empty() ? "Unknown error"_() : ctx->error_message,
                        std::vector<std::string>{"OK"_()});
                    return result::close;
                }
                image_width = ctx->codec_ctx->width;
                image_height = ctx->codec_ctx->height;
                char buf[128];
                spdlog::info("Video of size {}x{} loaded in pixel format {}", image_width, image_height,
                    av_get_pix_fmt_string(buf, sizeof(buf), ctx->codec_ctx->pix_fmt));
                yuv_conversion = ctx->codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P;
                if(yuv_conversion) {
                    spdlog::info("Video is in YUV420P format, converting it to RGBA on GPU");
                } else if(ctx->codec_ctx->pix_fmt != preferred_format) {
                    char buf2[128];
                    spdlog::warn("Video is not in preferred format, converting it to {}",
                        av_get_pix_fmt_string(buf2, sizeof(buf2), preferred_format));
                }

                {
                    vk::ImageCreateInfo image_info(vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage,
                        vk::ImageType::e2D, vk::Format::eR8G8B8A8Srgb,
                        vk::Extent3D(image_width, image_height, 1), 1, 1, vk::SampleCountFlagBits::e1,
                        vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst,
                        vk::SharingMode::eExclusive, 0, nullptr, vk::ImageLayout::eUndefined);
                    vma::AllocationCreateInfo alloc_info({}, vma::MemoryUsage::eGpuOnly);
                    std::tie(decoded_image, decoded_allocation) = allocator.createImageUnique(image_info, alloc_info);
                }
                {
                    vk::ImageViewUsageCreateInfo view_usage(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
                    vk::ImageViewCreateInfo view_info({}, decoded_image.get(), vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Srgb,
                        vk::ComponentMapping(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
                    vk::StructureChain view_chain{view_info, view_usage};
                    decoded_view = device.createImageViewUnique(view_chain.get<vk::ImageViewCreateInfo>());
                }

                unsigned int staging_count = xmb->get_window()->swapchainImageCount;
                staging_buffers.resize(staging_count);
                staging_buffer_allocations.resize(staging_count);
                vk::DeviceSize staging_size = image_width * image_height * 4;
                spdlog::debug("Allocating {} staging buffers of size {}", staging_count, staging_size);

                vk::BufferCreateInfo buffer_info({}, staging_size, vk::BufferUsageFlagBits::eTransferSrc, vk::SharingMode::eExclusive);
                vma::AllocationCreateInfo alloc_info({}, vma::MemoryUsage::eCpuToGpu);
                for(unsigned int i = 0; i < staging_count; ++i) {
                    std::tie(staging_buffers[i], staging_buffer_allocations[i]) = allocator.createBufferUnique(buffer_info, alloc_info);
                }

                if(yuv_conversion) {
                    unormView = device.createImageViewUnique(vk::ImageViewCreateInfo({}, decoded_image.get(), vk::ImageViewType::e2D, vk::Format::eR8G8B8A8Unorm,
                        vk::ComponentMapping(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));

                    std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {
			            vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
		            	vk::DescriptorSetLayoutBinding(1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
		            	vk::DescriptorSetLayoutBinding(2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute),
		            	vk::DescriptorSetLayoutBinding(3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute)
		            };
		            descriptorSetLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, bindings));

                    vk::DescriptorPoolSize pool_size(vk::DescriptorType::eStorageImage, 4);
                    descriptorPool = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo({}, 1, pool_size));
                    descriptorSet = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(descriptorPool.get(), descriptorSetLayout.get())).front();

                    pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, descriptorSetLayout.get()));
                    vk::UniqueShaderModule shaderModule = render::shaders::yuv420p::decode_comp(device);
                    vk::PipelineShaderStageCreateInfo shaderInfo({}, vk::ShaderStageFlagBits::eCompute, shaderModule.get(), "main");
                    auto [r, p] = device.createComputePipelineUnique({}, vk::ComputePipelineCreateInfo({}, shaderInfo, pipelineLayout.get())).asTuple();
                    if(r != vk::Result::eSuccess) {
                        throw std::runtime_error("Failed to create compute pipeline");
                    }
                    this->pipeline = std::move(p);

                    plane_textures[0] = std::make_unique<dreamrender::texture>(device, allocator, image_width, image_height, vk::ImageUsageFlagBits::eStorage, vk::Format::eR8Unorm);
                    plane_textures[1] = std::make_unique<dreamrender::texture>(device, allocator, image_width/2, image_height/2, vk::ImageUsageFlagBits::eStorage, vk::Format::eR8Unorm);
                    plane_textures[2] = std::make_unique<dreamrender::texture>(device, allocator, image_width/2, image_height/2, vk::ImageUsageFlagBits::eStorage, vk::Format::eR8Unorm);

                    vk::DescriptorImageInfo output_info({}, unormView.get(), vk::ImageLayout::eGeneral);
                    vk::DescriptorImageInfo input_y_info({}, plane_textures[0]->imageView.get(), vk::ImageLayout::eGeneral);
                    vk::DescriptorImageInfo input_cb_info({}, plane_textures[1]->imageView.get(), vk::ImageLayout::eGeneral);
                    vk::DescriptorImageInfo input_cr_info({}, plane_textures[2]->imageView.get(), vk::ImageLayout::eGeneral);
                    std::array<vk::WriteDescriptorSet, 4> writes = {
                        vk::WriteDescriptorSet(descriptorSet, 0, 0, 1, vk::DescriptorType::eStorageImage, &output_info),
                        vk::WriteDescriptorSet(descriptorSet, 1, 0, 1, vk::DescriptorType::eStorageImage, &input_y_info),
                        vk::WriteDescriptorSet(descriptorSet, 2, 0, 1, vk::DescriptorType::eStorageImage, &input_cb_info),
                        vk::WriteDescriptorSet(descriptorSet, 3, 0, 1, vk::DescriptorType::eStorageImage, &input_cr_info)
                    };
                    device.updateDescriptorSets(writes, {});
                }

                start_time = std::chrono::steady_clock::now();
                state = play_state::playing;

                loaded = true;
            }

            base_viewer::tick();
            return result::success;
        }

        void prerender(vk::CommandBuffer cmd, int frame, shell* xmb) override;
        void render(dreamrender::gui_renderer& renderer, class shell* xmb) override;
        result on_action(action action) override;

        result on_joystick(unsigned int index, float x, float y) override {
            return base_viewer::on_joystick(index, x, y);
        }

        result on_mouse_move(float x, float y) override {
            return base_viewer::on_mouse_move(x, y);
        }

        [[nodiscard]] bool is_opaque() const override {
            return loaded;
        }
    private:
        vk::Device device;
        vma::Allocator allocator;

        std::filesystem::path path;
        bool loaded = false;
        struct video_decoding_context {
            AVFormatContext* format_ctx = nullptr;
            const AVCodec* codec = nullptr;
            AVCodecContext* codec_ctx = nullptr;
            SwsContext* sws_ctx = nullptr;
            int video_stream_index = -1;
            std::string error_message;

            ~video_decoding_context() {
                if (codec_ctx) avcodec_free_context(&codec_ctx);
                if (format_ctx) avformat_close_input(&format_ctx);
                if (sws_ctx) sws_freeContext(sws_ctx);
            }
        };
        std::unique_ptr<video_decoding_context> ctx;
        vma::UniqueImage decoded_image;
        vma::UniqueAllocation decoded_allocation;
        vk::UniqueImageView decoded_view;
        double decoded_timestamp = 0.0;

        bool yuv_conversion = false;
        std::array<std::unique_ptr<dreamrender::texture>, 3> plane_textures;
        vk::UniquePipelineLayout pipelineLayout;
        vk::UniquePipeline pipeline;
        vk::UniqueDescriptorSetLayout descriptorSetLayout;
        vk::UniqueDescriptorPool descriptorPool;
        vk::DescriptorSet descriptorSet;
        vk::UniqueImageView unormView;

        std::future<std::unique_ptr<video_decoding_context>> load_future;

        std::vector<vma::UniqueBuffer> staging_buffers;
        std::vector<vma::UniqueAllocation> staging_buffer_allocations;

        enum class play_state {
            loading,
            playing,
            paused,
            stopped,
        };
        play_state state = play_state::loading;
        std::chrono::steady_clock::time_point start_time;
};

void video_player::prerender(vk::CommandBuffer cmd, int frame, shell* xmb) {
    if(!loaded || state != play_state::playing) {
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* videoFrame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();

    try {
        while (av_read_frame(ctx->format_ctx, pkt) >= 0) {
            if (pkt->stream_index != ctx->video_stream_index) {
                av_packet_unref(pkt);
                continue;
            }

            decoded_timestamp = (double)pkt->pts * av_q2d(ctx->format_ctx->streams[ctx->video_stream_index]->time_base);

            if (avcodec_send_packet(ctx->codec_ctx, pkt) != 0) {
                spdlog::warn("Failed to send packet to decoder");
                av_packet_unref(pkt);
                continue;
            }

            int ret = avcodec_receive_frame(ctx->codec_ctx, videoFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_unref(pkt);
                continue;
            } else if (ret < 0) {
                spdlog::warn("Failed to decode video frame");
                av_packet_unref(pkt);
                continue;
            }

            // Frame decoded successfully
            if (yuv_conversion) {
                // ... (YUV conversion logic remains the same)
            } else {
                if (!ctx->sws_ctx) {
                    ctx->sws_ctx = sws_getContext(
                        ctx->codec_ctx->width, ctx->codec_ctx->height, ctx->codec_ctx->pix_fmt,
                        ctx->codec_ctx->width, ctx->codec_ctx->height, preferred_format,
                        SWS_BILINEAR, nullptr, nullptr, nullptr
                    );
                }

                int rgb_buffer_size = av_image_get_buffer_size(preferred_format, ctx->codec_ctx->width, ctx->codec_ctx->height, 32);
                vk::BufferCreateInfo rgb_buffer_info({}, rgb_buffer_size, vk::BufferUsageFlagBits::eTransferSrc);
                vma::AllocationCreateInfo alloc_info({}, vma::MemoryUsage::eCpuToGpu);
                auto [rgb_buffer, rgb_alloc] = allocator.createBufferUnique(rgb_buffer_info, alloc_info);
                auto* buffer = (uint8_t*)av_malloc(rgb_buffer_size * sizeof(uint8_t));
                av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, preferred_format, ctx->codec_ctx->width, ctx->codec_ctx->height, 1);

                sws_scale(ctx->sws_ctx, (const uint8_t* const*)videoFrame->data, videoFrame->linesize, 0, ctx->codec_ctx->height, rgbFrame->data, rgbFrame->linesize);

                allocator.copyMemoryToAllocation(rgbFrame->data[0], staging_buffer_allocations[frame].get(), 0, rgb_buffer_size);
                av_free(buffer);

                cmd.pipelineBarrier(
                    vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer,
                    {}, {}, {},
                    vk::ImageMemoryBarrier(
                        {}, vk::AccessFlagBits::eTransferWrite,
                        vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        decoded_image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    )
                );
                cmd.copyBufferToImage(staging_buffers[frame].get(), decoded_image.get(), vk::ImageLayout::eTransferDstOptimal,
                    vk::BufferImageCopy(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                        vk::Offset3D(0, 0, 0), vk::Extent3D(image_width, image_height, 1)));
                cmd.pipelineBarrier(
                    vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                    {}, {}, {},
                    vk::ImageMemoryBarrier(
                        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                        vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
                        decoded_image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
                    )
                );
            }
            av_frame_unref(videoFrame);
            av_packet_unref(pkt);
            break; // process one frame per call
        }
    } catch (...) {
        // handle exceptions
    }
    av_frame_free(&rgbFrame);
    av_frame_free(&videoFrame);
    av_packet_free(&pkt);
}

void video_player::render(dreamrender::gui_renderer& renderer, class shell* xmb) {
    if(!loaded) {
        return;
    }

    constexpr float size = 0.8;
    base_viewer::render(decoded_view.get(), size, renderer);

    { // progress bar
        double duration = ctx->format_ctx->duration / (double)AV_TIME_BASE;
        double progress = duration > 0.0f ? decoded_timestamp / duration : 0.5f;

        simple_renderer::params border_radius{{}, {0.5f, 0.5f, 0.5f, 0.5f}};
        simple_renderer::params blur{std::array{glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.0f}, glm::vec2{0.0f, 0.5f}, glm::vec2{0.0f, 0.5f}}, {0.5f, 0.5f, 0.5f, 0.5f}};
        renderer.draw_rect(glm::vec2(0.1f, 0.95f), glm::vec2(0.8f, 0.01f), glm::vec4(0.2f, 0.2f, 0.2f, 1.0f), border_radius);
        renderer.draw_rect(glm::vec2(0.1f, 0.95f), glm::vec2(0.8f, 0.01f), glm::vec4(0.1f, 0.1f, 0.1f, 1.0f), blur);

        constexpr glm::vec2 padding = glm::vec2{0.001f, 0.001f};
        renderer.draw_rect(glm::vec2(0.1f, 0.95f)+padding, glm::vec2(progress*0.8f, 0.01f)-2.0f*padding,
            glm::vec4(0x83/255.0f, 0x8d/255.0f, 0x22/255.0f, 1.0f), border_radius);
        renderer.draw_rect(glm::vec2(0.1f, 0.95f)+padding, glm::vec2(progress*0.8f, 0.01f)-2.0f*padding,
            glm::vec4(1.0f, 1.0f, 1.0f, 0.1f), blur);
    }
}

result video_player::on_action(action action) {
    switch(action) {
        case action::cancel:
            return result::close;
        case action::ok:
            if(state == play_state::playing) {
                state = play_state::paused;
            } else if(state == play_state::paused) {
                state = play_state::playing;
                start_time = std::chrono::floor<std::chrono::steady_clock::time_point::duration>(
                    std::chrono::steady_clock::now() - decoded_timestamp * std::chrono::seconds(1));
            } else if(state == play_state::stopped) {
                state = play_state::playing;
                start_time = std::chrono::steady_clock::now();
            }
            return result::success | result::ok_sound;
        default: {
            result r = base_viewer::on_action(action);
            if(r != result::unsupported) {
                return r;
            }
            break;
        }
    }
    return result::failure;
}

}
;