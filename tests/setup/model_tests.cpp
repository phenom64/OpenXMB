#include "openxmb/setup/model.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <tuple>

using namespace openxmb::setup;
namespace {
void check(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
Model fixture() {
    return Model({{"en", "English (United Kingdom)"}, {"fr", "Français"}},
        {{"Etc/UTC", "London", 0, false, true}, {"Asia/Kolkata", "Bombay, Calcutta, Madras, New Delhi", 330, false, true}},
        {.language="en", .region="GB", .user_name="User 1"});
}
Instant utc(int year,int month,int day,int hour=0,int minute=0,int second=0) {
    using namespace std::chrono;
    return sys_days{std::chrono::year{year}/std::chrono::month{static_cast<unsigned>(month)}/std::chrono::day{static_cast<unsigned>(day)}}+
        hours{hour}+minutes{minute}+seconds{second};
}
void time_zone_checks() {
    const auto zones=time_zones();
    const auto find=[&](const char* id)->const Zone& {
        const auto found=std::find_if(zones.begin(),zones.end(),[&](const auto& zone){return zone.id==id;});
        check(found!=zones.end(),"required time zone is present in catalog");
        return *found;
    };
    const auto& india=find("Asia/Kolkata");
    const auto& nepal=find("Asia/Kathmandu");
    const auto& london=find("Europe/London");
    const Zone negative_quarter{"Test/Fixed", "Test quarter hour", -225, false, false};
    check(india.label()=="GMT+05:30 Bombay, Calcutta, Madras, New Delhi", "India label retains exact city names and half-hour offset");
    check(nepal.label()=="GMT+05:45 Kathmandu", "Nepal label retains quarter-hour offset");
    check(negative_quarter.label()=="GMT-03:45 Test quarter hour", "negative quarter-hour offset formats correctly");
    check(!india.observes_daylight&&!nepal.observes_daylight, "India and Nepal have no daylight saving");
    check(local_time(india,utc(2026,1,1,20),true)==CivilTime{2026,1,2,1,30,0}, "India clock crosses UTC day boundary once");
    check(local_time(nepal,utc(2026,7,1,20),true)==CivilTime{2026,7,2,1,45,0}, "Nepal clock preserves quarter-hour offset");
    check(to_instant(india,{2026,1,2,1,30},true)==utc(2026,1,1,20), "India wall clock converts back to the original UTC instant");
    check(local_time(negative_quarter,utc(2026,1,1,2),true)==CivilTime{2025,12,31,22,15,0}, "negative offset crosses year boundary correctly");
    check(!to_instant(india,{2025,2,29},false), "invalid calendar date cannot become a manual instant");
    check(local_time(london,utc(2026,1,15,12),false)==CivilTime{2026,1,15,12,0,0}, "London standard clock stays UTC in winter");
    check(local_time(london,utc(2026,7,15,12),false)==CivilTime{2026,7,15,12,0,0}, "disabling automatic daylight keeps London at standard time in summer");
    if(london.rules_available) {
        check(london.observes_daylight,"available London rules identify daylight saving");
        check(local_time(london,utc(2026,1,15,12),true)==CivilTime{2026,1,15,12,0,0}, "London winter follows GMT");
        check(local_time(london,utc(2026,7,15,12),true)==CivilTime{2026,7,15,13,0,0}, "London summer follows BST");
        check(to_instant(london,{2026,7,15,13,0},true)==utc(2026,7,15,12), "London summer wall time round trips without double offset");
        check(!to_instant(london,{2026,3,29,1,30},true), "spring gap rejects nonexistent London wall time");
        check(!to_instant(london,{2026,10,25,1,30},true), "autumn fold rejects ambiguous London wall time");
        check(to_instant(london,{2026,3,29,0,30},true)==utc(2026,3,29,0,30), "wall time before spring gap is accepted");
        check(to_instant(london,{2026,3,29,2,30},true)==utc(2026,3,29,1,30), "wall time after spring gap is accepted");
        check(to_instant(london,{2026,10,25,2,30},true)==utc(2026,10,25,2,30), "wall time after autumn fold is accepted");
        check(to_instant(london,{2026,3,29,1,30},false)==utc(2026,3,29,1,30), "fixed-standard mode accepts spring wall time with automatic daylight off");
        Model gap({{"en","English"}},{london},{.language="en",.region="GB",.user_name="User 1",.automatic_daylight=true});
        gap.show_page(Page::date_time);
        const auto before_gap=utc(2026,3,29,0,30);
        gap.command(Command::confirm,before_gap);
        for(int i=0;i<3;++i)gap.command(Command::right,before_gap);
        gap.command(Command::up,before_gap);gap.command(Command::confirm,before_gap);
        check(gap.editing()&&!gap.error().empty()&&gap.draft().automatic_time,"accepting a skipped wall time keeps editor open with error and no committed clock");
        gap.command(Command::back,before_gap);
        check(!gap.editing()&&gap.draft().automatic_time,"skipped-time edit can be cancelled");
        std::cout << "London seasonal, gap and fold checks exercised with zone rules\n";
    } else {
        check(!london.observes_daylight,"unavailable London rules do not advertise automatic daylight capability");
        check(local_time(london,utc(2026,7,15,12),true)==CivilTime{2026,7,15,12,0,0}, "missing rules use explicit fixed standard offset");
        std::cout << "SKIP: London seasonal/gap/fold rule checks; standard-library zone rules unavailable (fixed-offset fallback tested)\n";
    }
    check(format_time({2026,1,2,3,4,5})=="02/01/2026  03:04", "date/time preview uses zero-padded day/month and 24-hour clock");
    check(format_time({2026,1,2,3,4,5},true)=="02/01/2026  03:04:05", "date/time preview optionally includes seconds");
}
}
int main() {
    try {
        check(requires_setup(false, {}), "fresh installation enters setup");
        check(!requires_setup(true, {}), "Beta 3 profile migrates without setup or data reset");
        check(requires_setup(true, false), "reset marker re-enters setup on restart");
        check(!requires_setup(false, true), "completed setup is respected");
        const auto choices=supported_languages();
        const std::vector<Zone> utc_zone{{"Etc/UTC","UTC",0,false,true}};
        for(const auto& [locale,language,region]:std::vector<std::tuple<std::string,std::string,std::string>>{
            {"zh-HK","zh_TW","HK"},{"zh-MO","zh_TW","MO"},{"zh-Hant-TW","zh_TW","TW"},
            {"zh-Hans-SG","zh_CN","SG"},{"en-GB","en","GB"},{"fr_FR.UTF-8","fr","FR"}}) {
            const auto detected=detect_defaults(choices,utc_zone,locale);
            check(detected.language==language && detected.region==region,"host locale uses shipped regional language and real region");
        }
        check(valid({2024,2,29,23,59,59}) && !valid({2025,2,29}), "Gregorian leap-day validation");
        check(adjust_segment({2024,3,31,12,0},1,-1)==CivilTime{2024,2,29,12,0}, "month edit clamps day");
        check(adjust_segment({2024,2,29,12,0},2,1)==CivilTime{2025,2,28,12,0}, "year edit clamps leap day");
        check(adjust_segment({2024,1,1},0,-1).day==31, "day spinner wraps within month");
        check(adjust_segment({2024,1,1,23,59},3,1).hour==0, "hour spinner wraps without changing date");
        check(adjust_segment({2024,12,31,23,59},1,1)==CivilTime{2024,1,31,23,59}, "month wraps without silently changing year");
        check(adjust_segment({2024,1,31},1,1)==CivilTime{2024,2,29}, "month spinner clamps long day to leap February");
        check(adjust_segment({2024,1,1,12,0},4,-1)==CivilTime{2024,1,1,12,59}, "minute spinner wraps without changing hour");
        check(!valid({2026,0,1})&&!valid({2026,13,1})&&!valid({2026,1,0})&&!valid({2026,1,32}), "invalid date segments are rejected");
        check(!valid({2026,1,1,24})&&!valid({2026,1,1,0,60})&&!valid({2026,1,1,0,0,60}), "invalid clock segments are rejected");
        check(valid({1900,1,1})&&valid({2100,12,31,23,59,59}), "manual clock supports both safe platform year boundaries");
        check(!valid({1899,12,31})&&!valid({2101,1,1}), "manual years outside safe platform range are rejected");
        check(adjust_segment({2100,12,31},2,1)==CivilTime{1900,12,31}, "year spinner wraps at safe upper bound");
        check(adjust_segment({1900,1,1},2,-1)==CivilTime{2100,1,1}, "year spinner wraps at safe lower bound");
        check(!to_instant({"Etc/UTC","UTC",0,false,false},{9999,12,31},false), "out-of-range manual year cannot overflow system clock conversion");
        auto model=fixture();
        const auto epoch=Instant{std::chrono::seconds{0}};
        model.advance_intro(); model.advance_intro();
        check(model.page()==Page::language, "intro reaches language page");
        model.command(Command::up,epoch);
        check(model.draft().language=="fr", "language choices wrap backwards");
        model.command(Command::down,epoch);
        check(model.draft().language=="en", "language choices wrap forwards");
        model.command(Command::down,epoch);
        check(model.draft().language=="fr", "language choice updates draft");
        model.command(Command::right,epoch);
        model.command(Command::up,epoch);
        check(model.draft().zone_index==1,"time zone choices wrap backwards");
        model.command(Command::down,epoch);
        model.command(Command::down,epoch);
        check(model.zones()[model.draft().zone_index].label()=="GMT+05:30 Bombay, Calcutta, Madras, New Delhi", "India display label is exact");
        check(model.current_time(epoch)==CivilTime{1970,1,1,5,30,0}, "UTC instant gets selected zone offset exactly once");
        model.command(Command::extra,epoch); model.command(Command::confirm,epoch);
        check(!model.draft().automatic_daylight, "India cannot acquire daylight saving");
        model.command(Command::extra,epoch); model.command(Command::right,epoch);
        model.command(Command::confirm,epoch);
        check(model.editing(), "date page has explicit editing mode");
        model.command(Command::left,epoch);
        check(model.segment()==4,"editor segment focus wraps backwards");
        model.command(Command::right,epoch);
        check(model.segment()==0,"editor segment focus wraps forwards");
        model.command(Command::up,epoch); model.command(Command::back,epoch);
        check(model.draft().automatic_time && !model.editing(), "cancel date editor preserves automatic clock");
        model.command(Command::confirm,epoch); model.command(Command::up,epoch); model.command(Command::confirm,epoch);
        check(!model.draft().automatic_time, "accept date editor selects OpenXMB manual clock");
        model.command(Command::extra,epoch);
        check(model.draft().automatic_time && model.draft().clock_offset.count()==0, "automatic clock clears manual offset");
        model.draft().automatic_time=false;model.draft().clock_offset=std::chrono::hours{2};
        const auto prior_clock=model.current_time(epoch);
        model.command(Command::confirm,epoch);model.command(Command::up,epoch);model.command(Command::back,epoch);
        check(!model.draft().automatic_time&&model.draft().clock_offset==std::chrono::hours{2}&&model.current_time(epoch)==prior_clock,"cancel manual edit preserves earlier clock offset");
        model.command(Command::right,epoch); model.set_name("  Player One  ");
        model.set_name("");model.set_name(" \t\r\n ");model.set_name(std::string{"\x01\x7f",2});
        check(model.draft().user_name=="Player One","empty, whitespace-only and control-only names preserve valid name");
        model.set_name("  Player\t\nOne\x7f  ");
        check(model.draft().user_name=="PlayerOne","embedded ASCII controls are removed from user name");
        model.set_name("  Player One  ");
        model.command(Command::right,epoch);
        check(model.page()==Page::summary && model.draft().user_name=="Player One", "name trims and summary stages changes");
        model.command(Command::confirm,epoch);
        check(model.confirmation() && !model.confirmation_yes() && model.page()==Page::summary, "completion requires confirmation with No selected by default");
        model.command(Command::confirm,epoch);
        check(!model.confirmation()&&model.page()==Page::summary&&model.draft().user_name=="Player One","confirming No returns to summary and preserves draft");
        model.command(Command::confirm,epoch);
        model.command(Command::back,epoch);
        check(!model.confirmation() && model.page()==Page::summary, "cancel confirmation keeps draft");
        model.command(Command::confirm,epoch); model.command(Command::left,epoch); model.command(Command::confirm,epoch);
        check(model.page()==Page::complete, "confirmed summary completes setup");
        time_zone_checks();
        std::cout << "Setup migration, time-zone, date editor and navigation checks passed\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
