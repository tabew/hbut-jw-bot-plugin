// 插件冒烟测试：直接 LoadLibrary 加载 hbut_jw.dll，模拟几条消息，
// 确认导出符号、on_load、指令路由与异步回调都正常（不联网）。
// 用法：plugin_smoke.exe [插件目录]  默认 ..\
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "plugin_manager.hpp"

static void on_send(void* user_data, const char* message_type, int64_t target, const char* text) {
    printf("[%s -> %lld] %s\n", message_type, (long long)target, text);
    if (user_data) *(int*)user_data += 1;
}

static void on_log(void* user_data, const char* text) {
    printf("[log] %s\n", text);
}

int main(int argc, char** argv) {
    std::string dir = (argc > 1) ? argv[1] : "..\\";
    std::string dll = dir + (dir.back() == '\\' || dir.back() == '/' ? "" : "\\") + "hbut_jw.dll";
    printf("load: %s\n", dll.c_str());

    HMODULE mod = LoadLibraryA(dll.c_str());
    if (!mod) {
        printf("LoadLibrary failed, err=%lu\n", GetLastError());
        return 1;
    }
    auto create = (IBotPlugin * (*)())GetProcAddress(mod, "create_plugin");
    auto destroy = (void (*)(IBotPlugin*))GetProcAddress(mod, "destroy_plugin");
    if (!create || !destroy) {
        printf("missing exports\n");
        return 2;
    }
    IBotPlugin* p = create();
    PluginInfo info = p->get_info();
    printf("plugin: %s v%s by %s\n  %s\n", info.name.c_str(), info.version.c_str(),
           info.author.c_str(), info.description.c_str());

    // 预置一个「别人」的假会话，供后面群聊 @ 测试用
    CreateDirectoryA("data", nullptr);
    CreateDirectoryA("data\\hbut_jw", nullptr);
    CreateDirectoryA("data\\hbut_jw\\users", nullptr);
    {
        FILE* f = fopen("data/hbut_jw/users/10002.json", "wb");
        if (f) {
            const char* sess =
                "{\"base\":\"https://jwxt.hbut.edu.cn\",\"sid\":\"20241002\",\"xhid\":\"\","
                "\"sem\":\"2026-2027-1\",\"cx\":0,\"jar\":[{\"n\":\"JSESSIONID\",\"v\":\"fake\","
                "\"d\":\"jwxt.hbut.edu.cn\",\"p\":\"/\",\"e\":0,\"h\":1,\"s\":1}]}";
            fwrite(sess, 1, strlen(sess), f);
            fclose(f);
        }
    }

    bool ok = p->on_load();
    printf("on_load: %s\n", ok ? "OK" : "FAIL");
    if (!ok) return 3;

    int sent = 0;
    PluginContext ctx;
    ctx.message_type = "private";
    ctx.user_id = 10001;
    ctx.sender_name = "tester";
    ctx.async_user_data = &sent;
    ctx.async_send_reply = on_send;
    ctx.async_log = on_log;

    const char* cases[] = {
        "/课表 帮助",
        "/课表",
        "/课表 5",
        "/课表 状态",
        "/课表 时间",
        "/课表 时间 3 10:20-11:05",
        "/课表 加 名称=自习 星期=3 节次=9 连堂=2 周数=1-16 教室=图书馆",
        "/课表 加 名称=缺参数",
        "/课表 改 1 教室=1教-301",
        "/课表 还原",
        "/课表 退出",
        "/hello",
    };
    for (const char* c : cases) {
        ctx.plain_text = c;
        ctx.raw_message = c;
        bool handled = p->on_message(ctx);
        printf("msg \"%s\" handled=%d\n", c, (int)handled);
        Sleep(300); // 让后台线程把回包打完
    }
    Sleep(1500);

    // ---- 群聊 @ 场景 ----
    printf("\n===== 群聊 @ 测试 =====\n");
    // 插件从运行目录的 config.json 读 bot_qq，这里临时造一份
    {
        FILE* f = fopen("config.json", "wb");
        if (f) {
            const char* cfg = "{\"onebot\":{\"bot_qq\":2374214151}}";
            fwrite(cfg, 1, strlen(cfg), f);
            fclose(f);
        }
    }

    PluginContext g = ctx;
    g.message_type = "group";
    g.group_id = 999000111;
    g.user_id = 10001;

    // 1) @ 一个没登录过的人
    g.plain_text = "/课表";
    g.at_user_ids = {10003};
    printf("-- @10003（未登录）\n");
    p->on_message(g);
    Sleep(600);

    // 2) @ 一个伪造过会话的人（会真的去请求教务，预期会话过期）
    g.plain_text = "/课表";
    g.at_user_ids = {10002};
    printf("-- @10002（有假会话）\n");
    p->on_message(g);
    Sleep(3000);

    // 3) @ 机器人自己 + /课表 → 应该查自己（此时自己也没登录）
    g.plain_text = "/课表";
    g.at_user_ids = {2374214151};
    printf("-- @bot + /课表\n");
    p->on_message(g);
    Sleep(600);

    // 4) 隐私开关
    g.plain_text = "/课表 隐私 关";
    g.at_user_ids = {2374214151};
    p->on_message(g);
    Sleep(300);
    g.plain_text = "/课表 隐私";
    p->on_message(g);
    Sleep(300);
    g.plain_text = "/课表 隐私 开";
    p->on_message(g);
    Sleep(300);

    // 5) 已关闭时别人来查
    g.plain_text = "/课表 隐私 关";
    p->on_message(g);
    Sleep(200);
    g.plain_text = "/课表";
    g.at_user_ids = {10001}; // @ 自己
    p->on_message(g);
    Sleep(400);

    // 6) 别人来查一个设了隐私的人（10001 上面已设「关」）
    g.user_id = 10003;
    g.plain_text = "/课表";
    g.at_user_ids = {10001};
    printf("-- 10003 @ 10001（对方已关隐私）\n");
    p->on_message(g);
    Sleep(400);

    // 7) 对方重新打开隐私后再查
    g.user_id = 10001;
    g.plain_text = "/课表 隐私 开";
    p->on_message(g);
    Sleep(200);
    g.user_id = 10003;
    g.plain_text = "/课表";
    g.at_user_ids = {10001};
    printf("-- 10003 @ 10001（已开隐私）\n");
    p->on_message(g);
    Sleep(400);

    remove("config.json");

    p->on_unload();
    destroy(p);
    // 不 FreeLibrary：插件内部有 detached 线程，卸载后立刻 Free 容易踩到已释放内存
    printf("sent=%d\n", sent);
    return 0;
}
