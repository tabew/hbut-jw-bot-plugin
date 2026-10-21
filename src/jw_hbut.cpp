#include "jw_hbut.hpp"
#include "jw_aes.hpp"
#include "simple_json.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace hbut {
namespace {

using jw::base64_decode;
using jw::between;
using jw::cas_encrypt_password;
using jw::contains;
using jw::form_encode;
using jw::plain_text;
using jw::replace_all;
using jw::split;
using jw::starts_with;
using jw::to_lower;
using jw::trim;
using jw::url_encode;

using TagAttrs = std::map<std::string, std::string>;

// 解析所有 <input ...> 标签的属性
std::vector<TagAttrs> parse_inputs(const std::string& html) {
    std::vector<TagAttrs> out;
    size_t pos = 0;
    while (true) {
        size_t s = html.find("<input", pos);
        if (s == std::string::npos) break;
        size_t e = html.find('>', s);
        if (e == std::string::npos) break;
        std::string tag = html.substr(s + 6, e - s - 6);
        pos = e + 1;

        TagAttrs attrs;
        size_t i = 0;
        while (i < tag.size()) {
            // 找 name
            while (i < tag.size() && !isalpha((unsigned char)tag[i])) ++i;
            size_t ns = i;
            while (i < tag.size() && (isalnum((unsigned char)tag[i]) || tag[i] == '-' || tag[i] == '_')) ++i;
            if (ns == i) break;
            std::string key = to_lower(tag.substr(ns, i - ns));
            while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
            std::string val;
            if (i < tag.size() && tag[i] == '=') {
                ++i;
                while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
                if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
                    char q = tag[i++];
                    size_t vs = i;
                    while (i < tag.size() && tag[i] != q) ++i;
                    val = tag.substr(vs, i - vs);
                    if (i < tag.size()) ++i;
                } else {
                    size_t vs = i;
                    while (i < tag.size() && tag[i] != ' ' && tag[i] != '\t') ++i;
                    val = tag.substr(vs, i - vs);
                }
            }
            if (!key.empty()) attrs[key] = val;
        }
        out.push_back(attrs);
    }
    return out;
}

std::string input_value_by_id(const std::vector<TagAttrs>& inputs, const std::string& id) {
    for (const auto& a : inputs) {
        auto it = a.find("id");
        if (it != a.end() && it->second == id) {
            auto v = a.find("value");
            if (v != a.end()) return v->second;
        }
    }
    return std::string();
}

std::string input_value_by_name(const std::vector<TagAttrs>& inputs, const std::string& name) {
    for (const auto& a : inputs) {
        auto it = a.find("name");
        if (it != a.end() && it->second == name) {
            auto v = a.find("value");
            if (v != a.end()) return v->second;
        }
    }
    return std::string();
}

bool looks_like_login_page(const std::string& url_or_html) {
    std::string s = to_lower(url_or_html);
    return contains(s, "authserver/login") || contains(s, "/admin/login") ||
           contains(s, "passport2.chaoxing.com") || contains(s, "passport.chaoxing.com");
}

// 从登录失败页里抠出中文提示
std::string extract_login_error(const std::string& html) {
    // 常见容器：<span id="msg">xxx</span> / <div class="authError">xxx</div>
    static const char* kBoxes[] = {
        "id=\"msg\"", "id='msg'", "class=\"authError\"", "class='authError'",
        "class=\"tips-error\"", "class=\"error\"", "id=\"errorMsg\"", "class=\"msg-error\""
    };
    for (const char* box : kBoxes) {
        size_t p = html.find(box);
        if (p == std::string::npos) continue;
        size_t gt = html.find('>', p);
        if (gt == std::string::npos) continue;
        size_t lt = html.find('<', gt + 1);
        if (lt == std::string::npos) continue;
        std::string text = plain_text(html.substr(gt + 1, lt - gt - 1));
        if (!text.empty()) return text;
    }
    // 直接找关键字
    static const char* kPhrases[] = {
        "用户名或密码错误", "密码错误", "验证码错误", "验证码不正确", "验证码已失效",
        "账号不存在", "用户不存在", "账号被锁定", "密码不正确", "登录过于频繁",
        "用户名不能为空", "密码不能为空", "验证码不能为空"
    };
    for (const char* ph : kPhrases) {
        if (html.find(ph) != std::string::npos) return std::string(ph);
    }
    return std::string();
}

