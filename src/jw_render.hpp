// jw_render.hpp —— 用 GDI+ 把课表画成 PNG（系统自带 gdiplus，无需第三方库）
#pragma once
#include <string>
#include <vector>

#include "jw_hbut.hpp"

namespace jw {

struct TimetableImage {
    std::string semester_label;  // 2026-2027学年 第一学期
    int week = 1;
    int total_weeks = 0;
    std::string date_range;      // 9月7日 - 9月13日
    std::string student_line;    // 姓名 · 学号
    std::string footer;          // 生成时间 / 数据来源
    int today_weekday = -1;      // 0=周一，-1 表示本周高亮
    std::vector<Date> day_dates; // 7 个日期（可为空）
    std::vector<hbut::Course> courses;
};

bool gdiplus_init();
void gdiplus_shutdown();

bool render_timetable_png(const TimetableImage& data, const std::string& out_path, std::string* err);

} // namespace jw
