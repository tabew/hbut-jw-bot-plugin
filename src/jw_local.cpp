#include "jw_local.hpp"
#include "jw_util.hpp"
#include "simple_json.hpp"

#include <algorithm>
#include <filesystem>

namespace jw {
namespace {

std::string times_path() { return data_dir("hbut_jw") + "/times.json"; }
std::string edits_path(int64_t qq) { return data_dir("hbut_jw/custom") + "/" + std::to_string(qq) + ".json"; }

} // namespace

std::vector<PeriodTime> default_period_times() {
    static const char* kTimes[][3] = {
        {"1", "08:20", "09:05"}, {"2", "09:10", "09:55"}, {"3", "10:15", "11:00"},
        {"4", "11:05", "11:50"}, {"5", "14:00", "14:45"}, {"6", "14:50", "15:35"},
        {"7", "15:55", "16:40"}, {"8", "16:45", "17:30"}, {"9", "18:30", "19:15"},
        {"10", "19:20", "20:05"}, {"11", "20:10", "20:55"},
    };
    std::vector<PeriodTime> out;
    for (const auto& row : kTimes) {
        PeriodTime t;
        t.p = atoi(row[0]);
        t.start = row[1];
        t.end = row[2];
        out.push_back(t);
    }
    return out;
}

std::vector<PeriodTime> load_period_times() {
    std::vector<PeriodTime> defaults = default_period_times();
    std::string text = read_file_text(times_path());
    if (text.empty()) return defaults;

    auto v = simplejson::Value::parse(text);
    const auto& arr = v["periods"];
    if (!arr.is_array() || arr.empty()) return defaults;

    std::vector<PeriodTime> out;
    for (size_t i = 0; i < arr.size(); ++i) {
        const auto& item = arr[i];
        PeriodTime t;
        t.p = item["p"].get<int>();
        t.start = item["start"].get<std::string>();
        t.end = item["end"].get<std::string>();
        if (t.p > 0 && !t.start.empty()) out.push_back(t);
    }
    if (out.empty()) return defaults;
    std::sort(out.begin(), out.end(), [](const PeriodTime& a, const PeriodTime& b) { return a.p < b.p; });
    return out;
}

bool save_period_times(const std::vector<PeriodTime>& times) {
    simplejson::Value arr = simplejson::Value::array();
    for (const auto& t : times) {
        simplejson::Value item;
        item["p"] = t.p;
        item["start"] = t.start;
        item["end"] = t.end;
        arr.push_back(item);
    }
    simplejson::Value root;
    root["periods"] = arr;
    return write_file_binary(times_path(), root.dump(2));
}

bool reset_period_times() {
    std::error_code ec;
    return std::filesystem::remove(std::filesystem::u8path(times_path()), ec) || !file_exists(times_path());
}

// ---------------- 用户改写 ----------------

bool load_user_edits(int64_t qq, UserEdits* out) {
    *out = UserEdits();
    std::string text = read_file_text(edits_path(qq));
    if (text.empty()) return true;
    auto v = simplejson::Value::parse(text);
    if (!v.is_object()) return true;

    const auto& arr = v["edits"];
    if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& item = arr[i];
            CourseEdit e;
            e.match = item["match"].get<std::string>();
            if (e.match.empty()) continue;
            e.hidden = item["hidden"].get<int>() != 0;
            e.name = item["name"].get<std::string>();
            e.room = item["room"].get<std::string>();
            e.teacher = item["teacher"].get<std::string>();
            e.weekday = item["weekday"].get<int>();
            e.period = item["period"].get<int>();
            e.count = item["count"].get<int>();
            e.weeks = item["weeks"].get<std::string>();
            out->edits.push_back(e);
        }
    }

    const auto& adds = v["adds"];
    if (adds.is_array()) {
        for (size_t i = 0; i < adds.size(); ++i) {
            const auto& item = adds[i];
            hbut::Course c;
            c.name = item["name"].get<std::string>();
            if (c.name.empty()) continue;
            c.teacher = item["teacher"].get<std::string>();
            c.room = item["room"].get<std::string>();
            c.weekday = item["weekday"].get<int>();
            c.period = item["period"].get<int>();
            c.count = item["count"].get<int>();
            std::string weeks = item["weeks"].get<std::string>();
            if (c.weekday < 1) c.weekday = 1;
            if (c.period < 1) c.period = 1;
            if (c.count < 1) c.count = 2;
            c.weeks = hbut::parse_weeks(weeks);
            out->adds.push_back(c);
            out->add_weeks.push_back(weeks);
        }
    }
    return true;
}