int json_int(const simplejson::Value& v, const char* key, int def) {
    const auto& f = v[key];
    if (f.is_number()) return (int)f.get<int>();
    if (f.is_string()) return atoi(f.get<std::string>().c_str());
    return def;
}

std::string json_str(const simplejson::Value& v, const char* key) {
    const auto& f = v[key];
    if (f.is_string()) return plain_text(f.get<std::string>());
    return std::string();
}

std::string first_nonempty(const simplejson::Value& v, const char* a, const char* b, const char* c = nullptr) {
    std::string s = json_str(v, a);
    if (!s.empty()) return s;
    s = json_str(v, b);
    if (!s.empty()) return s;
    if (c) return json_str(v, c);
    return std::string();
}

} // namespace

// ---------------- 学习通扫码 ----------------

bool chaoxing_qr_create(jw::HttpClient& hc, QrTicket* out, std::string* err) {
    std::string url = std::string(kChaoxingPassport) + "/login?fid=&refer=http%3A%2F%2Fi.chaoxing.com";
    auto r = hc.get(url, {{"Referer", "https://i.chaoxing.com/"}});
    if (!r.transport_ok()) {
        if (err) *err = "无法连接学习通登录页：" + r.error;
        return false;
    }
    if (r.status != 200) {
        if (err) *err = "学习通登录页返回 HTTP " + std::to_string(r.status);
        return false;
    }
    auto inputs = parse_inputs(r.body);
    std::string uuid = input_value_by_id(inputs, "uuid");
    std::string enc = input_value_by_id(inputs, "enc");
    if (uuid.empty()) {
        if (err) *err = "学习通登录页结构变化：未取到二维码 uuid";
        return false;
    }
    if (enc.empty()) {
        // 少数模板把 enc 放在 JS 里
        between(r.body, "enc = \"", "\"", &enc);
        if (enc.empty()) between(r.body, "\"enc\":\"", "\"", &enc);
    }
    out->uuid = uuid;
    out->enc = enc;
    out->refer = "http://i.chaoxing.com";
    return true;
}

bool chaoxing_qr_image(jw::HttpClient& hc, const std::string& uuid, std::string* png, std::string* err) {
    std::string url = std::string(kChaoxingPassport) + "/createqr?uuid=" + uuid + "&fid=-1";
    auto r = hc.get(url, {{"Referer", std::string(kChaoxingPassport) + "/login?fid=&refer=http%3A%2F%2Fi.chaoxing.com"}});
    if (!r.transport_ok() || r.status != 200 || r.body.size() < 200) {
        if (err) *err = "二维码图片下载失败";
        return false;
    }
    *png = r.body;
    return true;
}

QrState chaoxing_qr_poll(jw::HttpClient& hc, const QrTicket& tk, std::string* message) {
    std::string body = jw::form_encode({
        {"enc", tk.enc},
        {"uuid", tk.uuid},
        {"doubleFactorLogin", "0"},
        {"forbidotherlogin", "0"},
    });
    auto r = hc.post_form(std::string(kChaoxingPassport) + "/getauthstatus/v2", body,
                          {{"X-Requested-With", "XMLHttpRequest"},
                           {"Referer", std::string(kChaoxingPassport) +
                                           "/login?fid=&refer=http%3A%2F%2Fi.chaoxing.com"}});
    if (!r.transport_ok()) {
        if (message) *message = r.error;
        return QrState::Failed;
    }
    auto v = simplejson::Value::parse(r.body);
    if (!v.is_object()) {
        // 非 JSON：多半是登录页被踢回
        if (message) *message = "学习通返回了非预期内容";
        return QrState::Waiting;
    }
    bool ok = false;
    if (v["status"].is_boolean()) ok = v["status"].get<bool>();
    else if (v["status"].is_number()) ok = v["status"].get<int>() != 0;

    std::string msg = json_str(v, "mess");
    if (msg.empty()) msg = json_str(v, "msg");
    if (msg.empty()) msg = json_str(v, "message");
    if (msg.empty()) msg = json_str(v, "nickname");
    if (message) *message = msg;

    if (ok) return QrState::Confirmed;
    if (contains(msg, "失效") || contains(msg, "过期") || contains(msg, "取消") ||
        contains(msg, "异常") || contains(msg, "失败") || contains(msg, "error")) {
        return QrState::Expired;
    }
    if (contains(msg, "已扫描") || contains(msg, "待确认") || contains(msg, "确认")) {
        return QrState::Scanned;
    }
    return QrState::Waiting;
}

