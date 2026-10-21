// 本地调试工具：不参与插件构建，用来验证 AES 与课表出图效果。
// 编译（在 plugins\hbut_jw 下）：
//   cl /nologo /std:c++17 /MT /EHsc /utf-8 /I ..\..\include /Foobj\t_ obj\jw_util.obj ^
//      obj\jw_aes.obj obj\jw_hbut.obj obj\jw_http.obj obj\jw_render.obj ^
//      tests\preview.cpp /Fe:preview.exe /link gdiplus.lib winhttp.lib ole32.lib user32.lib gdi32.lib
// 运行：preview.exe  → 生成 tests\preview.png
#include <cstdio>
#include <string>
#include <vector>

#include "jw_aes.hpp"
#include "jw_hbut.hpp"
#include "jw_render.hpp"
#include "jw_util.hpp"

static std::string hex_to_bytes(const char* hex) {
    std::string out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back((char)((v(hex[i]) << 4) | v(hex[i + 1])));
    }
    return out;
}

static std::string to_hex(const std::string& bytes) {
    static const char* h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out.push_back(h[c >> 4]);
        out.push_back(h[c & 0xF]);
    }
    return out;
}

int main() {
    // 1) AES-128 已知向量（FIPS-197 附录 C.1）
    std::string key = hex_to_bytes("000102030405060708090a0b0c0d0e0f");
    std::string iv = hex_to_bytes("00000000000000000000000000000000");
    std::string plain = hex_to_bytes("00112233445566778899aabbccddeeff");
    std::string cipher = jw::aes128_cbc_encrypt(key, iv, plain);
    std::string first16 = to_hex(cipher.substr(0, 16));
    printf("AES block = %s\n", first16.c_str());
    printf("AES %s\n", first16 == "69c4e0d86a7b0430d8cdb78070b4c55a" ? "OK" : "FAIL");

    // 2) 周次解析
    auto weeks = hbut::parse_weeks("1-16周(单)");
    printf("weeks=%zu first=%d last=%d\n", weeks.size(), weeks.empty() ? -1 : weeks.front(),
           weeks.empty() ? -1 : weeks.back());
    auto weeks2 = hbut::parse_weeks("3,5,7,9-11周");
    printf("weeks2=%zu (%d %d %d ...)\n", weeks2.size(), weeks2[0], weeks2[1], weeks2[2]);

    // 3) 课表出图（假数据，检查排版）
    jw::TimetableImage img;
    img.semester_label = "2026-2027学年 第一学期";
    img.week = 7;
    img.total_weeks = 20;
    img.date_range = "10月19日 - 10月25日";
    img.student_line = "张三 · 20241001";
    img.footer = "数据来自 HBUT 教务系统 · 生成于 2026-10-21 15:40 · TabMind hbut_jw";
    img.today_weekday = 2;

    jw::Date mon;
    mon.y = 2026; mon.m = 10; mon.d = 19;
    for (int i = 0; i < 7; ++i) img.day_dates.push_back(jw::add_days(mon, i));

    auto add = [&img](const char* name, const char* teacher, const char* room, int wd, int period,
                      int count) {
        hbut::Course c;
        c.name = name;
        c.teacher = teacher;
        c.room = room;
        c.weekday = wd;
        c.period = period;
        c.count = count;
        img.courses.push_back(c);
    };
    add("高等数学A(上)", "李明", "1教-301", 1, 1, 2);
    add("大学物理", "王芳", "2教-105", 1, 5, 2);
    add("程序设计基础", "张伟", "3教-机房A", 2, 1, 2);
    add("线性代数", "赵磊", "1教-202", 2, 7, 2);
    add("大学生英语视听说", "Alice", "外语楼-305", 3, 3, 2);
    add("体育（篮球）", "陈强", "东区篮球场", 3, 9, 2);
    add("数据结构", "刘洋", "3教-401", 4, 1, 2);
    add("马克思主义基本原理", "孙丽", "5教-108", 4, 5, 2);
    add("电路分析基础", "周涛", "2教-208", 5, 3, 3);
    add("习近平新时代中国特色社会主义思想概论", "何静", "5教-201", 5, 8, 3);
    add("创新创业实践", "客座讲师", "双创大楼-A", 6, 2, 4);

    std::string err;
    bool ok = jw::render_timetable_png(img, "tests/preview.png", &err);
    printf("render %s %s\n", ok ? "OK" : "FAIL", err.c_str());

    // 4) 学期推算
    jw::Date d;
    d.y = 2026; d.m = 10; d.d = 21;
    printf("semester=%s label=%s\n", jw::semester_by_date(d).c_str(),
           jw::semester_label("2026-2027-1").c_str());
    jw::gdiplus_shutdown();
    return 0;
}
