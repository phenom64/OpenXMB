#include "openxmb/xmb/trophy_notice.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace openxmb;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
desktop::TrophyWatchSession origin(std::uint64_t generation=7) {
  return {generation,"session-a","profile","game","hdd0","00000001"};
}
desktop::TrophyUnlockEvent unlock(const desktop::TrophyWatchSession& session,std::string title) {
  desktop::TrophyUnlockEvent event;event.session=session;event.provider="rpcs3";event.user_id=session.rpcs3_user_id;
  event.set_id="NPWR00000_00";event.trophy_id="1";event.achievement.title=std::move(title);
  event.achievement.grade=desktop::TrophyGrade::gold;event.achievement.unlock_state_known=true;event.achievement.unlocked=true;
  return event;
}
bool near(float a,float b){return std::abs(a-b)<0.0001F;}
}
int main(){try{
  const auto session=origin();
  xmb::TrophyNoticeQueue queue;
  require(!queue.current()&&queue.opacity(1,false)==0,"an empty queue must render nothing");
  require(!queue.push(unlock(origin(8),"Stale"),session),"another session generation was accepted");
  auto other_user=session;other_user.rpcs3_user_id="00000002";
  require(!queue.push(unlock(other_user,"Other user"),session),"another RPCS3 user was accepted");
  auto provider=unlock(session,"Provider");provider.provider="retroachievements";
  require(!queue.push(provider,session),"a non-RPCS3 provider was accepted");
  auto unknown=unlock(session,"Unknown");unknown.achievement.unlock_state_known=false;
  require(!queue.push(unknown,session),"an unknown unlock state was accepted");
  auto locked=unlock(session,"Locked");locked.achievement.unlocked=false;
  require(!queue.push(locked,session),"a locked trophy was accepted");
  require(queue.size()==0,"rejected edges were queued");

  require(queue.push(unlock(session,"First"),session)&&queue.push(unlock(session,"Second"),session),"genuine unlocks were rejected");
  const auto first=queue.current()->serial;
  require(queue.current()->event.achievement.title=="First"&&first!=0,"queue order or serial changed");
  require(queue.opacity(10,false)==0&&!queue.expire(100),"an unstarted notice became visible or expired");
  const std::optional<desktop::TrophyWatchSession> current_origin=session;
  require(!xmb::trophy_notice_sound_ready(queue,current_origin,true,false,0),"unpresented notice could play audio");

  queue.start(10);queue.start(11);
  require(xmb::trophy_notice_sound_ready(queue,current_origin,true,false,0),"visible genuine notice could not play audio");
  require(!xmb::trophy_notice_sound_ready(queue,current_origin,false,false,0),"hidden notice could play audio");
  require(!xmb::trophy_notice_sound_ready(queue,current_origin,true,true,0),"capture suppression allowed notice audio");
  require(!xmb::trophy_notice_sound_ready(queue,current_origin,true,false,first),"unavailable banner could play audio");
  require(!xmb::trophy_notice_sound_ready(queue,std::nullopt,true,false,0),"cancelled session could play audio");
  for(auto changed:{origin(8),other_user})require(!xmb::trophy_notice_sound_ready(queue,changed,true,false,0),"stale session/user could play audio");
  auto other_profile=session;other_profile.profile_directory="other-profile";
  require(!xmb::trophy_notice_sound_ready(queue,other_profile,true,false,0),"another profile could play audio");
  require(queue.started()&&near(queue.opacity(10,false),0)&&near(queue.opacity(10.09,false),0.5F),"entrance fade incorrect");
  require(near(queue.opacity(12,false),1)&&near(queue.opacity(14.875,false),0.5F),"hold or exit fade incorrect; a second start reset time");
  require(near(queue.opacity(10,true),1)&&near(queue.opacity(14.99,true),1),"reduced motion must not fade");
  require(queue.opacity(15,true)==0&&queue.opacity(15,false)==0,"expired notice remained visible");
  require(!queue.expire(14.999)&&queue.expire(15),"hold expiry boundary incorrect");
  require(queue.current()&&queue.current()->event.achievement.title=="Second"&&queue.current()->serial>first,"next notice missing");
  require(!queue.started()&&queue.opacity(15.5,false)==0,"next notice inherited the previous hold");

  for(int i=0;i<20;++i)static_cast<void>(queue.push(unlock(session,"Burst "+std::to_string(i)),session));
  require(queue.size()==xmb::TrophyNoticeQueue::capacity,"queue exceeded its bound");
  require(queue.current()->event.achievement.title=="Second","overflow displaced a queued notice");
  queue.start(20);queue.clear();
  require(!queue.current()&&!queue.started()&&queue.opacity(20,false)==0,"cancellation retained a notice");
  require(!xmb::trophy_notice_sound_ready(queue,current_origin,true,false,0),"empty queue could play audio");
  queue.start(21);require(!queue.started(),"an empty queue started a hold");
  require(queue.push(unlock(session,"After"),session)&&queue.current()->serial>first,"serials were reused after cancellation");

  const auto primary=xmb::trophy_notice_bounds(0,0,1920,1080);
  require(primary&&*primary==xmb::TrophyNoticeBounds{1267,140,595,97,1.0F},"1080p bounds changed");
  const auto left=xmb::trophy_notice_bounds(-2560,-200,2560,1440);
  require(left&&left->x==-2560+1690&&left->y==-200+187&&left->width==794&&left->height==130&&near(left->scale,4.0F/3.0F),"secondary monitor offset ignored");
  const auto square=xmb::trophy_notice_bounds(0,0,1024,768);
  require(square&&near(square->scale,1024.0F/1920.0F),"narrow monitor typography scale incorrect");
  require(!xmb::trophy_notice_bounds(0,0,0,1080)&&!xmb::trophy_notice_bounds(0,0,20000,1080)&&!xmb::trophy_notice_bounds(2147483000,0,3840,2160),"unusable monitor bounds accepted");
  std::cout<<"Trophy notice queue, timing and bounds fixtures passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
