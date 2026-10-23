// hbut_jw —— 湖北工业大学教务插件（TabMind）
//   /课表                 本周课表图片
//   /课表 5               第 5 周课表
//   /课表 扫码            学习通扫码登录
//   /课表 登录 学号 密码   统一身份认证登录（可能需要验证码）
//   /课表 验证码 xxxx
//   /课表 状态 / 学期 2026-2027-1 / 退出
#include "plugin_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jw_hbut.hpp"
#include "jw_http.hpp"
#include "jw_local.hpp"
#include "jw_render.hpp"
#include "jw_util.hpp"
#include "simple_json.hpp"

using namespace jw;

namespace {

std::atomic<bool> g_unloading{false};
std::atomic<int> g_active{0};

struct ThreadGuard {
    ThreadGuard() { ++g_active; }
    ~ThreadGuard() { --g_active; }
};

// ---------------- 异步回发 ----------------

struct Notifier {
    void* ud = nullptr;
    PluginAsyncSendReplyFn send = nullptr;
    PluginAsyncLogFn log_fn = nullptr;
    std::string type;
    int64_t target = 0;

    void say(const std::string& text) const {
        if (!g_unloading && send) send(ud, type.c_str(), target, text.c_str());
    }
    void note(const std::string& text) const {
        if (!g_unloading && log_fn) log_fn(ud, text.c_str());
    }
};

Notifier make_notifier(const PluginContext& ctx) {
    Notifier n;
    n.ud = ctx.async_user_data;
    n.send = ctx.async_send_reply;
    n.log_fn = ctx.async_log;
    n.type = ctx.message_type;
    n.target = ctx.message_type == "group" ? ctx.group_id : ctx.user_id;
    return n;
}

std::string now_text() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute);
    return std::string(buf);
}

// ---------------- 会话与「等验证码」状态 ----------------

struct PendingCaptcha {
    hbut::CasPage page;
    jw::CookieJar jar;
    std::string user;
    std::string pass;
    std::string service;
    std::string image_path;
    long long expire_ms = 0;
};

class Store {
public:
    static Store& get() {
        static Store inst;
        return inst;
    }

    bool get_session(int64_t qq, hbut::Session* out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(qq);
        if (it == sessions_.end()) return false;
        if (out) *out = it->second;
        return true;
    }

    void put_session(int64_t qq, const hbut::Session& s) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            sessions_[qq] = s;
        }
        persist(qq, s);
    }

    void drop_session(int64_t qq) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            sessions_.erase(qq);
            pending_.erase(qq);
        }
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(session_path(qq)), ec);
    }

    void put_pending(int64_t qq, const PendingCaptcha& p) {
        std::lock_guard<std::mutex> lk(mu_);
        pending_[qq] = p;
    }

    bool take_pending(int64_t qq, PendingCaptcha* out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_.find(qq);
        if (it == pending_.end()) return false;
        if (it->second.expire_ms > 0 && it->second.expire_ms < now_ms()) {
            pending_.erase(it);
            return false;
        }
        if (out) *out = it->second;
        pending_.erase(it);
        return true;
    }

    void load_all() {
        std::string dir = data_dir("hbut_jw/users");
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::u8path(dir), ec)) return;
        for (auto& entry : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().stem().u8string();
            int64_t qq = atoll(name.c_str());
            if (qq <= 0) continue;
            auto v = simplejson::Value::parse(read_file_text(entry.path().u8string()));
            if (!v.is_object()) continue;
            hbut::Session s;
            s.base = v["base"].get<std::string>();
            if (s.base.empty()) s.base = hbut::kJwxtBase;
            s.student_id = v["sid"].get<std::string>();
            s.name = v["name"].get<std::string>();
            s.xhid = v["xhid"].get<std::string>();
            s.semester = v["sem"].get<std::string>();
            s.chaoxing = v["cx"].get<int>() != 0;
            s.jar.from_json(v["jar"].dump(-1));
            if (s.jar.empty()) continue;
            std::lock_guard<std::mutex> lk(mu_);
            sessions_[qq] = s;
        }
    }

private:
    Store() = default;

    static std::string session_path(int64_t qq) {
        return data_dir("hbut_jw/users") + "/" + std::to_string(qq) + ".json";
    }

    void persist(int64_t qq, const hbut::Session& s) {
        simplejson::Value v;
        v["base"] = s.base;
        v["sid"] = s.student_id;
        v["name"] = s.name;
        v["xhid"] = s.xhid;
        v["sem"] = s.semester;
        v["cx"] = s.chaoxing ? 1 : 0;
        v["jar"] = simplejson::Value::parse(s.jar.to_json());

        // 先写临时文件再改名，避免进程被杀时读到半截 Cookie
        std::string path = session_path(qq);
        std::string tmp = path + ".tmp";
        if (write_file_binary(tmp, v.dump(-1))) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path), ec); // Windows 下 rename 不覆盖
            std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
            if (ec) std::filesystem::remove(std::filesystem::u8path(tmp), ec);
        }
    }

    std::mutex mu_;
    std::map<int64_t, hbut::Session> sessions_;
    std::map<int64_t, PendingCaptcha> pending_;
};

// ---------------- 课表抓取与出图 ----------------

struct WeekResult {
    hbut::TermCalendar cal;
    std::vector<hbut::Course> courses;
    int week = 1;
};

