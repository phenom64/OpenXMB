#include "openxmb/desktop/async_job.hpp"
#include <iostream>
#include <stdexcept>
using namespace openxmb::desktop;
int main() {
  try {
    std::promise<void> entered,release;std::promise<bool> finished;auto gate=release.get_future().share();auto complete=finished.get_future();
    {
      AsyncJob<bool> job;
      job.start([&entered,&finished,gate](std::stop_token stop) {entered.set_value();gate.wait();const bool cancelled=stop.stop_requested();finished.set_value(cancelled);return cancelled;});
      entered.get_future().wait();
    }
    // A joining destructor would deadlock before this line.
    release.set_value();if(!complete.get())throw std::runtime_error("Closing the owner did not request cancellation");
    AsyncJob<int> job;job.start([](std::stop_token){return 7;});
    if(job.get()!=7)throw std::runtime_error("Worker result was lost");
    job.start([](std::stop_token)->int {throw std::runtime_error("fixture");});
    try {static_cast<void>(job.get());throw std::logic_error("Worker error was suppressed");}catch(const std::runtime_error& error) {if(std::string(error.what())!="fixture")throw;}
    std::cout<<"Nonblocking UI worker lifetime and result checks passed\n";
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
