// jw_http.hpp —— 基于 WinHTTP 的极简 HTTP 客户端（自带 Cookie 会话）
// 之所以不用 include/simple_http.hpp：那边既没有 Cookie 也没有重定向控制，
// 而整个教务登录全靠 Cookie 会话 + 302 跳转。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace jw {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string final_url;
    std::string error;   // 非空表示传输层失败（DNS/连接/超时）
    bool transport_ok() const { return error.empty() && status > 0; }
    bool http_ok() const { return error.empty() && status >= 200 && status < 300; }
};

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;   // 小写，不带前导点
    std::string path = "/";
    long long expires = 0; // 0 = 会话 Cookie
    bool host_only = false;
    bool secure = false;
};

class CookieJar {
public:
    void clear();
    void add_from_set_cookie(const std::string& host, const std::string& set_cookie);
    // 生成 Cookie 请求头（无匹配时返回空串）
    std::string header_for(const std::string& host, const std::string& path, bool https) const;
    std::string to_json() const;
    bool from_json(const std::string& text);
    bool empty() const { return cookies_.empty(); }
    size_t size() const { return cookies_.size(); }

private:
    std::vector<Cookie> cookies_;
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    CookieJar jar;
    std::string user_agent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";
    std::string accept_language = "zh-CN,zh;q=0.9";
    int timeout_ms = 20000;

    HttpResponse get(const std::string& url,
                     const std::map<std::string, std::string>& headers = std::map<std::string, std::string>());

    HttpResponse post_form(const std::string& url, const std::string& form_body,
                           const std::map<std::string, std::string>& headers = std::map<std::string, std::string>());

private:
    HttpResponse request(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& content_type,
                         const std::map<std::string, std::string>& headers,
                         int max_redirects);
    void* session_ = nullptr;
};

// 工具：把 k=v 列表拼成 application/x-www-form-urlencoded（自动 urlencode）
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv);

} // namespace jw
