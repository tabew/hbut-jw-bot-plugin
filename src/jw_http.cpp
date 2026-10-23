#include "jw_http.hpp"
#include "jw_util.hpp"

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

namespace jw {
namespace {

struct UrlParts {
    std::wstring scheme;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
    bool valid = false;
};

UrlParts parse_url(const std::string& url) {
    UrlParts u;
    std::wstring w = wfromu8(url);
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &uc)) return u;
    u.scheme.assign(uc.lpszScheme, uc.dwSchemeLength);
    u.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    u.path = path;
    u.port = uc.nPort;
    u.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    u.valid = true;
    return u;
}

std::string host_of(const std::string& url) {
    UrlParts u = parse_url(url);
    if (!u.valid) return std::string();
    return to_lower(u8fromw(u.host));
}

std::string path_of(const std::string& url) {
    UrlParts u = parse_url(url);
    if (!u.valid) return "/";
    return u8fromw(u.path);
}

// 解析 Set-Cookie 里的日期（RFC1123: Wed, 21 Oct 2026 07:28:00 GMT）
long long parse_http_date(const std::string& raw) {
    static const char* kMon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char wd[16] = {0}, mon[8] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf_s(raw.c_str(), "%15[^,], %d %7s %d %d:%d:%d", wd, (unsigned)sizeof(wd),
                 &day, mon, (unsigned)sizeof(mon), &year, &hh, &mm, &ss) != 7) {
        return 0;
    }
    int mon_idx = 0;
    for (int i = 0; i < 12; ++i) {
        if (_stricmp(mon, kMon[i]) == 0) { mon_idx = i + 1; break; }
    }
    if (mon_idx == 0) return 0;
    SYSTEMTIME st;
    ZeroMemory(&st, sizeof(st));
    st.wYear = (WORD)year;
    st.wMonth = (WORD)mon_idx;
    st.wDay = (WORD)day;
    st.wHour = (WORD)hh;
    st.wMinute = (WORD)mm;
    st.wSecond = (WORD)ss;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (long long)(uli.QuadPart / 10000000ULL - 11644473600ULL);
}

std::string resolve_location(const std::string& base, const std::string& loc) {
    if (loc.empty()) return std::string();
    if (starts_with(loc, "http://") || starts_with(loc, "https://")) return loc;
    UrlParts u = parse_url(base);
    if (!u.valid) return std::string();
    std::string scheme = u8fromw(u.scheme);
    std::string host = u8fromw(u.host);
    std::string port_part;
    if ((u.https && u.port != 443) || (!u.https && u.port != 80)) {
        port_part = ":" + std::to_string(u.port);
    }
    if (starts_with(loc, "//")) return scheme + ":" + loc;
    if (starts_with(loc, "/")) return scheme + "://" + host + port_part + loc;
    std::string p = u8fromw(u.path);
    size_t slash = p.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "/" : p.substr(0, slash + 1);
    return scheme + "://" + host + port_part + dir + loc;
}