bool save_user_edits(int64_t qq, const UserEdits& edits) {
    simplejson::Value edits_arr = simplejson::Value::array();
    for (const auto& e : edits.edits) {
        simplejson::Value item;
        item["match"] = e.match;
        item["hidden"] = e.hidden ? 1 : 0;
        if (!e.name.empty()) item["name"] = e.name;
        if (!e.room.empty()) item["room"] = e.room;
        if (!e.teacher.empty()) item["teacher"] = e.teacher;
        if (e.weekday > 0) item["weekday"] = e.weekday;
        if (e.period > 0) item["period"] = e.period;
        if (e.count > 0) item["count"] = e.count;
        if (!e.weeks.empty()) item["weeks"] = e.weeks;
        edits_arr.push_back(item);
    }

    simplejson::Value adds_arr = simplejson::Value::array();
    for (size_t i = 0; i < edits.adds.size(); ++i) {
        const auto& c = edits.adds[i];
        simplejson::Value item;
        item["name"] = c.name;
        if (!c.teacher.empty()) item["teacher"] = c.teacher;
        if (!c.room.empty()) item["room"] = c.room;
        item["weekday"] = c.weekday;
        item["period"] = c.period;
        item["count"] = c.count;
        item["weeks"] = i < edits.add_weeks.size() ? edits.add_weeks[i] : std::string();
        adds_arr.push_back(item);
    }

    simplejson::Value root;
    root["edits"] = edits_arr;
    root["adds"] = adds_arr;

    std::string path = edits_path(qq);
    std::string tmp = path + ".tmp";
    if (!write_file_binary(tmp, root.dump(2))) return false;
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(path), ec);
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
    if (ec) {
        std::filesystem::remove(std::filesystem::u8path(tmp), ec);
        return false;
    }
    return true;
}

bool clear_user_edits(int64_t qq) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(edits_path(qq)), ec);
    return true;
}

std::vector<hbut::Course> apply_edits(const std::vector<hbut::Course>& src, const UserEdits& edits) {
    std::vector<hbut::Course> out;
    out.reserve(src.size() + edits.adds.size());

    for (auto c : src) {
        const CourseEdit* hit = nullptr;
        for (const auto& e : edits.edits) {
            if (e.match == c.name) {
                hit = &e;
                break;
            }
        }
        if (hit) {
            if (hit->hidden) continue;
            if (!hit->name.empty()) c.name = hit->name;
            if (!hit->room.empty()) c.room = hit->room;
            if (!hit->teacher.empty()) c.teacher = hit->teacher;
            if (hit->weekday > 0) c.weekday = hit->weekday;
            if (hit->period > 0) c.period = hit->period;
            if (hit->count > 0) c.count = hit->count;
            if (!hit->weeks.empty()) c.weeks = hbut::parse_weeks(hit->weeks);
        }
        out.push_back(c);
    }
    for (const auto& a : edits.adds) out.push_back(a);
    return out;
}

std::vector<std::string> course_names(const std::vector<hbut::Course>& courses) {
    std::vector<std::string> names;
    for (const auto& c : courses) {
        if (c.name.empty()) continue;
        if (std::find(names.begin(), names.end(), c.name) == names.end()) names.push_back(c.name);
    }
    return names;
}

CourseEdit* find_or_add_edit(UserEdits* edits, const std::string& match) {
    for (auto& e : edits->edits) {
        if (e.match == match) return &e;
    }
    CourseEdit e;
    e.match = match;
    edits->edits.push_back(e);
    return &edits->edits.back();
}

} // namespace jw
