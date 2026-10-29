#pragma once
#include <chrono>
#include <future>
#include <stop_token>
#include <thread>
#include <utility>

namespace openxmb::desktop {
// Value workers never retain their UI owner. A promise-backed future can be
// discarded without joining a network request on the render thread.
template<class T> class AsyncJob {
  std::stop_source cancellation_;
  std::future<T> result_;
public:
  AsyncJob()=default;
  ~AsyncJob() {cancel();}
  AsyncJob(const AsyncJob&)=delete;
  AsyncJob& operator=(const AsyncJob&)=delete;
  void cancel() {cancellation_.request_stop();}
  bool valid() const {return result_.valid();}
  bool ready() const {return valid() && result_.wait_for(std::chrono::seconds(0))==std::future_status::ready;}
  bool running() const {return valid() && !ready();}
  T get() {return result_.get();}
  template<class Function> void start(Function work) {
    cancel();cancellation_=std::stop_source{};
    std::promise<T> promise;result_=promise.get_future();
    std::thread([promise=std::move(promise),token=cancellation_.get_token(),work=std::move(work)]() mutable {
      try {promise.set_value(work(token));}
      catch(...) {promise.set_exception(std::current_exception());}
    }).detach();
  }
};
} // namespace openxmb::desktop
