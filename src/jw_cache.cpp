#include "jw_cache.hpp"
#include "jw_util.hpp"
#include "simple_json.hpp"

#include <cstdio>
#include <filesystem>

namespace jw {
namespace {

std::string cache_path(int64_t qq) {
    return data_dir("hbut_jw/cache") + "/" + std::to_string(qq) + ".json";
}

} // namespace

bool load_cache(int64_t qq, WeekCache* out) {
    *out = WeekCache();
    std::string text = read_file_text(cache_path(qq));
    if (text.empty()) return false;

    auto v = simplejson::Value::parse(text);
    if (!v.is_object()) return false;

    out->semester = v["sem"].get<std::string>();
    out->base = v["base"].get<std::string>();
    out->fetched_ms = v["ts"].get<int64_t>();

    const auto& cal = v["cal"];
    if (cal.is_object()) {
        out->calendar.total_weeks = cal["total"].get<int>();
        out->calendar.current_week = cal["cur"].get<int>();
        out->calendar.from_school = cal["school"].get<int>() != 0;
        const auto& weeks = cal["weeks"];
        if (weeks.is_array()) {
            for (size_t i = 0; i < weeks.size(); ++i) {
                const auto& row = weeks[i];
                hbut::WeekSpan span;
                span.week = row["w"].get<int>();
                Date mon, sun;
                if (!parse_date(row["mon"].get<std::string>(), &mon)) continue;
                if (!parse_date(row["sun"].get<std::string>(), &sun)) sun = add_days(mon, 6);
                span.monday = mon;
                span.sunday = sun;
                out->calendar.weeks.push_back(span);
            }
        }
    }

    const auto& courses = v["courses"];
    if (courses.is_array()) {
        for (size_t i = 0; i < courses.size(); ++i) {
            const auto& row = courses[i];
            hbut::Course c;
            c.name = row["n"].get<std::string>();
            if (c.name.empty()) continue;
            c.teacher = row["t"].get<std::string>();
            c.room = row["r"].get<std::string>();
            c.building = row["b"].get<std::string>();
            c.class_name = row["cl"].get<std::string>();
            c.weekday = row["wd"].get<int>();
            c.period = row["p"].get<int>();
            c.big_section = row["bs"].get<int>();
            c.count = row["cnt"].get<int>();
            if (c.weekday < 1) c.weekday = 1;
            if (c.period < 1) c.period = 1;
            if (c.count < 1) c.count = 1;
            const auto& wk = row["wk"];
            if (wk.is_array()) {
                for (size_t k = 0; k < wk.size(); ++k) {
                    int w = wk[k].get<int>();
                    if (w > 0) c.weeks.push_back(w);
                }
            }
            out->courses.push_back(c);
        }
    }
    return out->valid();
}

bool save_cache(int64_t qq, const WeekCache& cache) {
    if (!cache.valid()) return false;

    simplejson::Value root;
    root["sem"] = cache.semester;
    root["base"] = cache.base;
    root["ts"] = cache.fetched_ms;

    simplejson::Value cal;
    cal["total"] = cache.calendar.total_weeks;
    cal["cur"] = cache.calendar.current_week;
    cal["school"] = cache.calendar.from_school ? 1 : 0;
    simplejson::Value weeks = simplejson::Value::array();
    for (const auto& span : cache.calendar.weeks) {
        simplejson::Value row;
        row["w"] = span.week;
        row["mon"] = fmt_date(span.monday);
        row["sun"] = fmt_date(span.sunday);
        weeks.push_back(row);
    }
    cal["weeks"] = weeks;
    root["cal"] = cal;

    simplejson::Value courses = simplejson::Value::array();
    for (const auto& c : cache.courses) {
        simplejson::Value row;
        row["n"] = c.name;
        if (!c.teacher.empty()) row["t"] = c.teacher;
        if (!c.room.empty()) row["r"] = c.room;
        if (!c.building.empty()) row["b"] = c.building;
        if (!c.class_name.empty()) row["cl"] = c.class_name;
        row["wd"] = c.weekday;
        row["p"] = c.period;
        row["bs"] = c.big_section;
        row["cnt"] = c.count;
        simplejson::Value wk = simplejson::Value::array();
        for (int w : c.weeks) wk.push_back(simplejson::Value(w));
        row["wk"] = wk;
        courses.push_back(row);
    }
    root["courses"] = courses;

    std::string path = cache_path(qq);
    std::string tmp = path + ".tmp";
    if (!write_file_binary(tmp, root.dump(-1))) return false;
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(path), ec); // Windows 下 rename 不覆盖
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
    if (ec) {
        std::filesystem::remove(std::filesystem::u8path(tmp), ec);
        return false;
    }
    return true;
}

bool clear_cache(int64_t qq) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(cache_path(qq)), ec);
    return true;
}

std::string cache_age_text(long long fetched_ms) {
    if (fetched_ms <= 0) return "未知时间";
    long long diff = now_ms() - fetched_ms;
    if (diff < 0) diff = 0;
    long long minutes = diff / 60000;
    if (minutes < 1) return "刚刚";
    if (minutes < 60) return std::to_string(minutes) + " 分钟前";
    long long hours = minutes / 60;
    if (hours < 24) return std::to_string(hours) + " 小时前";
    long long days = hours / 24;
    return std::to_string(days) + " 天前";
}

} // namespace jw