namespace {

// 学习通首页 HTML 里的补票链路（xxtlogin），用于把学习通票据换成教务短票 jw_uf
void collect_xxtlogin(const std::string& html, std::vector<std::string>* out) {
    auto push = [out](std::string u) {
        if (u.empty()) return;
        for (const auto& e : *out) {
            if (e == u) return;
        }
        out->push_back(u);
    };

    size_t pos = 0;
    while ((pos = html.find("xxtlogin", pos)) != std::string::npos) {
        size_t begin = pos;
        while (begin > 0 && html[begin - 1] != '"' && html[begin - 1] != '\'' &&
               html[begin - 1] != ' ' && html[begin - 1] != '(' && html[begin - 1] != '=') {
            --begin;
        }
        size_t end = pos;
        while (end < html.size() && html[end] != '"' && html[end] != '\'' && html[end] != ' ' &&
               html[end] != ')' && html[end] != '<') {
            ++end;
        }
        std::string cand = html.substr(begin, end - begin);
        pos = end;

        if (starts_with(cand, "//")) {
            cand = "https:" + cand;
        } else if (starts_with(cand, "http://")) {
            cand = replace_all(cand, "http://", "https://");
        } else if (!starts_with(cand, "https://") && !starts_with(cand, "/admin/")) {
            cand = "https://" + std::string(cand[0] == '/' ? cand.c_str() + 1 : cand.c_str());
        }

        if (starts_with(cand, "/admin/api/xxtlogin")) {
            for (const char* host : {"https://vkb.jw.chaoxing.com", "https://hbut.jw.chaoxing.com",
                                     "https://i.chaoxing.com"}) {
                push(std::string(host) + cand);
            }
            continue;
        }
        push(cand);
    }

    // 兜底：首页拿不到链路时，直接点常用补票入口
    push("https://vkb.jw.chaoxing.com/admin/api/xxtlogin?loginUrl=https%3A%2F%2Fhbut.jw.chaoxing.com%2Fadmin%2Flogin2%3Frole%3Dxs");
}

} // namespace

bool chaoxing_bootstrap(jw::HttpClient& hc, std::string* err) {
    // 学习通票据（.chaoxing.com 域）→ 教务会话：
    // 先访问 i.chaoxing.com 首页拿到 xxtlogin 补票链路，逐个点一遍，
    // 服务端才会给 hbut.jw.chaoxing.com 下发 jw_uf 短票。
    auto base = hc.get("https://i.chaoxing.com/base?t=" + std::to_string(jw::now_ms()),
                       {{"Referer", "https://i.chaoxing.com/"}});
    if (!base.transport_ok()) {
        if (err) *err = "无法连接学习通：" + base.error;
        return false;
    }
    if (contains(base.final_url, "passport2.chaoxing.com/login") ||
        contains(base.final_url, "passport2.chaoxing.com/mlogin")) {
        if (err) *err = "学习通票据未被接受，请重新扫码";
        return false;
    }

    std::vector<std::string> bridges;
    collect_xxtlogin(base.body, &bridges);
    for (const auto& u : bridges) {
        hc.get(u, {{"Referer", "https://i.chaoxing.com/"}});
    }

    // 预热教务首页 + 菜单接口，让服务端补齐会话
    hc.get(std::string(kChaoxingJwBase) + "/admin/", {{"Referer", "https://i.chaoxing.com/"}});
    hc.post_form(std::string(kChaoxingJwBase) + "/admin/getMenuList", std::string(),
                 {{"X-Requested-With", "XMLHttpRequest"},
                  {"Origin", kChaoxingJwBase},
                  {"Referer", std::string(kChaoxingJwBase) + "/admin/"}});

    std::string ck = hc.jar.header_for("hbut.jw.chaoxing.com", "/", true);
    bool has_jw_uf = contains(ck, "jw_uf=");
    if (!has_jw_uf) {
        if (err) *err = "教务短票 jw_uf 未下发";
        return false;
    }
    return true;
}

