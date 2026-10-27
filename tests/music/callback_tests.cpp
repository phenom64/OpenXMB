#define SDL_MAIN_HANDLED
#include "openxmb/music/player.hpp"
#include "callback_test_hooks.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace test = openxmb::music::callback_test;
using openxmb::music::Player;
using openxmb::music::Status;
using namespace std::chrono_literals;

void* operator new(std::size_t bytes) {
  if(test::in_callback) test::audit.allocations.fetch_add(1);
  if(auto* result=std::malloc(bytes ? bytes : 1)) return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer,std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer,std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes,std::align_val_t alignment) {
  if(test::in_callback) test::audit.allocations.fetch_add(1);
  const auto align=static_cast<std::size_t>(alignment);
#ifdef _WIN32
  auto* result=_aligned_malloc(bytes?bytes:1,align);
#else
  auto* result=std::aligned_alloc(align,((bytes?bytes:1)+align-1)/align*align);
#endif
  if(result) return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes,std::align_val_t alignment) {return ::operator new(bytes,alignment);}
void operator delete(void* pointer,std::align_val_t) noexcept {
#ifdef _WIN32
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}
void operator delete[](void* pointer,std::align_val_t alignment) noexcept {::operator delete(pointer,alignment);}
void operator delete(void* pointer,std::size_t,std::align_val_t alignment) noexcept {::operator delete(pointer,alignment);}
void operator delete[](void* pointer,std::size_t,std::align_val_t alignment) noexcept {::operator delete(pointer,alignment);}

