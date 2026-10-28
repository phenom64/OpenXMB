#include "../../src/menu/media_preview_queue.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
#include <chrono>
using namespace openxmb::menu_preview;
using namespace std::chrono_literals;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
void complete(const std::shared_ptr<Request>& request) {
    const auto deadline=std::chrono::steady_clock::now()+3s;
    while(request->state.load(std::memory_order_acquire)==State::queued && std::chrono::steady_clock::now()<deadline)
        std::this_thread::yield();
    require(request->state.load()!=State::queued,"decoder completion timed out");
}
int main() try {
    std::promise<void> entered,release;
    auto released=release.get_future().share();
    std::atomic<int> calls{};
    Queue queue([&](const auto& path,Kind,int edge) {
        require(edge==256,"bounded decoder edge");
        if(calls.fetch_add(1)==0) {entered.set_value();released.wait();}
        if(path=="bad")throw std::runtime_error("bad image");
        return openxmb::photos::Bitmap{2,1,{255,0,0,255,0,0,255,255}};
    },1);
    auto request=[](const char* name) {auto value=std::make_shared<Request>();value->path=name;value->kind=Kind::photo;return value;};
    auto first=request("first"),cancelled=request("cancelled"),third=request("third"),bad=request("bad");
    require(queue.enqueue(first),"first request enqueued");
    require(entered.get_future().wait_for(3s)==std::future_status::ready,"worker entered");
    require(queue.enqueue(cancelled),"one queued request accepted while decoder stalls");
    require(!queue.enqueue(third)&&third->state.load()==State::idle,"full queue is nonblocking and retryable");
    cancelled.reset();
    require(queue.enqueue(third),"expired menu request releases capacity");
    require(!queue.enqueue(third),"duplicate request not decoded twice");
    release.set_value();complete(first);complete(third);
    require(calls==2,"closed entry not decoded");
    require(first->state.load()==State::ready&&first->image.rgba[0]==255&&first->image.rgba[6]==255,"release/acquire publishes actual pixels");
    require(queue.enqueue(bad),"bad image accepted");complete(bad);
    require(bad->state.load()==State::failed,"decode exception leaves fallback state");
    std::cout<<"Preview queue passed: bounded/nonblocking requests, cancellation, retry, pixel publication, decode failure\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