// ---------------- 统一身份认证 ----------------

bool cas_begin(jw::HttpClient& hc, const std::string& service, CasPage* page, std::string* err) {
    std::string url = std::string(kAuthBase) + "/login?service=" + url_encode(service);
    auto r = hc.get(url, {{"Referer", std::string(kAuthBase) + "/"}});
    if (!r.transport_ok()) {
        if (err) *err = "无法连接统一身份认证：" + r.error;
        return false;
    }
    if (contains(r.final_url, "ticket=") && !contains(r.final_url, "authserver/login")) {
        page->logged_in = true;
        return true;
    }
    auto inputs = parse_inputs(r.body);
    page->execution = input_value_by_name(inputs, "execution");
    if (page->execution.empty()) page->execution = input_value_by_id(inputs, "execution");
    page->lt = input_value_by_name(inputs, "lt");
    page->salt = input_value_by_id(inputs, "pwdEncryptSalt");
    if (page->salt.empty()) between(r.body, "pwdEncryptSalt\" value=\"", "\"", &page->salt);
    if (page->salt.empty()) {
        // 某些模板用 JS 变量
        between(r.body, "pwdEncryptSalt = \"", "\"", &page->salt);
        if (page->salt.empty()) between(r.body, "pwdEncryptSalt='", "'", &page->salt);
    }

    page->hidden.clear();
    for (const auto& a : inputs) {
        auto n = a.find("name");
        if (n == a.end() || n->second.empty()) continue;
        auto t = a.find("type");
        bool is_hidden = (t != a.end() && to_lower(t->second) == "hidden");
        std::string name = n->second;
        if (!is_hidden) continue;
        auto v = a.find("value");
        page->hidden.push_back({name, v != a.end() ? v->second : std::string()});
    }

    page->need_captcha = contains(r.body, "captchaResponse") || contains(r.body, "getCaptcha") ||
                         contains(r.body, "name=\"captcha\"") || contains(r.body, "id=\"captcha\"");

    if (page->execution.empty() || page->salt.empty()) {
        if (err) {
            *err = "统一身份认证登录页解析失败（execution/salt 缺失），学校可能改版";
        }
        return false;
    }
    return true;
}

