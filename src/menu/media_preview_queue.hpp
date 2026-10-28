#pragma once
#include "media_preview_decode.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace openxmb::menu_preview {
enum class State { idle, queued, ready, failed };
struct Request {
    std::filesystem::path path;
    Kind kind;
    std::atomic<State> state{State::idle};
    photos::Bitmap image;
};

// One CPU decoder per open folder; only visible entries enqueue work. Weak
// requests discard closed entries without retaining their textures or menus.
class Queue {
public:
    using Decoder = std::function<photos::Bitmap(const std::filesystem::path&,Kind,int)>;
    explicit Queue(Decoder decoder=decode, std::size_t capacity=24)
        : decoder_(std::move(decoder)),capacity_(capacity),worker_([this]{run();}) {}
    ~Queue() {
        { std::lock_guard lock(mutex_); stopping_=true; }
        changed_.notify_all(); worker_.join();
    }
    bool enqueue(const std::shared_ptr<Request>& request) {
        std::lock_guard lock(mutex_);
        std::erase_if(pending_,[](const auto& weak){return weak.expired();});
        if(stopping_ || pending_.size()>=capacity_) return false;
        auto expected=State::idle;
        if(!request->state.compare_exchange_strong(expected,State::queued)) return false;
        pending_.push_back(request); changed_.notify_one(); return true;
    }
private:
    void run() {
        for(;;) {
            std::shared_ptr<Request> request;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,[this]{return stopping_ || !pending_.empty();});
                if(stopping_) return;
                request=pending_.front().lock();pending_.pop_front();
            }
            if(!request) continue;
            try {
                request->image=decoder_(request->path,request->kind,256);
                const auto& image=request->image;
                const bool valid=image.width>0 && image.height>0 && image.width<=256 && image.height<=256 &&
                    image.rgba.size()==static_cast<std::size_t>(image.width)*image.height*4;
                request->state.store(valid?State::ready:State::failed,std::memory_order_release);
            } catch(...) {request->state.store(State::failed,std::memory_order_release);}
        }
    }
    Decoder decoder_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::weak_ptr<Request>> pending_;
    bool stopping_{};
    std::thread worker_;
};
}