bool load_week(hbut::Session& s, const UserEdits& edits, int want_week, WeekResult* out,
               std::string* err, bool* expired) {
    *expired = false;
    HttpClient hc;
    hc.jar = s.jar;

    std::string sem = hbut::resolve_semester(s.semester);
    s.semester = sem;

    if (s.xhid.empty()) {
        std::string e;
        bool exp = false;
        if (!hbut::fetch_xhid(hc, s.base, sem, &s.xhid, &e, &exp)) {
            if (exp) *expired = true;
            *err = e;
            return false;
        }
    }

    if (!hbut::fetch_calendar(hc, s.base, sem, &out->cal, nullptr)) {
        *err = "校历获取失败";
        return false;
    }

    std::string e;
    bool exp = false;
    std::vector<hbut::Course> raw;
    if (!hbut::fetch_schedule(hc, s.base, sem, s.xhid, &raw, &e, &exp)) {
        if (exp) *expired = true;
        *err = e;
        return false;
    }
    s.jar = hc.jar;

    out->courses = apply_edits(raw, edits); // 先套本地改写，再按周过滤

    int week = want_week > 0 ? want_week : out->cal.current_week;
    if (week < 1) week = 1;
    if (out->cal.total_weeks > 0 && week > out->cal.total_weeks) {
        *err = "第 " + std::to_string(week) + " 周超出本学期范围（共 " +
               std::to_string(out->cal.total_weeks) + " 周）";
        return false;
    }
    out->week = week;

    std::vector<hbut::Course> filtered;
    for (const auto& c : out->courses) {
        if (c.weeks.empty() || std::find(c.weeks.begin(), c.weeks.end(), week) != c.weeks.end()) {
            filtered.push_back(c);
        }
    }
    out->courses.swap(filtered);
    return true;
}

// 只拉教务原始课程（不套本地改写、不按周过滤），编辑指令用
bool load_raw_courses(hbut::Session& s, std::vector<hbut::Course>* out, std::string* err,
                      bool* expired) {
    *expired = false;
    HttpClient hc;
    hc.jar = s.jar;
    std::string sem = hbut::resolve_semester(s.semester);
    s.semester = sem;

    if (s.xhid.empty()) {
        std::string e;
        bool exp = false;
        if (!hbut::fetch_xhid(hc, s.base, sem, &s.xhid, &e, &exp)) {
            if (exp) *expired = true;
            *err = e;
            return false;
        }
    }
    std::string e;
    bool exp = false;
    if (!hbut::fetch_schedule(hc, s.base, sem, s.xhid, out, &e, &exp)) {
        if (exp) *expired = true;
        *err = e;
        return false;
    }
    s.jar = hc.jar;
    return true;
}

std::string render_for(const hbut::Session& s, const WeekResult& wr, int64_t qq, std::string* err) {
    TimetableImage img;
    img.semester_label = semester_label(s.semester);
    img.week = wr.week;
    img.total_weeks = wr.cal.total_weeks;
    img.courses = wr.courses;
    img.period_times = load_period_times();

    const hbut::WeekSpan* span = nullptr;
    for (const auto& w : wr.cal.weeks) {
        if (w.week == wr.week) {
            span = &w;
            break;
        }
    }
    if (span) {
        img.date_range = fmt_md(span->monday) + " - " + fmt_md(span->sunday);
        for (int i = 0; i < 7; ++i) img.day_dates.push_back(add_days(span->monday, i));
        Date today = today_local();
        if (days_between(span->monday, today) >= 0 && days_between(today, span->sunday) >= 0) {
            img.today_weekday = weekday_mon0(today);
        }
    }

    std::string who = s.name;
    if (!s.student_id.empty()) {
        if (!who.empty()) who += " · ";
        who += s.student_id;
    }
    img.student_line = who;
    img.footer = "数据来自 HBUT 教务系统 · 生成于 " + now_text() + " · TabMind hbut_jw";

    std::string out_dir = data_dir("hbut_jw/out");
    std::string path = out_dir + "/kb_" + std::to_string(qq) + "_w" + std::to_string(wr.week) + "_" +
                       std::to_string(now_ms() % 1000000) + ".png";
    if (!render_timetable_png(img, path, err)) return std::string();
    return path;
}

// ---------------- 插件本体 ----------------

class HbutJwPlugin : public IBotPlugin {
public:
    PluginInfo get_info() override {
        PluginInfo info;
        info.name = "湖工大教务";
        info.version = "1.0.0";
        info.author = "tabew";
        info.description =
            "HBUT 教务课表：/课表 出本周图片，/课表 5 指定周；支持学习通扫码与统一身份认证登录";
        info.enabled = true;
        return info;
    }

    bool on_load() override {
        data_dir("hbut_jw/users");
        data_dir("hbut_jw/out");
        data_dir("hbut_jw/tmp");
        if (!gdiplus_init()) return false;
        Store::get().load_all();
        return true;
    }

