// jw_cache.hpp —— 课表本地缓存：登录一次后即使 Cookie 失效也能出图
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "jw_hbut.hpp"

namespace jw {

struct WeekCache {
    std::string semester;              // 缓存对应的学期（2026-2027-1）
    std::string base;                  // 当时用的教务域名
    long long fetched_ms = 0;          // 拉取时间
    std::vector<hbut::Course> courses; // 原始课程（未套用户改名/隐藏）
    hbut::TermCalendar calendar;

    bool valid() const { return !courses.empty() && !calendar.weeks.empty(); }
};

bool load_cache(int64_t qq, WeekCache* out);
bool save_cache(int64_t qq, const WeekCache& cache);
bool clear_cache(int64_t qq);
// 缓存时间的人话描述，如 "3 小时前"
std::string cache_age_text(long long fetched_ms);

} // namespace jw
