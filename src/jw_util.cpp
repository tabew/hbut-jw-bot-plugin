#include "jw_util.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <random>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace jw {

// ---------------- 字符串 / 编码 ----------------

std::wstring wfromu8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::string u8fromw(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len, nullptr, nullptr);
    return out;
}

static bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (is_unreserved(c)) {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex_val(s[i + 1]), l = hex_val(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back((char)((h << 4) | l));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') { out.push_back(' '); continue; }
        out.push_back(s[i]);
    }
    return out;
}

static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned a = (unsigned char)in[i], b = (unsigned char)in[i + 1], c = (unsigned char)in[i + 2];
        out.push_back(kB64[a >> 2]);
        out.push_back(kB64[((a & 0x3) << 4) | (b >> 4)]);
        out.push_back(kB64[((b & 0xF) << 2) | (c >> 6)]);
        out.push_back(kB64[c & 0x3F]);
        i += 3;
    }
    size_t rest = in.size() - i;
    if (rest == 1) {
        unsigned a = (unsigned char)in[i];
        out.push_back(kB64[a >> 2]);
        out.push_back(kB64[(a & 0x3) << 4]);
        out += "==";
    } else if (rest == 2) {
        unsigned a = (unsigned char)in[i], b = (unsigned char)in[i + 1];
        out.push_back(kB64[a >> 2]);
        out.push_back(kB64[((a & 0x3) << 4) | (b >> 4)]);
        out.push_back(kB64[(b & 0xF) << 2]);
        out.push_back('=');
    }
    return out;
}

std::string base64_decode(const std::string& in) {
    std::string out;
    int table[256];
    for (int i = 0; i < 256; ++i) table[i] = -1;
    for (int i = 0; i < 64; ++i) table[(unsigned char)kB64[i]] = i;

    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = table[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

std::string random_string(size_t n) {
    static const char* cs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::mt19937 rng((unsigned)std::random_device{}() ^ (unsigned)GetTickCount64());
    std::uniform_int_distribution<int> dist(0, 61);
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(cs[dist(rng)]);
    return out;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool contains(const std::string& s, const std::string& p) {
    return s.find(p) != std::string::npos;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

std::string replace_all(const std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    size_t pos = 0;
    while (true) {
        size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            break;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
    return out;
}

std::string strip_html(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

std::string html_unescape(const std::string& s) {
    std::string out = s;
    out = replace_all(out, "&nbsp;", " ");
    out = replace_all(out, "&amp;", "&");
    out = replace_all(out, "&lt;", "<");
    out = replace_all(out, "&gt;", ">");
    out = replace_all(out, "&quot;", "\"");
    out = replace_all(out, "&#39;", "'");
    out = replace_all(out, "\\r", " ");
    out = replace_all(out, "\\n", " ");
    return out;
}

std::string plain_text(const std::string& s) {
    return trim(html_unescape(strip_html(s)));
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + 1;
    }
    return out;
}

bool between(const std::string& text, const std::string& prefix, const std::string& suffix,
             std::string* out) {
    if (prefix.empty()) return false;
    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        size_t cs = pos + prefix.size();
        size_t ce = text.find(suffix, cs);
        if (ce == std::string::npos) return false;
        if (ce > cs) {
            if (out) *out = text.substr(cs, ce - cs);
            return true;
        }
        pos = cs; // 空内容，继续往后找
    }
    return false;
}

// ---------------- 日期 ----------------

Date today_local() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    Date d;
    d.y = st.wYear;
    d.m = st.wMonth;
    d.d = st.wDay;
    return d;
}

static std::time_t to_time_t(const Date& dt) {
    std::tm t{};
    t.tm_year = dt.y - 1900;
    t.tm_mon = dt.m - 1;
    t.tm_mday = dt.d;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    return std::mktime(&t);
}

static Date from_time_t(std::time_t tt) {
    std::tm t{};
    localtime_s(&t, &tt);
    Date d;
    d.y = t.tm_year + 1900;
    d.m = t.tm_mon + 1;
    d.d = t.tm_mday;
    return d;
}

int weekday_mon0(const Date& dt) {
    std::time_t tt = to_time_t(dt);
    std::tm t{};
    localtime_s(&t, &tt);
    // tm_wday: 0=周日
    return (t.tm_wday + 6) % 7;
}

Date add_days(const Date& dt, int days) {
    return from_time_t(to_time_t(dt) + (std::time_t)days * 86400);
}

int days_between(const Date& a, const Date& b) {
    return (int)((to_time_t(b) - to_time_t(a)) / 86400);
}

bool parse_date(const std::string& text, Date* out) {
    if (!out) return false;
    std::string s = trim(text);
    if (s.size() < 8) return false;
    // 去掉时间部分
    size_t sp = s.find_first_of(" T");
    if (sp != std::string::npos) s = s.substr(0, sp);
    int y = 0, m = 0, d = 0;
    if (sscanf_s(s.c_str(), "%d-%d-%d", &y, &m, &d) == 3 ||
        sscanf_s(s.c_str(), "%d/%d/%d", &y, &m, &d) == 3 ||
        sscanf_s(s.c_str(), "%d.%d.%d", &y, &m, &d) == 3) {
        Date dt;
        dt.y = y; dt.m = m; dt.d = d;
        if (dt.valid()) { *out = dt; return true; }
    }
    return false;
}

std::string fmt_date(const Date& dt) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.y, dt.m, dt.d);
    return std::string(buf);
}

std::string fmt_md(const Date& dt) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d月%d日", dt.m, dt.d);
    return std::string(buf);
}

