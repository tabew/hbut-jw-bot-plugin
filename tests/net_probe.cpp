// 本地联调工具（不参与插件构建）：验证学习通二维码链路与 CAS 登录页解析。
// 用法：net_probe.exe            → 申请二维码并存成 tests/qr.png，轮询一次
//       net_probe.exe cas        → 抓统一身份认证登录页，打印 execution/salt 长度
#include <cstdio>
#include <string>

#include "jw_hbut.hpp"
#include "jw_http.hpp"
#include "jw_util.hpp"

int main(int argc, char** argv) {
    bool cas_mode = (argc > 1 && std::string(argv[1]) == "cas");

    if (argc > 2 && std::string(argv[1]) == "raw") {
        jw::HttpClient hc;
        auto r = hc.get(argv[2]);
        printf("status=%d final=%s bytes=%zu error=%s cookies=%zu\n", r.status, r.final_url.c_str(),
               r.body.size(), r.error.c_str(), hc.jar.size());
        printf("%.600s\n", r.body.c_str());
        return 0;
    }

    if (cas_mode) {
        jw::HttpClient hc;
        hbut::CasPage page;
        std::string err;
        bool ok = hbut::cas_begin(hc, hbut::kJwxtCasService, &page, &err);
        printf("cas_begin=%s err=%s\n", ok ? "OK" : "FAIL", err.c_str());
        printf("execution_len=%zu lt=%s salt_len=%zu need_captcha=%d logged_in=%d hidden=%zu\n",
               page.execution.size(), page.lt.empty() ? "(none)" : page.lt.c_str(), page.salt.size(),
               (int)page.need_captcha, (int)page.logged_in, page.hidden.size());
        for (const auto& kv : page.hidden) {
            printf("  hidden %s = %.40s\n", kv.first.c_str(), kv.second.c_str());
        }
        std::string img;
        bool jpeg = false;
        if (hbut::cas_captcha(hc, &img, &jpeg, &err)) {
            printf("captcha bytes=%zu jpeg=%d\n", img.size(), (int)jpeg);
            jw::write_file_binary(jpeg ? "tests/captcha.jpg" : "tests/captcha.png", img);
        } else {
            printf("captcha FAIL %s\n", err.c_str());
        }
        return 0;
    }

    jw::HttpClient hc;
    hbut::QrTicket tk;
    std::string err;
    if (!hbut::chaoxing_qr_create(hc, &tk, &err)) {
        printf("qr_create FAIL: %s\n", err.c_str());
        return 1;
    }
    printf("uuid=%s enc=%s cookies=%zu\n", tk.uuid.c_str(), tk.enc.c_str(), hc.jar.size());

    std::string png;
    if (!hbut::chaoxing_qr_image(hc, tk.uuid, &png, &err)) {
        printf("qr_image FAIL: %s\n", err.c_str());
        return 2;
    }
    printf("qr_image bytes=%zu -> tests/qr.png\n", png.size());
    jw::write_file_binary("tests/qr.png", png);

    std::string msg;
    auto st = hbut::chaoxing_qr_poll(hc, tk, &msg);
    printf("poll state=%d msg=%s\n", (int)st, msg.c_str());
    return 0;
}
