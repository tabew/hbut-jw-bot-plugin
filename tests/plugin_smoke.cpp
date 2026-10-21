// 插件冒烟测试：直接 LoadLibrary 加载 hbut_jw.dll，模拟几条消息，
// 确认导出符号、on_load、指令路由与异步回调都正常（不联网）。
// 用法：plugin_smoke.exe [插件目录]  默认 ..\
#include <windows.h>
#include <cstdio>
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

    p->on_unload();
    destroy(p);
    // 不 FreeLibrary：插件内部有 detached 线程，卸载后立刻 Free 容易踩到已释放内存
    printf("sent=%d\n", sent);
    return 0;
}