bool domain_match(const std::string& cookie_domain, const std::string& host) {
    if (cookie_domain == host) return true;
    if (host.size() > cookie_domain.size() + 1) {
        size_t off = host.size() - cookie_domain.size();
        if (host.compare(off, cookie_domain.size(), cookie_domain) == 0 && host[off - 1] == '.') {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------- CookieJar ----------------

void CookieJar::clear() { cookies_.clear(); }

void CookieJar::add_from_set_cookie(const std::string& host, const std::string& set_cookie) {
    if (set_cookie.empty()) return;
    auto parts = split(set_cookie, ';');
    if (parts.empty()) return;

    std::string first = trim(parts[0]);
    size_t eq = first.find('=');
    if (eq == std::string::npos) return;

    Cookie ck;
    ck.name = trim(first.substr(0, eq));
    ck.value = trim(first.substr(eq + 1));
    if (ck.name.empty()) return;

    bool expired = false;
    for (size_t i = 1; i < parts.size(); ++i) {
        std::string attr = trim(parts[i]);
        std::string lower = to_lower(attr);
        if (starts_with(lower, "domain=")) {
            std::string d = to_lower(trim(attr.substr(7)));
            if (!d.empty() && d[0] == '.') d.erase(0, 1);
            ck.domain = d;
            ck.host_only = false;
        } else if (starts_with(lower, "path=")) {
            std::string p = trim(attr.substr(5));
            if (!p.empty()) ck.path = p;
        } else if (starts_with(lower, "max-age=")) {
            long long secs = atoll(trim(attr.substr(8)).c_str());
            if (secs <= 0) {
                expired = true;
            } else {
                ck.expires = (long long)time(nullptr) + secs;
            }
        } else if (starts_with(lower, "expires=")) {
            long long t = parse_http_date(trim(attr.substr(8)));
            if (t > 0) {
                ck.expires = t;
                if (t < (long long)time(nullptr)) expired = true;
            }
        } else if (lower == "secure") {
            ck.secure = true;
        } else if (lower == "httponly") {
            /* 与普通 Cookie 同等对待 */
        }
    }

    if (ck.domain.empty()) {
        ck.domain = to_lower(host);
        ck.host_only = true;
    }

    // 同名同域同路径：覆盖；过期则删除
    for (auto it = cookies_.begin(); it != cookies_.end(); ++it) {
        if (it->name == ck.name && it->domain == ck.domain && it->path == ck.path) {
            if (expired || ck.value.empty()) {
                cookies_.erase(it);
            } else {
                *it = ck;
            }
            return;
        }
    }
    if (expired || ck.value.empty()) return;
    cookies_.push_back(ck);
}

std::string CookieJar::header_for(const std::string& host, const std::string& path, bool https) const {
    std::string out;
    long long now = (long long)time(nullptr);
    std::string lower_host = to_lower(host);
    for (const auto& ck : cookies_) {
        if (ck.expires != 0 && ck.expires < now) continue;
        if (ck.secure && !https) continue;
        if (!domain_match(ck.domain, lower_host)) continue;
        if (ck.host_only && ck.domain != lower_host) continue;
        if (!ck.path.empty() && ck.path != "/" && path.compare(0, ck.path.size(), ck.path) != 0) continue;
        if (!out.empty()) out += "; ";
        out += ck.name + "=" + ck.value;
    }
    return out;
}

std::string CookieJar::to_json() const {
    // Cookie 值按 RFC 6265 不含引号/反斜杠；这里再过滤一遍，
    // 保证写出的字符串能被自己的 from_json 精确读回。
    auto safe = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (c == '"' || c == '\\' || c < 0x20) continue;
            out.push_back((char)c);
        }
        return out;
    };

    std::string out = "[";
    long long now = (long long)time(nullptr);
    bool first = true;
    for (const auto& ck : cookies_) {
        if (ck.expires != 0 && ck.expires < now) continue;
        if (!first) out += ",";
        first = false;
        out += "{\"n\":\"" + safe(ck.name) + "\",\"v\":\"" + safe(ck.value) + "\",\"d\":\"" +
               safe(ck.domain) + "\",\"p\":\"" + safe(ck.path) + "\",\"e\":" +
               std::to_string(ck.expires) + ",\"h\":" + (ck.host_only ? "1" : "0") +
               ",\"s\":" + (ck.secure ? "1" : "0") + "}";
    }
    out += "]";
    return out;
}

bool CookieJar::from_json(const std::string& text) {
    // 逐个 {...} 对象解析，字段按 key 查找——不能假设键顺序：
    // 存盘走的是 simplejson，对象键是字典序（d/e/h/n/p/s/v），
    // 早先按固定顺序读会把所有 Cookie 读丢，重启后登录态全失效。
    cookies_.clear();
    size_t pos = 0;
    while (true) {
        size_t s = text.find('{', pos);
        if (s == std::string::npos) break;
        size_t e = text.find('}', s);
        if (e == std::string::npos) break;
        std::string obj = text.substr(s, e - s + 1);
        pos = e + 1;

        auto get_str = [&obj](const char* key) -> std::string {
            std::string k = std::string("\"") + key + "\":\"";
            size_t p = obj.find(k);
            if (p == std::string::npos) return std::string();
            p += k.size();
            size_t q = obj.find('"', p);
            if (q == std::string::npos) return std::string();
            return obj.substr(p, q - p);
        };
        auto get_num = [&obj](const char* key) -> long long {
            std::string k = std::string("\"") + key + "\":";
            size_t p = obj.find(k);
            if (p == std::string::npos) return 0;
            return atoll(obj.c_str() + p + k.size());
        };

        Cookie ck;
        ck.name = get_str("n");
        ck.value = get_str("v");
        ck.domain = get_str("d");
        ck.path = get_str("p");
        if (ck.path.empty()) ck.path = "/";
        ck.expires = get_num("e");
        ck.host_only = get_num("h") != 0;
        ck.secure = get_num("s") != 0;
        if (!ck.name.empty()) cookies_.push_back(ck);

        if (pos >= text.size()) break;
    }
    return !cookies_.empty();
}

// ---------------- HttpClient ----------------

HttpClient::HttpClient() {
    session_ = WinHttpOpen(L"TabMind-HBUT-JW/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        WinHttpSetTimeouts(session_, 10000, 10000, timeout_ms, timeout_ms);
    }
}

HttpClient::~HttpClient() {
    if (session_) WinHttpCloseHandle((HINTERNET)session_);
    session_ = nullptr;
}

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (const auto& item : kv) {
        if (!out.empty()) out += "&";
        out += url_encode(item.first) + "=" + url_encode(item.second);
    }
    return out;
}

HttpResponse HttpClient::get(const std::string& url, const std::map<std::string, std::string>& headers) {
    return request("GET", url, std::string(), std::string(), headers, 8);
}

HttpResponse HttpClient::post_form(const std::string& url, const std::string& form_body,
                                   const std::map<std::string, std::string>& headers) {
    return request("POST", url, form_body, "application/x-www-form-urlencoded; charset=UTF-8", headers, 8);
}