bool cas_captcha(jw::HttpClient& hc, std::string* image, bool* is_jpeg, std::string* err) {
    std::string url = std::string(kAuthBase) + "/getCaptcha.htl?" + std::to_string(jw::now_ms());
    auto r = hc.get(url, {{"Referer", std::string(kAuthBase) + "/login"}});
    if (!r.transport_ok() || r.body.size() < 100) {
        if (err) *err = "验证码图片获取失败";
        return false;
    }
    std::string data = r.body;
    bool jpeg = false;
    if ((unsigned char)data[0] == 0x89 && data.substr(1, 3) == "PNG") {
        jpeg = false;
    } else if ((unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8) {
        jpeg = true;
    } else {
        // 返回的是 JSON：{"captcha":"data:image/png;base64,..."}
        std::string b64;
        if (!between(data, "\"captcha\":\"", "\"", &b64)) {
            if (err) *err = "验证码返回格式无法识别";
            return false;
        }
        size_t comma = b64.find("base64,");
        if (comma != std::string::npos) b64 = b64.substr(comma + 7);
        data = base64_decode(b64);
        if (data.size() < 100) {
            if (err) *err = "验证码解码失败";
            return false;
        }
        jpeg = !((unsigned char)data[0] == 0x89);
    }
    *image = data;
    if (is_jpeg) *is_jpeg = jpeg;
    return true;
}

CasResult cas_submit(jw::HttpClient& hc, const CasPage& page, const std::string& user,
                     const std::string& password, const std::string& captcha,
                     const std::string& service, std::string* err) {
    std::string login_url = std::string(kAuthBase) + "/login?service=" + url_encode(service);

    // 登录页里同时存在 fido / dynamic / qr / userName 四套表单，
    // 它们的 hidden 字段同名（cllt/dllt/lt/execution），全量提交会互相污染。
    // 这里只提交账号密码真正需要的字段。
    std::vector<std::pair<std::string, std::string>> form;
    auto set = [&form](const std::string& k, const std::string& v, bool keep_first = false) {
        for (auto& kv : form) {
            if (kv.first == k) {
                if (!keep_first || kv.second.empty()) kv.second = v;
                return;
            }
        }
        form.push_back({k, v});
    };

    set("username", user);
    set("password", cas_encrypt_password(password, page.salt));
    set("_eventId", "submit");
    set("cllt", "userNameLogin");
    set("dllt", "generalLogin");
    set("rmShown", "1");
    if (!page.lt.empty()) set("lt", page.lt, true);
    if (!page.execution.empty()) set("execution", page.execution, true);
    if (!captcha.empty()) set("captcha", captcha);

    auto r = hc.post_form(login_url, jw::form_encode(form), {{"Referer", login_url}, {"Origin", kAuthBase}});
    if (!r.transport_ok()) {
        if (err) *err = "提交登录失败：" + r.error;
        return CasResult::Fail;
    }
    if (contains(r.final_url, "ticket=")) return CasResult::Ok;

    // 没带 ticket 但已经落在业务页（非登录页），也视为通过，后续接口会二次校验
    if (!looks_like_login_page(r.final_url) && !contains(r.body, "pwdEncryptSalt")) {
        if (r.status == 200) return CasResult::Ok;
    }

    std::string msg = extract_login_error(r.body);
    if (msg.empty()) {
        if (r.status == 401) msg = "账号或密码错误";
        else if (looks_like_login_page(r.final_url)) msg = "登录未通过（仍停留在登录页）";
        else msg = "登录失败（HTTP " + std::to_string(r.status) + "）";
    }
    if (err) *err = msg;
    if (contains(msg, "验证码")) return CasResult::NeedCaptcha;
    return CasResult::Fail;
}

// ---------------- 课表 ----------------

bool probe_session(jw::HttpClient& hc, const std::string& base) {
    auto r = hc.get(base + "/admin/index.html");
    if (!r.transport_ok()) return false;
    return !looks_like_login_page(r.final_url);
}

bool fetch_xhid(jw::HttpClient& hc, const std::string& base, const std::string& semester,
                std::string* xhid, std::string* err, bool* session_expired) {
    std::vector<std::string> urls = {
        base + "/admin/xsd/pkgl/xskb/queryKbForXsd?xnxq=" + semester,
        base + "/admin/pkgl/xskb/queryKbForXsd?xnxq=" + semester,
        base + "/admin/xsd/pkgl/xskb/queryKbForXsd",
    };
    if (session_expired) *session_expired = false;
    std::string last;
    for (const auto& url : urls) {
        auto r = hc.get(url, {{"Referer", base + "/admin/index.html"},
                              {"X-Requested-With", "XMLHttpRequest"},
                              {"Accept", "text/html,application/xhtml+xml,*/*"}});
        if (!r.transport_ok()) {
            last = "请求失败: " + r.error;
            continue;
        }
        if (looks_like_login_page(r.final_url)) {
            if (session_expired) *session_expired = true;
            if (err) *err = "会话已过期，请重新登录";
            return false;
        }
        if (r.status != 200) {
            last = "状态码 " + std::to_string(r.status);
            continue;
        }
        auto inputs = parse_inputs(r.body);
        std::string found = input_value_by_id(inputs, "xhid");
        if (found.empty()) {
            between(r.body, "xhid=", "&", &found);
            if (found.empty()) between(r.body, "xhid=", "\"", &found);
            if (found.empty()) between(r.body, "xhid=", "'", &found);
        }
        if (!found.empty()) {
            *xhid = trim(found);
            return true;
        }
        last = "页面中未找到 xhid";
    }
    if (err) *err = last.empty() ? "无法获取学号 ID" : last;
    return false;
}

std::vector<int> parse_weeks(const std::string& text) {
    std::vector<int> weeks;
    if (text.empty()) return weeks;
    bool odd = contains(text, "(单)") || contains(text, "（单）");
    bool even = contains(text, "(双)") || contains(text, "（双）");

    std::string s = text;
    s = replace_all(s, "周", "");
    s = replace_all(s, "(单)", "");
    s = replace_all(s, "（单）", "");
    s = replace_all(s, "(双)", "");
    s = replace_all(s, "（双）", "");
    s = replace_all(s, "，", ",");

    for (auto part : split(s, ',')) {
        part = trim(part);
        if (part.empty()) continue;
        size_t dash = part.find('-');
        if (dash != std::string::npos) {
            int a = atoi(part.substr(0, dash).c_str());
            int b = atoi(part.substr(dash + 1).c_str());
            if (a > 0 && b >= a && b - a < 40) {
                for (int w = a; w <= b; ++w) weeks.push_back(w);
            }
        } else {
            int w = atoi(part.c_str());
            if (w > 0) weeks.push_back(w);
        }
    }
    if (odd || even) {
        std::vector<int> filtered;
        for (int w : weeks) {
            if (odd && w % 2 == 0) continue;
            if (even && w % 2 == 1) continue;
            filtered.push_back(w);
        }
        weeks.swap(filtered);
    }
    std::sort(weeks.begin(), weeks.end());
    weeks.erase(std::unique(weeks.begin(), weeks.end()), weeks.end());
    return weeks;
}

bool fetch_schedule(jw::HttpClient& hc, const std::string& base, const std::string& semester,
                    const std::string& xhid, std::vector<Course>* out, std::string* err,
                    bool* session_expired) {
    if (session_expired) *session_expired = false;
    std::vector<std::string> paths = {
        "/admin/xsd/pkgl/xskb/sdpkkbList",
        "/admin/pkgl/xskb/sdpkkbList",
    };
    std::string last_err;
    for (const auto& path : paths) {
        std::string url = base + path + "?xnxq=" + url_encode(semester) + "&xhid=" + url_encode(xhid) +
                          "&xqdm=1&zdzc=&zxzc=&xskbxslx=0";
        auto r = hc.get(url, {{"Referer", base + "/admin/xsd/pkgl/xskb/queryKbForXsd?xnxq=" + semester},
                              {"X-Requested-With", "XMLHttpRequest"},
                              {"Accept", "application/json, text/javascript, */*; q=0.01"}});
        if (!r.transport_ok()) {
            last_err = "请求失败: " + r.error;
            continue;
        }
        if (looks_like_login_page(r.final_url)) {
            if (session_expired) *session_expired = true;
            if (err) *err = "会话已过期，请重新登录";
            return false;
        }
        if (r.status != 200) {
            last_err = "状态码 " + std::to_string(r.status);
            continue;
        }
        auto v = simplejson::Value::parse(r.body);
        if (!v.is_object()) {
            last_err = "课表返回不是 JSON（可能未选课或接口改版）";
            continue;
        }
        int ret = json_int(v, "ret", 0);
        if (ret != 0) {
            std::string msg = json_str(v, "msg");
            if (msg.empty()) msg = "接口返回 ret=" + std::to_string(ret);
            if (contains(msg, "无课表") || contains(msg, "暂无")) {
                if (err) *err = "该学期没有课表";
                return false;
            }
            last_err = msg;
            continue;
        }
        const auto& data = v["data"];
        if (!data.is_array()) {
            last_err = "课表数据为空";
            continue;
        }
        out->clear();
        for (size_t i = 0; i < data.size(); ++i) {
            const auto& item = data[i];
            Course c;
            c.name = first_nonempty(item, "kcmc", "kcbmc");
            if (c.name.empty()) continue;
            c.teacher = first_nonempty(item, "tmc", "xm", "jsmc");
            c.room = first_nonempty(item, "croommc", "cdmc");
            c.building = json_str(item, "jxlmc");
            c.class_name = json_str(item, "jxbzc");
            c.weekday = json_int(item, "xingqi", json_int(item, "xqj", 1));
            c.period = json_int(item, "djc", json_int(item, "jcs", 1));
            c.count = json_int(item, "djs", 2);
            if (c.weekday < 1 || c.weekday > 7) continue;
            if (c.period < 1) c.period = 1;
            if (c.count < 1) c.count = 2;
            std::string wtext = json_str(item, "zcstr");
            if (wtext.empty()) wtext = json_str(item, "zc");
            if (wtext.empty()) wtext = json_str(item, "zcd");
            c.weeks = parse_weeks(wtext);
            if (c.weeks.empty()) {
                // 少数接口把周次直接给成数字
                int zc = json_int(item, "zc", 0);
                if (zc > 0) c.weeks.push_back(zc);
            }
            out->push_back(c);
        }
        return true;
    }
    if (err) *err = last_err.empty() ? "课表获取失败" : last_err;
    return false;
}

bool fetch_calendar(jw::HttpClient& hc, const std::string& base, const std::string& semester,
                    TermCalendar* out, std::string* err) {
    std::vector<WeekSpan> spans;
    std::string url = base + "/admin/xsd/jcsj/xlgl/getData/" + semester;
    auto r = hc.get(url, {{"Referer", base + "/admin/index.html"},
                          {"X-Requested-With", "XMLHttpRequest"},
                          {"Accept", "application/json, text/javascript, */*; q=0.01"}});
    if (r.transport_ok() && r.status == 200) {
        auto v = simplejson::Value::parse(r.body);
        if (v.is_array()) {
            std::map<int, WeekSpan> merged;
            for (size_t i = 0; i < v.size(); ++i) {
                const auto& row = v[i];
                if (!row.is_object()) continue;
                int zc = json_int(row, "zc", 0);
                if (zc <= 0) continue;
                jw::Date mon, sun;
                std::string ms = json_str(row, "monday");
                std::string ss = json_str(row, "sunday");
                if (!jw::parse_date(ms, &mon)) continue;
                if (!jw::parse_date(ss, &sun)) sun = jw::add_days(mon, 6);
                auto it = merged.find(zc);
                if (it == merged.end()) {
                    WeekSpan ws;
                    ws.week = zc;
                    ws.monday = mon;
                    ws.sunday = sun;
                    merged[zc] = ws;
                } else {
                    if (jw::days_between(mon, it->second.monday) > 0) it->second.monday = mon;
                    if (jw::days_between(it->second.sunday, sun) > 0) it->second.sunday = sun;
                }
            }
            for (auto& kv : merged) spans.push_back(kv.second);
        }
    }

    if (!spans.empty()) {
        // 教务有时给「学年周次」（下学期从 26 开始），归一化成学期周次
        int minw = spans.front().week;
        for (auto& s : spans) minw = std::min(minw, s.week);
        if (minw > 1) {
            for (auto& s : spans) s.week = s.week - minw + 1;
        }
        std::sort(spans.begin(), spans.end(),
                  [](const WeekSpan& a, const WeekSpan& b) { return a.week < b.week; });
        out->weeks = spans;
        out->total_weeks = spans.back().week;
        out->from_school = true;
    } else {
        // 兜底：按学期起始日推算（教务校历接口偶尔 404）
        auto parts = split(semester, '-');
        if (parts.size() == 3) {
            int y = atoi(parts[0].c_str());
            int term = atoi(parts[2].c_str());
            jw::Date start;
            start.y = (term == 1) ? y : y + 1;
            start.m = (term == 1) ? 9 : 3;
            start.d = 1;
            start = jw::add_days(start, -jw::weekday_mon0(start));
            for (int w = 1; w <= 22; ++w) {
                WeekSpan ws;
                ws.week = w;
                ws.monday = jw::add_days(start, (w - 1) * 7);
                ws.sunday = jw::add_days(ws.monday, 6);
                spans.push_back(ws);
            }
            out->weeks = spans;
            out->total_weeks = 22;
            out->from_school = false;
        } else {
            if (err) *err = "无法解析学期格式";
            return false;
        }
    }

    jw::Date today = jw::today_local();
    out->current_week = infer_current_week(out->weeks);
    for (const auto& s : out->weeks) {
        if (jw::days_between(s.monday, today) >= 0 && jw::days_between(today, s.sunday) >= 0) {
            out->current_week = s.week;
            break;
        }
    }
    return true;
}

int infer_current_week(const std::vector<WeekSpan>& weeks) {
    if (weeks.empty()) return 1;
    jw::Date today = jw::today_local();
    for (const auto& s : weeks) {
        if (jw::days_between(s.monday, today) >= 0 && jw::days_between(today, s.sunday) >= 0) {
            return s.week;
        }
    }
    if (jw::days_between(today, weeks.front().monday) > 0) return weeks.front().week;
    int days = jw::days_between(weeks.front().monday, today);
    int w = weeks.front().week + days / 7;
    int maxw = weeks.back().week;
    if (w < 1) w = 1;
    if (w > maxw) w = maxw;
    return w;
}

std::string resolve_semester(const std::string& hint) {
    if (jw::semester_valid(hint)) return hint;
    return jw::semester_by_date(jw::today_local());
}

} // namespace hbut