long long now_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (long long)(uli.QuadPart / 10000ULL - 11644473600000ULL);
}

// ---------------- 学期 ----------------

std::string semester_by_date(const Date& dt) {
    auto mk = [](int y, int term) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d-%d-%d", y, y + 1, term);
        return std::string(buf);
    };
    if (dt.m >= 9) return mk(dt.y, 1);
    if (dt.m >= 3) return mk(dt.y - 1, 2);
    if (dt.m == 2 && dt.d >= 15) return mk(dt.y - 1, 2);
    return mk(dt.y - 1, 1); // 1 月 / 2 月上旬仍属秋季学期
}

bool semester_valid(const std::string& s) {
    auto parts = split(trim(s), '-');
    if (parts.size() != 3) return false;
    auto digits = [](const std::string& v, size_t n) {
        if (v.size() != n) return false;
        for (char c : v) if (c < '0' || c > '9') return false;
        return true;
    };
    if (!digits(parts[0], 4) || !digits(parts[1], 4)) return false;
    return parts[2] == "1" || parts[2] == "2";
}

std::string semester_label(const std::string& s) {
    auto parts = split(s, '-');
    if (parts.size() != 3) return s;
    return parts[0] + "-" + parts[1] + "学年 第" + (parts[2] == "1" ? "一" : "二") + "学期";
}

// ---------------- 文件 ----------------

bool write_file_binary(const std::string& path, const std::string& data) {
    std::ofstream ofs(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (!data.empty()) ofs.write(data.data(), (std::streamsize)data.size());
    return ofs.good();
}

std::string read_file_text(const std::string& path) {
    std::ifstream ifs(fs::u8path(path), std::ios::binary);
    if (!ifs) return std::string();
    std::string out((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return out;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::u8path(path), ec);
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path), ec);
    return !ec;
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0) return std::string(".");
    std::wstring full(buf, n);
    size_t pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::string(".");
    return u8fromw(full.substr(0, pos));
}

std::string data_dir(const std::string& sub) {
    std::string base = exe_dir() + "/data";
    make_dirs(base);
    if (sub.empty()) return base;
    std::string full = base + "/" + sub;
    make_dirs(full);
    return full;
}

} // namespace jw