HttpResponse HttpClient::request(const std::string& method, const std::string& url_in,
                                 const std::string& body, const std::string& content_type,
                                 const std::map<std::string, std::string>& headers,
                                 int max_redirects) {
    HttpResponse resp;
    resp.final_url = url_in;
    if (!session_) {
        resp.error = "WinHTTP 初始化失败";
        return resp;
    }

    std::string url = url_in;
    std::string cur_method = method;
    std::string cur_body = body;
    std::string cur_ct = content_type;

    for (int hop = 0; hop <= max_redirects; ++hop) {
        UrlParts u = parse_url(url);
        if (!u.valid) {
            resp.error = "URL 解析失败: " + url;
            return resp;
        }

        HINTERNET conn = WinHttpConnect((HINTERNET)session_, u.host.c_str(), u.port, 0);
        if (!conn) {
            resp.error = "连接失败: " + u8fromw(u.host);
            return resp;
        }

        std::wstring wmethod = wfromu8(cur_method);
        DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, wmethod.c_str(), u.path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!req) {
            WinHttpCloseHandle(conn);
            resp.error = "创建请求失败";
            return resp;
        }

        DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        if (u.https) {
            DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                        SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
        }

        std::string host = to_lower(u8fromw(u.host));
        std::string path = u8fromw(u.path);

        std::string hdr = "Accept: text/html,application/json,application/xhtml+xml,*/*;q=0.8\r\n";
        hdr += "Accept-Language: " + accept_language + "\r\n";
        hdr += "Accept-Encoding: identity\r\n";
        hdr += "User-Agent: " + user_agent + "\r\n";
        std::string ck = jar.header_for(host, path, u.https);
        if (!ck.empty()) hdr += "Cookie: " + ck + "\r\n";
        for (const auto& h : headers) {
            if (_stricmp(h.first.c_str(), "cookie") == 0) continue; // 统一由 jar 管理
            hdr += h.first + ": " + h.second + "\r\n";
        }
        if (!cur_body.empty() && !cur_ct.empty()) hdr += "Content-Type: " + cur_ct + "\r\n";

        BOOL sent = WinHttpSendRequest(
            req, wfromu8(hdr).c_str(), (DWORD)hdr.size(),
            cur_body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)cur_body.data(),
            (DWORD)cur_body.size(), (DWORD)cur_body.size(), 0);
        BOOL recv = sent ? WinHttpReceiveResponse(req, nullptr) : FALSE;

        if (!recv) {
            DWORD err = GetLastError();
            resp.error = "请求发送/接收失败 (WinHTTP " + std::to_string(err) + ")";
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            return resp;
        }

        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        resp.status = (int)status;
        resp.final_url = url;

        // ---- 收 Set-Cookie ----
        DWORD raw_len = 0;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &raw_len, WINHTTP_NO_HEADER_INDEX);
        if (raw_len > 0) {
            std::wstring raw;
            raw.resize(raw_len / sizeof(wchar_t) + 2);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &raw[0], &raw_len, WINHTTP_NO_HEADER_INDEX)) {
                raw.resize(wcslen(raw.c_str()));
                std::string raw_u8 = u8fromw(raw);
                size_t p = 0;
                while (p < raw_u8.size()) {
                    size_t e = raw_u8.find("\r\n", p);
                    if (e == std::string::npos) e = raw_u8.size();
                    std::string line = raw_u8.substr(p, e - p);
                    p = e + 2;
                    if (line.empty()) continue;
                    std::string lower = to_lower(line);
                    if (starts_with(lower, "set-cookie:")) {
                        jar.add_from_set_cookie(host, line.substr(11));
                    }
                }
            }
        }

        // ---- 重定向 ----
        bool is_redirect = (resp.status == 301 || resp.status == 302 || resp.status == 303 ||
                            resp.status == 307 || resp.status == 308);
        if (is_redirect) {
            DWORD loc_len = 0;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                WINHTTP_NO_OUTPUT_BUFFER, &loc_len, WINHTTP_NO_HEADER_INDEX);
            std::string loc;
            if (loc_len > 0) {
                std::wstring wloc;
                wloc.resize(loc_len / sizeof(wchar_t) + 2);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                        &wloc[0], &loc_len, WINHTTP_NO_HEADER_INDEX)) {
                    wloc.resize(wcslen(wloc.c_str()));
                    loc = u8fromw(wloc);
                }
            }
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            if (!loc.empty()) {
                std::string next = resolve_location(url, loc);
                if (resp.status == 303 || (resp.status <= 302 && cur_method != "GET")) {
                    cur_method = "GET";
                    cur_body.clear();
                    cur_ct.clear();
                }
                url = next;
                continue;
            }
            resp.body.clear();
            return resp;
        }

        // ---- 读 body ----
        std::string data;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::string chunk;
            chunk.resize(avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0) break;
            chunk.resize(read);
            data += chunk;
        }
        resp.body.swap(data);

        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        return resp;
    }

    resp.error = "重定向次数过多";
    return resp;
}

} // namespace jw