void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Predicate> void until(Predicate predicate,const char* message) {
  const auto deadline=std::chrono::steady_clock::now()+3s;
  while(!predicate()) {
    if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error(message);
    std::this_thread::sleep_for(100us);
  }
}
void fixture(const std::filesystem::path& path) {
  std::ofstream out(path,std::ios::binary);
  auto word=[&](std::uint32_t value,unsigned bytes) { for(unsigned i=0;i<bytes;++i)out.put(static_cast<char>(value>>(i*8))); };
  const unsigned frames=48000*20;
  out.write("RIFF",4);word(36+frames*4,4);out.write("WAVEfmt ",8);word(16,4);
  word(1,2);word(2,2);word(48000,4);word(192000,4);word(4,2);word(16,2);
  out.write("data",4);word(frames*4,4);
  for(unsigned i=0;i<frames;++i) {
    const auto value=i<48000?8192:i<96000?-16384:i<144000?0:24576;
    word(static_cast<std::uint16_t>(static_cast<std::int16_t>(value)),2);
    word(static_cast<std::uint16_t>(static_cast<std::int16_t>(value)),2);
  }
}
std::atomic<std::uint64_t> expected_epoch{}, observed{}, corrupt{};
std::atomic<float> expected_sample{};
std::atomic<bool> silence_check{};
bool float_output{}; // Set before opening the audio device, never while active.
void SDLCALL observe(void*, Uint8* bytes,int length) {
  const bool silence=silence_check.load();
  if(!silence && test::audit.last_output_epoch.load()!=expected_epoch.load()) return;
  const auto wanted=silence?0.0F:expected_sample.load();
  const auto count=length/(float_output?4:2);
  const auto music_samples=static_cast<int>(test::audit.last_frames.load()*2);
  bool bad=false;
  for(int i=0;i<count;++i) {
    const auto value=float_output?reinterpret_cast<float*>(bytes)[i]:reinterpret_cast<std::int16_t*>(bytes)[i]/32768.0F;
    if(std::abs(value-((!silence && i>=music_samples)?0.0F:wanted))>0.002F) bad=true;
  }
  if(bad) corrupt.fetch_add(1);
  observed.fetch_add(1);
}
void marker(Player& player,double position,float value,bool playing) {
  // Synchronize the test observer's configuration/counters independently of
  // the transport under test. This cannot mask PCM races: callbacks continue
  // running, and the deterministic held-consumer tests call seek/pause directly.
  Mix_SetPostMix(nullptr,nullptr);
  player.pause();
  check(player.seek(position),"marker seek failed");
  expected_sample.store(value);expected_epoch.store(test::audit.epoch.load());
  observed.store(0);corrupt.store(0);silence_check.store(false);
  Mix_SetPostMix(observe,nullptr);
  if(playing) check(player.play(),"marker playback failed");
}
template<class Command> void held_consumer(Command command) {
  test::audit.hold_callback.store(true);
  until([] { return test::audit.callback_held.load(); },"callback did not enter hold seam");
  std::atomic<bool> started{},finished{};
  const auto quiesces=test::audit.quiesces.load();
  std::exception_ptr failure;
  std::jthread control([&] {
    started.store(true);
    try { command(); } catch(...) { failure=std::current_exception(); }
    finished.store(true);
  });
  struct Release { ~Release() {test::audit.hold_callback.store(false);} } release;
  until([&] {return started.load();},"control thread did not start");
  until([&] {return test::audit.quiesces.load()>quiesces;},"control thread did not request callback quiescence");
  std::this_thread::sleep_for(15ms);
  const bool waited=!finished.load();
  test::audit.hold_callback.store(false);
  control.join();
  check(waited,"transport mutated/reset storage before the in-flight callback finished");
  if(failure) std::rethrow_exception(failure);
}
void run(const std::filesystem::path& path,bool floating) {
  float_output=floating;
  check(Mix_OpenAudio(48000,floating?AUDIO_F32SYS:AUDIO_S16SYS,2,128)==0,Mix_GetError());
  auto player=std::make_unique<Player>();
  check(player->set_queue({{path,"Concurrent PCM"}}),"stress fixture did not open");
  player->set_volume(1);player->set_ducking_enabled(false);
  Mix_SetPostMix(observe,nullptr);
  marker(*player,0.1,0.25F,true);
  until([] {return observed.load()>=3;},"known PCM marker not emitted");
  check(corrupt.load()==0,"initial marker PCM was corrupted");

  // Delay the producer with existing PCM queued. The real audio callback must
  // keep finishing while the control thread cannot publish any new samples.
  std::this_thread::sleep_for(20ms);
  test::audit.hold_producer.store(true);
  std::jthread producer([&] {player->tick();});
  struct ReleaseProducer { ~ReleaseProducer() {test::audit.hold_producer.store(false);} } release;
  until([] {return test::audit.producer_held.load();},"producer hold seam was not reached");
  const auto before=test::audit.completed.load();
  std::this_thread::sleep_for(35ms);
  until([&] {return test::audit.completed.load()>=before+3;},"callback did not progress independently of stalled producer");
  const auto progress=test::audit.completed.load()-before;
  test::audit.hold_producer.store(false);producer.join();
  check(progress>=3,"callback waited for the stalled producer");

  held_consumer([&] {player->pause();});
  const auto paused=player->snapshot().position;
  std::this_thread::sleep_for(15ms);
  Mix_SetPostMix(nullptr,nullptr);
  silence_check.store(true);corrupt.store(0);observed.store(0);
  Mix_SetPostMix(observe,nullptr);
  until([] {return observed.load()>=3;},"paused callback did not continue returning");
  check(corrupt.load()==0,"pause emitted buffered old music");
  player->tick();check(player->snapshot().position==paused,"pause failed to freeze consumed-sample clock");

  marker(*player,0.2,0.25F,true);
  held_consumer([&] {check(player->seek(1.2),"concurrent forward seek failed");});
  Mix_SetPostMix(nullptr,nullptr);
  expected_sample.store(-0.5F);expected_epoch.store(test::audit.epoch.load());
  corrupt.store(0);observed.store(0);
  Mix_SetPostMix(observe,nullptr);
  until([] {return observed.load()>=3;},"post-seek marker not emitted");
  check(corrupt.load()==0,"seek leaked old-epoch PCM or reset live ring slots");

  for(unsigned iteration=0;iteration<100;++iteration) {
    const unsigned section=iteration%4;
    const float values[]{0.25F,-0.5F,0.0F,0.75F};
    marker(*player,section+0.1,values[section],true);
    until([] {return observed.load()>=1;},"rapid seek failed to emit new epoch");
    check(corrupt.load()==0,"rapid seek/flush emitted stale or torn PCM");
    player->pause();
    const auto frozen=player->snapshot().position;
    if(iteration%10==0) {
      std::this_thread::sleep_for(5ms);player->tick();
      check(player->snapshot().position==frozen,"paused clock advanced during stress");
    }
  }
  // Keep publishing/consuming beyond the one-second capacity so both PCM
  // indices wrap their physical storage, without flushes hiding wrap errors.
  marker(*player,3.1,0.75F,true);
  const auto deadline=std::chrono::steady_clock::now()+1300ms;
  while(std::chrono::steady_clock::now()<deadline) {
    player->tick();std::this_thread::sleep_for(2ms);
  }
  check(observed.load()>100 && corrupt.load()==0,"streaming ring wrap corrupted PCM or playback stalled");
  check(test::audit.max_read.load()>test::audit.capacity.load() && test::audit.max_write.load()>test::audit.capacity.load(),"test did not exercise physical ring wraparound");
  // Let analysis fill while audio still has 400ms queued. Excess analysis must
  // be dropped without waiting or disturbing the live PCM output.
  player->pause();marker(*player,3.1,0.75F,true);
  const auto drops=test::audit.analysis_drops.load();
  std::this_thread::sleep_for(250ms);
  check(test::audit.analysis_drops.load()>drops,"analysis-full path was not exercised");
  check(corrupt.load()==0,"analysis backpressure corrupted audio PCM");
  player->tick();
  marker(*player,0.1,0.25F,true);
  held_consumer([&] {player->shutdown();});
  const auto retired=test::audit.calls.load();
  player.reset();std::this_thread::sleep_for(20ms);
  check(test::audit.calls.load()==retired,"callback retained freed player userdata after shutdown");
  for(unsigned i=0;i<12;++i) {
    Player replacement;
    check(replacement.set_queue({{path,"Replacement"}}),"replacement fixture failed");
    replacement.pause();check(replacement.seek(1.1),"replacement seek failed");
    replacement.shutdown();replacement.shutdown();
  }
  Mix_SetPostMix(nullptr,nullptr);Mix_CloseAudio();
}
int main() {
  SDL_SetMainReady();
  const auto path=std::filesystem::temp_directory_path()/("openxmb-callback-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".wav");
  try {
    check(SDL_InitSubSystem(SDL_INIT_AUDIO)==0,SDL_GetError());
    fixture(path);run(path,false);run(path,true);
    check(test::audit.allocations.load()==0,"C++ allocation occurred inside the callback");
    check(test::audit.invariant_failures.load()==0,"SPSC occupancy/atomic invariant failed");
    check(test::audit.max_queued.load()<=48000*2*0.4,"PCM producer exceeded the 400ms high watermark");
    check(test::audit.capacity.load()==48000*2,"PCM ring capacity is not bounded to one second");
    check(test::audit.timed.load()>100,"insufficient real callbacks were measured");
    std::cout<<"SPSC callback stress passed: calls="<<test::audit.calls.load()<<" admitted="<<test::audit.callbacks.load()
      <<" timed="<<test::audit.timed.load()<<" max_callback_us="<<test::audit.max_ns.load()/1000.0
      <<" allocations="<<test::audit.allocations.load()<<" invariant_failures="<<test::audit.invariant_failures.load()
      <<" max_queued_samples="<<test::audit.max_queued.load()<<" max_buffer_frames="<<test::audit.max_buffer_frames.load()<<'\n';
    std::filesystem::remove(path);SDL_QuitSubSystem(SDL_INIT_AUDIO);
  } catch(const std::exception& error) {
    test::audit.hold_callback.store(false);test::audit.hold_producer.store(false);
    Mix_SetPostMix(nullptr,nullptr);Mix_CloseAudio();SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::filesystem::remove(path);std::cerr<<error.what()<<'\n';return 1;
  }
}
