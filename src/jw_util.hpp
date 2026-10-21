// jw_util.hpp —— 通用工具：编码、HTML、日期、学期、文件
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace jw {

// ---------------- 字符串 / 编码 ----------------
std::wstring wfromu8(const std::string& s);
std::string  u8fromw(const std::wstring& s);

std::string url_encode(const std::string& s);
std::string url_decode(const std::string& s);

std::string base64_encode(const std::string& in);
std::string base64_decode(const std::string& in);

std::string random_string(size_t n);

std::string to_lower(std::string s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
bool contains(const std::string& s, const std::string& p);

std::string trim(const std::string& s);
std::string replace_all(const std::string& s, const std::string& from, const std::string& to);
std::string strip_html(const std::string& s);      // 去掉 <span> 之类标签
std::string html_unescape(const std::string& s);   // &nbsp; &amp; 等
std::string plain_text(const std::string& s);      // strip_html + html_unescape

std::vector<std::string> split(const std::string& s, char sep);

// 取 prefix 与 suffix 之间的第一段内容（用于从 HTML / JSON 里抠字段）
bool between(const std::string& text, const std::string& prefix, const std::string& suffix,
             std::string* out);

// ---------------- 日期 ----------------
struct Date {
    int y = 0, m = 0, d = 0;
    bool valid() const { return y > 1900 && m >= 1 && m <= 12 && d >= 1 && d <= 31; }
};

Date today_local();
int  weekday_mon0(const Date& dt);              // 0=周一 … 6=周日
Date add_days(const Date& dt, int days);
int  days_between(const Date& a, const Date& b); // b - a
bool parse_date(const std::string& text, Date* out); // 支持 2026-09-07 / 2026/9/7 / 带时间
std::string fmt_date(const Date& dt);            // 2026-09-07
std::string fmt_md(const Date& dt);              // 9月7日
long long now_ms();

// ---------------- 学期 ----------------
std::string semester_by_date(const Date& dt);   // 2026-2027-1
bool semester_valid(const std::string& s);      // YYYY-YYYY-N
std::string semester_label(const std::string& s); // 2026-2027学年 第1学期

// ---------------- 文件 ----------------
bool write_file_binary(const std::string& path, const std::string& data);
std::string read_file_text(const std::string& path);
bool file_exists(const std::string& path);
bool make_dirs(const std::string& path);
std::string exe_dir();                 // 可执行文件所在目录
std::string data_dir(const std::string& sub); // <exe>/data/<sub>，自动创建

} // namespace jw
