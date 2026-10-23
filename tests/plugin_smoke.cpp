// 插件冒烟测试：直接 LoadLibrary 加载 hbut_jw.dll，模拟几条消息，
// 确认导出符号、on_load、指令路由与异步回调都正常（不联网）。
// 用法：plugin_smoke.exe [插件目录]  默认 ..\
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "plugin_manager.hpp"

static void on_send(void* user_data, const char* message_type, int64_t target, const char* text) {
    printf("[%s -> %lld] %s\n", message_type, (long long)target, text);
    if (user_data) *(int*)user_data += 1;
}

static void on_log(void* user_data, const char* text) {
    printf("[log] %s\n", text);
}


// 写一份假的课表缓存，模拟「以前登录过、现在没有会话」
static void write_fake_cache(const char* path) {
    char buf[4096];
    long long ts = (long long)time(nullptr) * 1000 - 3600LL * 1000;
    snprintf(buf, sizeof(buf),
             "{\"sem\":\"2026-2027-1\",\"base\":\"https://jwxt.hbut.edu.cn\",\"ts\":%lld,"
             "\"cal\":{\"total\":20,\"cur\":8,\"school\":1,\"weeks\":["
             "{\"w\":7,\"mon\":\"2026-10-12\",\"sun\":\"2026-10-18\"},"
             "{\"w\":8,\"mon\":\"2026-10-19\",\"sun\":\"2026-10-25\"},"
             "{\"w\":9,\"mon\":\"2026-10-26\",\"sun\":\"2026-11-01\"}]},"
             "\"courses\":["
             "{\"n\":\"高等数学\",\"t\":\"李明\",\"r\":\"1-301\",\"wd\":1,\"p\":1,\"bs\":1,\"cnt\":1,"
             "\"wk\":[7,8,9]},"
             "{\"n\":\"大学物理\",\"t\":\"王芳\",\"r\":\"2-105\",\"wd\":3,\"p\":5,\"bs\":3,\"cnt\":1,"
             "\"wk\":[8]},"
             "{\"n\":\"电路理论\",\"t\":\"涂静敏\",\"r\":\"3-102\",\"wd\":5,\"p\":7,\"bs\":4,\"cnt\":1,"
             "\"wk\":[8]}]}",
             ts);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, strlen(buf), f);
        fclose(f);
    }
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

    // ---- 本地课表缓存（只登录一次就能一直看）----
    printf("\n===== 缓存测试 =====\n");
    write_fake_cache("data/hbut_jw/cache/10001.json");

    ctx.plain_text = "/课表";
    printf("-- 无会话但有缓存：/课表\n");
    p->on_message(ctx);
    Sleep(1500);

    ctx.plain_text = "/课表 状态";
    printf("-- /课表 状态（应能看到缓存条数）\n");
    p->on_message(ctx);
    Sleep(600);

    ctx.plain_text = "/课表 第8周";
    printf("-- /课表 第8周（缓存里 current=8）\n");
    p->on_message(ctx);
    Sleep(1200);

    ctx.plain_text = "/课表 清缓存";
    printf("-- /课表 清缓存\n");
    p->on_message(ctx);
    Sleep(300);

    ctx.plain_text = "/课表";
    printf("-- 清缓存后再查（应提示没有缓存）\n");
    p->on_message(ctx);
    Sleep(800);

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
