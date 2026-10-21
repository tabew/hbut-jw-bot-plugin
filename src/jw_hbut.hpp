// jw_hbut.hpp —— 湖北工业大学教务系统业务层
//   1) 学习通（超星）扫码登录 → hbut.jw.chaoxing.com
//   2) 统一身份认证 CAS 账号密码登录 → jwxt.hbut.edu.cn
//   3) 课表 / 校历（周次日期）查询
#pragma once
#include <string>
#include <vector>
#include <map>

#include "jw_http.hpp"
#include "jw_util.hpp"

namespace hbut {

constexpr const char* kAuthBase = "https://auth.hbut.edu.cn/authserver";
constexpr const char* kJwxtBase = "https://jwxt.hbut.edu.cn";
constexpr const char* kChaoxingJwBase = "https://hbut.jw.chaoxing.com";
constexpr const char* kChaoxingPassport = "https://passport2.chaoxing.com";
constexpr const char* kJwxtCasService = "https://jwxt.hbut.edu.cn/admin/caslogin";

// ---------------- 数据结构 ----------------

struct Course {
    std::string name;
    std::string teacher;
    std::string room;
    std::string building;
    std::string class_name;
    int weekday = 1;   // 1..7
    int period = 1;    // 起始小节
    int count = 2;     // 连上小节数
    std::vector<int> weeks;
};

struct WeekSpan {
    int week = 1;
    jw::Date monday;
    jw::Date sunday;
};

struct TermCalendar {
    std::vector<WeekSpan> weeks;   // 按 week 升序
    int current_week = 0;
    int total_weeks = 0;
    bool from_school = false;      // true = 来自教务校历接口，false = 按日期推算
};

// 一个 QQ 用户的教务会话
struct Session {
    std::string base = kJwxtBase;             // 当前应使用的教务域名
    std::string student_id;
    std::string name;
    std::string xhid;
    std::string semester;                     // 2026-2027-1
    bool chaoxing = false;                    // 是否学习通链路
    jw::CookieJar jar;
};

// ---------------- 学习通扫码 ----------------

struct QrTicket {
    std::string uuid;
    std::string enc;
    std::string refer;
};

// 申请二维码（同时把登录页 Cookie 收入 hc.jar）
bool chaoxing_qr_create(jw::HttpClient& hc, QrTicket* out, std::string* err);

// 拉二维码图片（PNG 原始字节）
bool chaoxing_qr_image(jw::HttpClient& hc, const std::string& uuid, std::string* png, std::string* err);

enum class QrState { Waiting, Scanned, Confirmed, Expired, Failed };

// 轮询扫码状态；message 用于给出“已扫码待确认”等提示
QrState chaoxing_qr_poll(jw::HttpClient& hc, const QrTicket& tk, std::string* message);

// 扫码通过后，把学习通票据换成教务会话（访问 hbut.jw.chaoxing.com）
bool chaoxing_bootstrap(jw::HttpClient& hc, std::string* err);

// ---------------- 统一身份认证（账号密码） ----------------

struct CasPage {
    std::string execution;
    std::string lt;
    std::string salt;
    std::vector<std::pair<std::string, std::string>> hidden; // 页面上所有 hidden input
    bool logged_in = false;
    bool need_captcha = false;
};

bool cas_begin(jw::HttpClient& hc, const std::string& service, CasPage* page, std::string* err);

// 拉验证码图片（返回值可能是 PNG 或 JPEG 原始字节）
bool cas_captcha(jw::HttpClient& hc, std::string* image, bool* is_jpeg, std::string* err);

enum class CasResult { Ok, NeedCaptcha, Fail };

CasResult cas_submit(jw::HttpClient& hc, const CasPage& page, const std::string& user,
                     const std::string& password, const std::string& captcha,
                     const std::string& service, std::string* err);

// ---------------- 课表 ----------------

// 取学号内部 ID（xhid）。同时会判断会话是否过期。
bool fetch_xhid(jw::HttpClient& hc, const std::string& base, const std::string& semester,
                std::string* xhid, std::string* err, bool* session_expired);

bool fetch_schedule(jw::HttpClient& hc, const std::string& base, const std::string& semester,
                    const std::string& xhid, std::vector<Course>* out, std::string* err,
                    bool* session_expired);

bool fetch_calendar(jw::HttpClient& hc, const std::string& base, const std::string& semester,
                    TermCalendar* out, std::string* err);

// "1-16周(单)" / "3,5,7周" / "1-16周" → 周次列表
std::vector<int> parse_weeks(const std::string& text);

// 从课表课程推断当前周次（校历拿不到时的兜底）
int infer_current_week(const std::vector<WeekSpan>& weeks);

// 教务返回的学期（校历行里的 xnxq）优先，其次按日期推算
std::string resolve_semester(const std::string& hint);

// 判断会话是否还有效（用于 /课表 状态）
bool probe_session(jw::HttpClient& hc, const std::string& base);

} // namespace hbut
