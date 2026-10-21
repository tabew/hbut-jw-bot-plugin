// jw_local.hpp —— 本地可配置项：作息时间 + 用户自定义/改写的课程
#pragma once
#include <string>
#include <vector>

#include "jw_hbut.hpp"

namespace jw {

// ---------------- 作息时间 ----------------

struct PeriodTime {
    int p = 1;
    std::string start; // "08:20"
    std::string end;   // "09:05"
};

// 湖工大默认作息（第 1-11 节）
std::vector<PeriodTime> default_period_times();

// data/hbut_jw/times.json；文件不存在时返回默认值
std::vector<PeriodTime> load_period_times();
bool save_period_times(const std::vector<PeriodTime>& times);
bool reset_period_times();

// ---------------- 用户改写 ----------------

// 对「教务里的某门课」的改写，match 是原课程名
struct CourseEdit {
    std::string match;
    bool hidden = false;
    std::string name;      // 空 = 不改
    std::string room;
    std::string teacher;
    int weekday = 0;       // 0 = 不改
    int period = 0;
    int count = 0;
    std::string weeks;     // 空 = 不改，形如 1-16 / 1-16(单)
};

struct UserEdits {
    std::vector<CourseEdit> edits;
    std::vector<hbut::Course> adds;      // 自定义追加课程（weeks 已解析）
    std::vector<std::string> add_weeks;  // 与 adds 一一对应的周次原文

    bool empty() const { return edits.empty() && adds.empty(); }
};

bool load_user_edits(int64_t qq, UserEdits* out);
bool save_user_edits(int64_t qq, const UserEdits& edits);
bool clear_user_edits(int64_t qq);

// 把改写套到教务课程上：先隐藏/替换，再追加自定义课程
std::vector<hbut::Course> apply_edits(const std::vector<hbut::Course>& src, const UserEdits& edits);

// 课程名去重（按出现顺序），用于给用户编号
std::vector<std::string> course_names(const std::vector<hbut::Course>& courses);

// 按名字找改写项（没有就新建）
CourseEdit* find_or_add_edit(UserEdits* edits, const std::string& match);

} // namespace jw