    void on_unload() override {
        g_unloading = true;
        for (int i = 0; i < 50 && g_active.load() > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        gdiplus_shutdown();
    }

    bool on_message(const PluginContext& ctx) override {
        try {
            std::string args;
            if (!match_command(ctx.plain_text, &args)) return false;
            dispatch(ctx, args);
            return true;
        } catch (...) {
            if (ctx.log) ctx.log("[教务] 处理消息时发生异常，已忽略");
            return true;
        }
    }

private:
    // ---- 指令识别 ----
    static bool match_command(const std::string& plain, std::string* args) {
        std::string t = trim(plain);
        static const char* kPrefix[] = {"/课表", "/课程表", "/kb", "课表", "课程表"};
        for (const char* p : kPrefix) {
            std::string prefix(p);
            if (t.size() < prefix.size() || t.compare(0, prefix.size(), prefix) != 0) continue;
            std::string rest = t.substr(prefix.size());
            if (!rest.empty()) {
                unsigned char c0 = (unsigned char)rest[0];
                if (c0 >= 0x80) continue; // 「课表app」之类不是指令
                if (c0 != ' ' && c0 != '\t' && c0 != '/' && !isdigit(c0)) continue;
            }
            std::string a = trim(rest);
            if (!a.empty() && a[0] == '/') a = trim(a.substr(1));
            *args = a;
            return true;
        }
        return false;
    }

    static bool starts(const std::string& s, const char* kw) {
        return s.compare(0, strlen(kw), kw) == 0;
    }

    static int pick_week(const std::string& args) {
        std::string digits;
        for (char c : args) {
            if (isdigit((unsigned char)c)) digits.push_back(c);
            else if (!digits.empty()) break;
        }
        if (digits.empty()) return 0;
        return atoi(digits.c_str());
    }

    static std::vector<std::string> split_fields(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static std::string help_text() {
        return "【湖工大教务课表】\n"
               "• /课表                 本周课表图片\n"
               "• /课表 5               第 5 周课表\n"
               "• /课表 扫码            学习通扫码登录（私聊）\n"
               "• /课表 登录 学号 密码   统一身份认证登录（私聊）\n"
               "• /课表 验证码 1234      需要验证码时补填\n"
               "• /课表 课程            列出课程编号\n"
               "• /课表 改 1 教室=1教-301 教师=李明   改课程信息\n"
               "• /课表 加 名称=自习 星期=3 节次=4 连堂=1 周数=1-16\n"
               "• /课表 删 1            隐藏某门课\n"
               "• /课表 还原            清除本地改动\n"
               "• /课表 时间            查看/修改每节作息时间\n"
               "• /课表 状态 / 诊断 / 学期 2026-2027-1 / 退出\n"
               "密码不会被保存，只保留教务系统的 Cookie 会话。";
    }

    // ---- 指令分发 ----
    static void dispatch(const PluginContext& ctx, const std::string& args) {
        Notifier n = make_notifier(ctx);

        if (args.empty()) {
            start_week(ctx, n, 0);
            return;
        }
        if (starts(args, "帮助") || starts(args, "help") || starts(args, "?") || starts(args, "？")) {
            n.say(help_text());
            return;
        }
        if (starts(args, "扫码") || starts(args, "qr") || starts(args, "QR") || starts(args, "二维码")) {
            if (ctx.message_type == "group") {
                n.say("扫码登录涉及个人账号，请私聊我发送「/课表 扫码」。");
                return;
            }
            start_qr_login(ctx, n);
            return;
        }
        if (starts(args, "登录") || starts(args, "登陆")) {
            auto parts = split_fields(trim(args.substr(6))); // 「登录」/「登陆」都是 6 字节
            if (parts.size() < 2) {
                n.say("用法：/课表 登录 学号 密码\n"
                      "（密码不会被保存，只保留 Cookie 会话）\n"
                      "也可以私聊发送「/课表 扫码」，用学习通扫码登录。");
                return;
            }
            if (ctx.message_type == "group") {
                n.say("群聊里发学号密码会泄露隐私，请私聊我登录。");
                return;
            }
            start_pass_login(ctx, n, parts[0], parts[1]);
            return;
        }
        if (starts(args, "验证码")) {
            std::string code = trim(args.substr(9)); // 「验证码」= 9 字节
            if (code.empty()) {
                n.say("用法：/课表 验证码 1234");
                return;
            }
            submit_captcha(ctx, n, code);
            return;
        }
        if (starts(args, "退出") || starts(args, "注销") || starts(args, "登出")) {
            Store::get().drop_session(ctx.user_id);
            n.say("已退出教务登录，本地会话已清除。");
            return;
        }
        if (starts(args, "状态")) {
            show_status(ctx, n);
            return;
        }
        if (starts(args, "诊断") || starts(args, "debug")) {
            show_diag(ctx, n);
            return;
        }
        if (starts(args, "学期")) {
            std::string sem = trim(args.substr(6)); // 「学期」= 6 字节
            if (!semester_valid(sem)) {
                n.say("用法：/课表 学期 2026-2027-1");
                return;
            }
            hbut::Session s;
            if (!Store::get().get_session(ctx.user_id, &s)) {
                n.say("还没有登录，先发「/课表 扫码」或「/课表 登录 学号 密码」。");
                return;
            }
            s.semester = sem;
            s.xhid.clear();
            Store::get().put_session(ctx.user_id, s);
            n.say("已切换到 " + semester_label(sem) + "，再发「/课表」看看。");
            return;
        }
        if (starts(args, "刷新")) {
            start_week(ctx, n, 0);
            return;
        }
        if (starts(args, "课程") || starts(args, "列表") || starts(args, "课程列表")) {
            show_courses(ctx, n);
            return;
        }
        if (starts(args, "改") || starts(args, "修改") || starts(args, "编辑")) {
            std::string rest = starts(args, "改") ? trim(args.substr(3)) : trim(args.substr(6));
            edit_course(ctx, n, rest);
            return;
        }
        if (starts(args, "加") || starts(args, "新增") || starts(args, "添加")) {
            std::string rest = starts(args, "加") ? trim(args.substr(3)) : trim(args.substr(6));
            add_course(ctx, n, rest);
            return;
        }
        if (starts(args, "删") || starts(args, "隐藏")) {
            std::string rest = starts(args, "删") ? trim(args.substr(3)) : trim(args.substr(6));
            remove_course(ctx, n, rest);
            return;
        }
        if (starts(args, "还原") || starts(args, "重置修改")) {
            clear_user_edits(ctx.user_id);
            n.say("已清除本地的课程改动（改名/隐藏/自定义课程），再发「/课表」就是教务原始课表。");
            return;
        }
        if (starts(args, "时间") || starts(args, "作息")) {
            std::string rest = trim(args.substr(6));
            edit_times(n, rest);
            return;
        }

        int week = pick_week(args);
        if (week > 0) {
            start_week(ctx, n, week);
            return;
        }
        n.say("没看懂「" + args + "」这个指令。\n" + help_text());
    }

    // ---- /课表 [周] ----
    static void start_week(const PluginContext& ctx, Notifier n, int want_week) {
        hbut::Session s;
        if (!Store::get().get_session(ctx.user_id, &s)) {
            n.say("还没登录教务系统。\n私聊发送「/课表 扫码」用学习通扫码登录，\n"
                  "或「/课表 登录 学号 密码」用统一身份认证登录。");
            return;
        }
        n.say(want_week > 0 ? ("正在获取第 " + std::to_string(want_week) + " 周课表…")
                            : std::string("正在获取本周课表…"));

        UserEdits edits;
        load_user_edits(ctx.user_id, &edits);

        int64_t qq = ctx.user_id;
        std::thread([qq, s, edits, want_week, n]() {
            ThreadGuard guard;
            try {
                hbut::Session sess = s;
                WeekResult wr;
                std::string err;
                bool expired = false;
                if (!load_week(sess, edits, want_week, &wr, &err, &expired)) {
                    if (expired) {
                        Store::get().drop_session(qq);
                        n.say("教务会话已过期，请重新登录：\n私聊「/课表 扫码」或「/课表 登录 学号 密码」");
                    } else {
                        n.say("获取课表失败：" + err);
                    }
                    return;
                }
                Store::get().put_session(qq, sess);

                std::string rerr;
                std::string png = render_for(sess, wr, qq, &rerr);
                if (png.empty()) {
                    n.say("课表图片生成失败：" + rerr);
                    return;
                }
                std::string cap = "第 " + std::to_string(wr.week) + " 周课表";
                if (wr.cal.current_week == wr.week) cap += "（本周）";
                if (wr.courses.empty()) cap += " · 本周无课";
                if (!edits.empty()) cap += "\n· 已应用本地改动（/课表 还原 可清除）";
                if (!wr.cal.from_school) cap += "\n（校历接口不可用，周次按日期估算，可用「/课表 学期」确认）";
                n.say("[CQ:image,file=" + png + "]\n" + cap);
            } catch (const std::exception& e) {
                n.say(std::string("课表处理异常：") + e.what());
            } catch (...) {
                n.say("课表处理异常，请稍后再试。");
            }
        }).detach();
    }

    // ---- /课表 扫码（学习通） ----
    static void start_qr_login(const PluginContext& ctx, Notifier n) {
        int64_t qq = ctx.user_id;
        n.say("正在向学习通申请登录二维码…");
        std::thread([qq, n]() {
            ThreadGuard guard;
            try {
                HttpClient hc;
                hbut::QrTicket tk;
                std::string err;
                if (!hbut::chaoxing_qr_create(hc, &tk, &err)) {
                    n.say("申请二维码失败：" + err);
                    return;
                }
                std::string png;
                if (!hbut::chaoxing_qr_image(hc, tk.uuid, &png, &err)) {
                    n.say("二维码图片下载失败：" + err);
                    return;
                }
                std::string path = data_dir("hbut_jw/tmp") + "/qr_" + std::to_string(qq) + "_" +
                                   std::to_string(now_ms() % 1000000) + ".png";
                if (!write_file_binary(path, png)) {
                    n.say("二维码图片保存失败");
                    return;
                }
                n.say("[CQ:image,file=" + path +
                      "]\n打开「学习通」App → 右上角扫一扫 → 扫描上方二维码 → 手机上点确认。\n"
                      "二维码约 2 分钟内有效。");

                bool scanned_tip = false;
                hbut::QrState st = hbut::QrState::Waiting;
                std::string last_msg;
                for (int i = 0; i < 60 && !g_unloading; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
                    std::string msg;
                    st = hbut::chaoxing_qr_poll(hc, tk, &msg);
                    if (!msg.empty()) last_msg = msg;
                    if (st == hbut::QrState::Confirmed) break;
                    if (st == hbut::QrState::Expired) {
                        n.say("二维码已失效，请重新发送「/课表 扫码」。");
                        return;
                    }
                    if (st == hbut::QrState::Scanned && !scanned_tip) {
                        scanned_tip = true;
                        n.say("已扫码，请在手机上点「确认登录」…");
                    }
                }
                if (st != hbut::QrState::Confirmed) {
                    n.say("扫码等待超时，请重新发送「/课表 扫码」。" +
                          (last_msg.empty() ? std::string() : "\n（" + last_msg + "）"));
                    return;
                }

                n.note("[教务] 学习通扫码成功 qq=" + std::to_string(qq));
                std::string bridge_warn;
                bool jw_ready = hbut::chaoxing_bootstrap(hc, &bridge_warn);
                if (!jw_ready) n.note("[教务] 学习通→教务补票未完成：" + bridge_warn);

                hbut::Session s;
                s.base = hbut::kChaoxingJwBase;
                s.chaoxing = true;
                s.semester = hbut::resolve_semester("");
                s.jar = hc.jar;

                std::string e2;
                bool exp = false;
                if (!hbut::fetch_xhid(hc, s.base, s.semester, &s.xhid, &e2, &exp)) {
                    s.jar = hc.jar;
                    Store::get().put_session(qq, s);
                    n.say("学习通登录成功，但读取教务数据失败：" + e2 +
                          (jw_ready ? "" : "\n（补票提示：" + bridge_warn + "）") +
                          "\n可以稍后再发「/课表」试试，或改用「/课表 登录 学号 密码」。");
                    return;
                }
                s.jar = hc.jar;
                Store::get().put_session(qq, s);
                n.say("✅ 学习通登录成功！\n发送「/课表」查看本周课表。");
            } catch (const std::exception& e) {
                n.say(std::string("扫码登录异常：") + e.what());
            } catch (...) {
                n.say("扫码登录异常，请稍后再试。");
            }
        }).detach();
    }

    // ---- /课表 登录 学号 密码（统一身份认证） ----
    static void start_pass_login(const PluginContext& ctx, Notifier n, const std::string& user,
                                 const std::string& pass) {
        int64_t qq = ctx.user_id;
        n.say("正在登录统一身份认证…");
        std::thread([qq, user, pass, n]() {
            ThreadGuard guard;
            try {
                const std::string service = hbut::kJwxtCasService;
                HttpClient hc;
                hbut::CasPage page;
                std::string err;
                if (!hbut::cas_begin(hc, service, &page, &err)) {
                    n.say("登录失败：" + err);
                    return;
                }
                if (page.logged_in) {
                    finish_cas_login(hc, qq, user, n);
                    return;
                }
                // 湖工统一身份认证默认开验证码（页面上 captchaSwitch=1）
                if (page.need_captcha && ask_captcha(hc, qq, user, pass, service, n)) return;

                auto r = hbut::cas_submit(hc, page, user, pass, "", service, &err);
                if (r == hbut::CasResult::NeedCaptcha) {
                    ask_captcha(hc, qq, user, pass, service, n);
                    return;
                }
                if (r == hbut::CasResult::Fail) {
                    n.say("登录失败：" + (err.empty() ? std::string("账号或密码不正确") : err));
                    return;
                }
                finish_cas_login(hc, qq, user, n);
            } catch (const std::exception& e) {
                n.say(std::string("登录异常：") + e.what());
            } catch (...) {
                n.say("登录异常，请稍后再试。");
            }
        }).detach();
    }

    // 需要验证码：重新取一页（execution 一次性），拉验证码图并让用户补填
    static bool ask_captcha(HttpClient& hc, int64_t qq, const std::string& user,
                            const std::string& pass, const std::string& service, Notifier n) {
        hbut::CasPage p2;
        std::string err;
        if (!hbut::cas_begin(hc, service, &p2, &err)) {
            n.say("登录失败：" + err);
            return true; // 已回复用户，不再继续无验证码尝试
        }
        std::string img;
        bool jpeg = false;
        if (!hbut::cas_captcha(hc, &img, &jpeg, &err)) {
            // 拿不到验证码图：让调用方按「无验证码」再试一次
            n.note("[教务] 验证码获取失败，改为无验证码尝试：" + err);
            return false;
        }
        std::string path = data_dir("hbut_jw/tmp") + "/captcha_" + std::to_string(qq) + "_" +
                           std::to_string(now_ms() % 1000000) + (jpeg ? ".jpg" : ".png");
        if (!write_file_binary(path, img)) {
            n.note("[教务] 验证码图片保存失败");
            return false;
        }
        PendingCaptcha pc;
        pc.page = p2;
        pc.jar = hc.jar;
        pc.user = user;
        pc.pass = pass;
        pc.service = service;
        pc.image_path = path;
        pc.expire_ms = now_ms() + 180000;
        Store::get().put_pending(qq, pc);
        n.say("[CQ:image,file=" + path +
              "]\n教务系统要求输入验证码，请在 3 分钟内发送：\n/课表 验证码 图中的字符");
        return true;
    }

    // ---- /课表 验证码 xxxx ----
    static void submit_captcha(const PluginContext& ctx, Notifier n, const std::string& code) {
        int64_t qq = ctx.user_id;
        PendingCaptcha pc;
        if (!Store::get().take_pending(qq, &pc)) {
            n.say("当前没有待填验证码的登录（可能已超时），请重新「/课表 登录 学号 密码」。");
            return;
        }
        n.say("正在提交验证码…");
        std::thread([qq, pc, code, n]() {
            ThreadGuard guard;
            try {
                HttpClient hc;
                hc.jar = pc.jar;
                std::string err;
                auto r = hbut::cas_submit(hc, pc.page, pc.user, pc.pass, code, pc.service, &err);
                if (r == hbut::CasResult::NeedCaptcha) {
                    ask_captcha(hc, qq, pc.user, pc.pass, pc.service, n);
                    return;
                }
                if (r == hbut::CasResult::Fail) {
                    n.say("登录失败：" + (err.empty() ? std::string("验证码或密码不正确") : err));
                    return;
                }
                finish_cas_login(hc, qq, pc.user, n);
            } catch (const std::exception& e) {
                n.say(std::string("登录异常：") + e.what());
            } catch (...) {
                n.say("登录异常，请稍后再试。");
            }
        }).detach();
    }

    static void finish_cas_login(HttpClient& hc, int64_t qq, const std::string& user, Notifier n) {
        hbut::Session s;
        s.base = hbut::kJwxtBase;
        s.chaoxing = false;
        s.student_id = user;
        s.semester = hbut::resolve_semester("");

        std::string err;
        bool exp = false;
        if (!hbut::fetch_xhid(hc, s.base, s.semester, &s.xhid, &err, &exp)) {
            s.jar = hc.jar;
            Store::get().put_session(qq, s);
            n.say("登录流程已完成，但读取教务首页失败：" + err +
                  "\n可稍后发「/课表」验证，或改用「/课表 扫码」。");
            return;
        }
        s.jar = hc.jar;
        Store::get().put_session(qq, s);
        n.note("[教务] 账号密码登录成功 qq=" + std::to_string(qq) + " sid=" + user);
        n.say("✅ 登录成功（学号 " + user + "）\n发送「/课表」查看本周课表。");
    }

    // ---- 课程编辑：解析与展示 ----

    static const char* weekday_cn(int wd) {
        static const char* kNames[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
        if (wd < 1 || wd > 7) return "周?";
        return kNames[wd - 1];
    }

    static bool parse_kv(const std::string& token, std::string* key, std::string* value) {
        size_t pos = token.find_first_of("=:：");
        if (pos == std::string::npos || pos == 0) return false;
        *key = trim(token.substr(0, pos));
        *value = trim(token.substr(pos + 1));
        // 全角等号：一个字符 3 字节，substr 已按字节切开，这里只需去掉残留
        if (!key->empty() && (unsigned char)key->back() == 0xEF && key->size() >= 3) {
            key->erase(key->size() - 3);
        }
        return !key->empty();
    }

    static std::string norm_key(const std::string& raw) {
        std::string k = to_lower(trim(raw));
        if (k == "name" || k == "名称" || k == "课名" || k == "课程") return "name";
        if (k == "room" || k == "教室" || k == "地点" || k == "场地") return "room";
        if (k == "teacher" || k == "教师" || k == "老师") return "teacher";
        if (k == "weekday" || k == "星期" || k == "周几") return "weekday";
        if (k == "period" || k == "节次" || k == "开始节" || k == "起始节") return "period";
        if (k == "count" || k == "连堂" || k == "节数" || k == "长度") return "count";
        if (k == "weeks" || k == "周数" || k == "周次") return "weeks";
        if (k == "hidden" || k == "隐藏") return "hidden";
        return "";
    }

    static int parse_weekday_str(const std::string& value) {
        std::string v = trim(value);
        static const char* kCn[] = {"一", "二", "三", "四", "五", "六", "日"};
        for (int i = 0; i < 7; ++i) {
            if (v.find(kCn[i]) != std::string::npos) {
                // "周日"/"周天"/"星期日" 都归到 7
                if (i == 6) return 7;
                return i + 1;
            }
        }
        int n = atoi(v.c_str());
        if (n >= 1 && n <= 7) return n;
        return 0;
    }

    static bool is_all_digits(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) {
            if (!isdigit((unsigned char)c)) return false;
        }
        return true;
    }

    // ---- /课表 课程 ----
    static void show_courses(const PluginContext& ctx, Notifier n) {
        hbut::Session s;
        if (!Store::get().get_session(ctx.user_id, &s)) {
            n.say("还没登录教务系统，先「/课表 扫码」或「/课表 登录 学号 密码」。");
            return;
        }
        n.say("正在读取教务课程…");
        int64_t qq = ctx.user_id;
        std::thread([qq, s, n]() {
            ThreadGuard guard;
            try {
                hbut::Session sess = s;
                std::vector<hbut::Course> raw;
                std::string err;
                bool expired = false;
                if (!load_raw_courses(sess, &raw, &err, &expired)) {
                    if (expired) {
                        Store::get().drop_session(qq);
                        n.say("教务会话已过期，请重新登录。");
                    } else {
                        n.say("读取课程失败：" + err);
                    }
                    return;
                }
                Store::get().put_session(qq, sess);

                UserEdits edits;
                load_user_edits(qq, &edits);
                auto names = course_names(raw);
                if (names.empty()) {
                    n.say("教务里没有读到课程。");
                    return;
                }

                std::string out = "教务课程（共 " + std::to_string(names.size()) + " 门）\n";
                for (size_t i = 0; i < names.size(); ++i) {
                    const hbut::Course* sample = nullptr;
                    for (const auto& c : raw) {
                        if (c.name == names[i]) {
                            sample = &c;
                            break;
                        }
                    }
                    std::string line = std::to_string(i + 1) + ". " + names[i];
                    if (sample) {
                        int big = sample->big_section > 0 ? sample->big_section
                                                          : (sample->period + 1) / 2;
                        line += "  " + std::string(weekday_cn(sample->weekday)) + " 大节" +
                                std::to_string(big);
                        if (sample->count > 1) {
                            line += "-" + std::to_string(big + sample->count - 1);
                        }
                        if (!sample->room.empty()) line += "  " + sample->room;
                    }
                    for (const auto& e : edits.edits) {
                        if (e.match != names[i]) continue;
                        if (e.hidden) line += "  [已隐藏]";
                        else if (!e.name.empty()) line += "  [已改名→" + e.name + "]";
                        break;
                    }
                    out += line + "\n";
                }
                if (!edits.adds.empty()) {
                    out += "自定义课程：" + std::to_string(edits.adds.size()) + " 门\n";
                }
                out += "\n改：/课表 改 1 教室=1教-301 教师=李明\n"
                       "加：/课表 加 名称=自习 星期=3 节次=4 连堂=1 周数=1-16 教室=图书馆\n"
                       "删：/课表 删 1        还原：/课表 还原";
                n.say(out);
            } catch (...) {
                n.say("读取课程失败");
            }
        }).detach();
    }

    // 把 kv 列表套到 edit 上；返回处理了几个字段
    static int apply_kvs(const std::vector<std::pair<std::string, std::string>>& kvs, CourseEdit* e) {
        int applied = 0;
        for (const auto& kv : kvs) {
            std::string k = kv.first;
            const std::string& v = kv.second;
            if (k == "name") { e->name = v; applied++; }
            else if (k == "room") { e->room = v; applied++; }
            else if (k == "teacher") { e->teacher = v; applied++; }
            else if (k == "weekday") {
                int wd = parse_weekday_str(v);
                if (wd > 0) { e->weekday = wd; applied++; }
            } else if (k == "period") {
                int p = atoi(v.c_str());
                if (p > 0) { e->period = p; applied++; }
            } else if (k == "count") {
                int c = atoi(v.c_str());
                if (c > 0) { e->count = c; applied++; }
            } else if (k == "weeks") {
                e->weeks = v;
                applied++;
            } else if (k == "hidden") {
                e->hidden = (v == "1" || v == "true" || v == "是");
                applied++;
            }
        }
        return applied;
    }

    // ---- /课表 改 编号|课名 k=v ... ----
    static void edit_course(const PluginContext& ctx, Notifier n, const std::string& rest) {
        auto fields = split_fields(rest);
        if (fields.empty()) {
            n.say("用法：/课表 改 编号 教室=1教-301 教师=李明\n先用「/课表 课程」看编号。");
            return;
        }
        std::string sel = fields[0];
        std::vector<std::pair<std::string, std::string>> kvs;
        for (size_t i = 1; i < fields.size(); ++i) {
            std::string k, v;
            if (!parse_kv(fields[i], &k, &v)) continue;
            std::string nk = norm_key(k);
            if (nk.empty()) continue;
            kvs.push_back({nk, v});
        }
        if (kvs.empty()) {
            n.say("没识别到要改的字段。例如：/课表 改 1 教室=1教-301 教师=李明\n"
                  "可用字段：名称 / 教室 / 教师 / 星期 / 节次 / 连堂 / 周数 / 隐藏");
            return;
        }

        hbut::Session s;
        if (!Store::get().get_session(ctx.user_id, &s)) {
            n.say("还没登录教务系统，先「/课表 扫码」或「/课表 登录 学号 密码」。");
            return;
        }
        int64_t qq = ctx.user_id;
        n.say("正在应用修改…");
        std::thread([qq, s, sel, kvs, n]() {
            ThreadGuard guard;
            try {
                hbut::Session sess = s;
                std::vector<hbut::Course> raw;
                std::string err;
                bool expired = false;
                if (!load_raw_courses(sess, &raw, &err, &expired)) {
                    if (expired) {
                        Store::get().drop_session(qq);
                        n.say("教务会话已过期，请重新登录。");
                    } else {
                        n.say("读取课程失败：" + err);
                    }
                    return;
                }
                Store::get().put_session(qq, sess);

                auto names = course_names(raw);
                std::string match;
                if (is_all_digits(sel)) {
                    int idx = atoi(sel.c_str());
                    if (idx < 1 || idx > (int)names.size()) {
                        n.say("编号超出范围，发「/课表 课程」查看编号。");
                        return;
                    }
                    match = names[idx - 1];
                } else {
                    for (const auto& nm : names) {
                        if (nm == sel) {
                            match = nm;
                            break;
                        }
                    }
                    if (match.empty()) {
                        n.say("找不到课程「" + sel + "」，发「/课表 课程」查看准确课名与编号。");
                        return;
                    }
                }

                UserEdits edits;
                load_user_edits(qq, &edits);
                CourseEdit* target = find_or_add_edit(&edits, match);
                int applied = apply_kvs(kvs, target);
                if (applied == 0) {
                    n.say("没有可用的字段值，检查一下格式（如 教室=1教-301）。");
                    return;
                }
                if (!save_user_edits(qq, edits)) {
                    n.say("保存失败，检查 data/hbut_jw 目录权限。");
                    return;
                }

                std::string detail;
                if (!target->name.empty()) detail += "名称→" + target->name + " ";
                if (!target->room.empty()) detail += "教室→" + target->room + " ";
                if (!target->teacher.empty()) detail += "教师→" + target->teacher + " ";
                if (target->weekday > 0) detail += std::string("星期→") + weekday_cn(target->weekday) + " ";
                if (target->period > 0) detail += "节次→" + std::to_string(target->period) + " ";
                if (target->count > 0) detail += "连堂→" + std::to_string(target->count) + " ";
                if (!target->weeks.empty()) detail += "周数→" + target->weeks + " ";
                n.say("✅ 已修改「" + match + "」\n" + detail + "\n发「/课表」看效果。");
            } catch (...) {
                n.say("修改失败");
            }
        }).detach();
    }

    // ---- /课表 加 名称=xx 星期=3 节次=9 ----
    static void add_course(const PluginContext& ctx, Notifier n, const std::string& rest) {
        auto fields = split_fields(rest);
        if (fields.empty()) {
            n.say("用法：/课表 加 名称=自习 星期=3 节次=4 连堂=1 周数=1-16 教室=图书馆\n（节次=第几个大节 1~6，连堂=跨几个大节）");
            return;
        }
        CourseEdit tmp;
        std::vector<std::pair<std::string, std::string>> kvs;
        for (const auto& f : fields) {
            std::string k, v;
            if (!parse_kv(f, &k, &v)) continue;
            std::string nk = norm_key(k);
            if (nk.empty()) continue;
            kvs.push_back({nk, v});
        }
        apply_kvs(kvs, &tmp);
        if (tmp.name.empty()) {
            n.say("缺少课程名，例如：/课表 加 名称=自习 星期=3 节次=9");
            return;
        }
        if (tmp.weekday <= 0 || tmp.period <= 0) {
            n.say("需要指定 星期=1~7 与 节次=1~6（第几个大节），例如：/课表 加 名称=自习 星期=3 节次=4");
            return;
        }
        if (tmp.period > 7) {
            n.say("节次是大节号（1~6），不是小节。第 3 大节就写 节次=3。");
            return;
        }

        int64_t qq = ctx.user_id;
        UserEdits edits;
        load_user_edits(qq, &edits);

        hbut::Course c;
        c.name = tmp.name;
        c.room = tmp.room;
        c.teacher = tmp.teacher;
        c.weekday = tmp.weekday;
        c.big_section = tmp.period;                 // 用户填的「节次」就是第几个大节
        c.period = (tmp.period - 1) * 2 + 1;
        c.count = tmp.count > 0 ? tmp.count : 1;
        std::string weeks_text = tmp.weeks.empty() ? std::string("1-20") : tmp.weeks;
        c.weeks = hbut::parse_weeks(weeks_text);
        edits.adds.push_back(c);
        edits.add_weeks.push_back(weeks_text);
        if (!save_user_edits(qq, edits)) {
            n.say("保存失败，检查 data/hbut_jw 目录权限。");
            return;
        }
        n.say("✅ 已添加「" + c.name + "」" + weekday_cn(c.weekday) + " 大节" + std::to_string(c.big_section) +
              (c.count > 1 ? "-" + std::to_string(c.big_section + c.count - 1) : std::string()) + "，周次 " + weeks_text +
              "\n发「/课表」看效果。");
    }

    // ---- /课表 删 编号 ----
    static void remove_course(const PluginContext& ctx, Notifier n, const std::string& rest) {
        std::string sel = trim(rest);
        if (sel.empty()) {
            n.say("用法：/课表 删 编号（先用「/课表 课程」看编号）");
            return;
        }
        hbut::Session s;
        if (!Store::get().get_session(ctx.user_id, &s)) {
            n.say("还没登录教务系统，先「/课表 扫码」或「/课表 登录 学号 密码」。");
            return;
        }
        int64_t qq = ctx.user_id;
        std::thread([qq, s, sel, n]() {
            ThreadGuard guard;
            try {
                hbut::Session sess = s;
                std::vector<hbut::Course> raw;
                std::string err;
                bool expired = false;
                if (!load_raw_courses(sess, &raw, &err, &expired)) {
                    if (expired) {
                        Store::get().drop_session(qq);
                        n.say("教务会话已过期，请重新登录。");
                    } else {
                        n.say("读取课程失败：" + err);
                    }
                    return;
                }
                Store::get().put_session(qq, sess);

                auto names = course_names(raw);
                std::string match;
                if (is_all_digits(sel)) {
                    int idx = atoi(sel.c_str());
                    if (idx < 1 || idx > (int)names.size()) {
                        n.say("编号超出范围，发「/课表 课程」查看编号。");
                        return;
                    }
                    match = names[idx - 1];
                } else {
                    for (const auto& nm : names) {
                        if (nm == sel) { match = nm; break; }
                    }
                    if (match.empty()) {
                        n.say("找不到课程「" + sel + "」。");
                        return;
                    }
                }

                UserEdits edits;
                load_user_edits(qq, &edits);
                CourseEdit* target = find_or_add_edit(&edits, match);
                target->hidden = true;
                if (!save_user_edits(qq, edits)) {
                    n.say("保存失败，检查 data/hbut_jw 目录权限。");
                    return;
                }
                n.say("✅ 已隐藏「" + match + "」，发「/课表 还原」可恢复。");
            } catch (...) {
                n.say("删除失败");
            }
        }).detach();
    }

    // ---- /课表 时间 [节次 HH:MM-HH:MM|重置] ----
    static void edit_times(Notifier n, const std::string& rest) {
        auto times = load_period_times();
        if (rest.empty()) {
            std::string out = "当前作息时间（改：/课表 时间 3 10:15-11:00）\n";
            for (const auto& t : times) {
                out += "第" + std::to_string(t.p) + "节   " + t.start + " - " + t.end + "\n";
            }
            out += "重置：/课表 时间 重置";
            n.say(out);
            return;
        }
        if (rest == "重置" || rest == "reset") {
            reset_period_times();
            n.say("作息时间已恢复默认（湖工大 11 节）。");
            return;
        }

        auto fields = split_fields(rest);
        if (fields.size() < 2) {
            n.say("用法：/课表 时间 3 10:15-11:00");
            return;
        }
        int p = atoi(fields[0].c_str());
        if (p < 1 || p > 13) {
            n.say("节次范围 1~13。");
            return;
        }
        std::string range = fields[1];
        range = replace_all(range, "~", "-");
        range = replace_all(range, "－", "-");
        range = replace_all(range, "—", "-");
        size_t dash = range.find('-');
        if (dash == std::string::npos) {
            n.say("时间格式：10:15-11:00");
            return;
        }
        std::string start = trim(range.substr(0, dash));
        std::string end = trim(range.substr(dash + 1));
        if (start.find(':') == std::string::npos || end.find(':') == std::string::npos) {
            n.say("时间格式：10:15-11:00");
            return;
        }

        bool found = false;
        for (auto& t : times) {
            if (t.p == p) {
                t.start = start;
                t.end = end;
                found = true;
                break;
            }
        }
        if (!found) {
            PeriodTime t;
            t.p = p;
            t.start = start;
            t.end = end;
            times.push_back(t);
            std::sort(times.begin(), times.end(),
                      [](const PeriodTime& a, const PeriodTime& b) { return a.p < b.p; });
        }
        if (!save_period_times(times)) {
            n.say("保存失败，检查 data/hbut_jw 目录权限。");
            return;
        }
        n.say("✅ 第" + std::to_string(p) + "节改为 " + start + " - " + end + "，发「/课表」看效果。");
    }

    // ---- /课表 诊断 ----
    static void show_diag(const PluginContext& ctx, Notifier n) {
        hbut::Session s;
        if (!Store::get().get_session(ctx.user_id, &s)) {
            n.say("当前未登录，先「/课表 扫码」或「/课表 登录 学号 密码」。");
            return;
        }
        int64_t qq = ctx.user_id;
        n.say("正在诊断接口链路…");
        std::thread([qq, s, n]() {
            ThreadGuard guard;
            try {
                HttpClient hc;
                hc.jar = s.jar;
                std::string out;
                out += "链路：" + std::string(s.chaoxing ? "学习通" : "统一身份认证") + "\n";
                out += "域名：" + s.base + "\n";
                out += std::string("会话首页：") + (hbut::probe_session(hc, s.base) ? "OK" : "失效") + "\n";

                std::string sem = hbut::resolve_semester(s.semester);
                out += "学期：" + sem + "\n";

                std::string xhid;
                std::string err;
                bool exp = false;
                bool ok_xhid = hbut::fetch_xhid(hc, s.base, sem, &xhid, &err, &exp);
                out += std::string("xhid：") + (ok_xhid ? xhid : ("失败 " + err)) + "\n";

                hbut::TermCalendar cal;
                bool ok_cal = hbut::fetch_calendar(hc, s.base, sem, &cal, nullptr);
                out += "校历：" +
                       (ok_cal ? (std::string(cal.from_school ? "教务校历" : "按日期兜底") + "，共 " +
                                  std::to_string(cal.total_weeks) + " 周，当前第 " +
                                  std::to_string(cal.current_week) + " 周")
                               : std::string("失败")) +
                       "\n";

                if (ok_xhid) {
                    std::vector<hbut::Course> courses;
                    std::string e2;
                    bool exp2 = false;
                    bool ok_sch = hbut::fetch_schedule(hc, s.base, sem, xhid, &courses, &e2, &exp2);
                    out += std::string("课表：") +
                           (ok_sch ? (std::to_string(courses.size()) + " 条")
                                   : ("失败 " + e2 + (exp2 ? "（会话过期）" : ""))) +
                           "\n";
                    if (ok_sch && !courses.empty()) {
                        const auto& c = courses.front();
                        out += "样例：" + c.name + " 周" + std::to_string(c.weekday) + " 第" +
                               std::to_string(c.period) + "节 x" + std::to_string(c.count) + "\n";
                    }
                }
                n.say(out);
            } catch (...) {
                n.say("诊断失败");
            }
        }).detach();
    }

    // ---- /课表 状态 ----
    static void show_status(const PluginContext& ctx, Notifier n) {
        hbut::Session s;
        if (!Store::get().get_session(ctx.user_id, &s)) {
            n.say("当前未登录。\n私聊「/课表 扫码」或「/课表 登录 学号 密码」。");
            return;
        }
        int64_t qq = ctx.user_id;
        std::thread([qq, s, n]() {
            ThreadGuard guard;
            try {
                HttpClient hc;
                hc.jar = s.jar;
                bool alive = hbut::probe_session(hc, s.base);
                std::string text;
                text += alive ? "✅ 教务会话有效\n" : "⚠️ 教务会话已失效，请重新登录\n";
                text += "登录方式：" + std::string(s.chaoxing ? "学习通扫码" : "统一身份认证") + "\n";
                text += "教务域名：" + s.base + "\n";
                text += "学期：" + semester_label(s.semester) + "\n";
                if (!s.student_id.empty()) text += "学号：" + s.student_id + "\n";
                n.say(text);
            } catch (...) {
                n.say("状态检查失败");
            }
        }).detach();
    }
};

} // namespace

extern "C" __declspec(dllexport) IBotPlugin* create_plugin() {
    return new HbutJwPlugin();
}

extern "C" __declspec(dllexport) void destroy_plugin(IBotPlugin* p) {
    delete p;
}
