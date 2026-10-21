#include "jw_render.hpp"
#include "jw_util.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdio>
#include <memory>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace jw {
namespace {

ULONG_PTR g_token = 0;
bool g_started = false;

// 版式参数
constexpr float kWidth = 1420.0f;
constexpr float kMargin = 28.0f;
constexpr float kTitleH = 120.0f;
constexpr float kHeadH = 72.0f;
constexpr float kLeftW = 76.0f;
constexpr float kRowH = 68.0f;
constexpr int kRows = 12;
constexpr float kFootH = 52.0f;

struct Palette {
    Color fill;
    Color accent;
    Color text;
    Color sub;
};

const Palette kPalette[] = {
    {Color(255, 232, 240, 254), Color(255, 66, 133, 244), Color(255, 21, 62, 120), Color(255, 66, 103, 160)},
    {Color(255, 252, 233, 230), Color(255, 234, 88, 66), Color(255, 122, 44, 26), Color(255, 176, 92, 76)},
    {Color(255, 230, 244, 234), Color(255, 52, 168, 83), Color(255, 22, 96, 55), Color(255, 74, 140, 98)},
    {Color(255, 254, 247, 224), Color(255, 232, 170, 20), Color(255, 122, 82, 8), Color(255, 170, 128, 52)},
    {Color(255, 243, 232, 253), Color(255, 145, 86, 220), Color(255, 75, 40, 130), Color(255, 126, 96, 172)},
    {Color(255, 228, 247, 251), Color(255, 30, 154, 176), Color(255, 14, 90, 104), Color(255, 62, 132, 146)},
    {Color(255, 253, 237, 243), Color(255, 214, 78, 128), Color(255, 133, 33, 76), Color(255, 178, 96, 128)},
    {Color(255, 238, 241, 245), Color(255, 95, 110, 130), Color(255, 55, 65, 81), Color(255, 108, 118, 136)},
};
constexpr int kPaletteCount = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

Palette palette_for(const std::string& key) {
    unsigned hash = 2166136261u;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 16777619u;
    }
    return kPalette[hash % kPaletteCount];
}

int encoder_clsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return -1;
    std::unique_ptr<BYTE[]> buf(new BYTE[size]);
    ImageCodecInfo* codecs = (ImageCodecInfo*)buf.get();
    if (GetImageEncoders(num, size, codecs) != Ok) return -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *clsid = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

FontFamily* pick_family() {
    static const WCHAR* kCandidates[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei", L"SimSun"};
    for (const WCHAR* name : kCandidates) {
        FontFamily* fam = new FontFamily(name);
        if (fam && fam->IsAvailable()) return fam;
        delete fam;
    }
    return FontFamily::GenericSansSerif()->Clone();
}

void fill_round_rect(Graphics& g, const RectF& rc, float radius, const Color& fill) {
    GraphicsPath path;
    float d = radius * 2;
    path.AddArc(rc.X, rc.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
}

void draw_text(Graphics& g, FontFamily* fam, const std::wstring& text, float size, int style,
               const Color& color, const RectF& rc, StringAlignment align = StringAlignmentNear,
               StringAlignment line = StringAlignmentCenter, bool wrap = false) {
    Font font(fam, size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(line);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) {
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    }
    g.DrawString(text.c_str(), (INT)text.size(), &font, rc, &fmt, &brush);
}

} // namespace

bool gdiplus_init() {
    if (g_started) return true;
    GdiplusStartupInput input;
    if (GdiplusStartup(&g_token, &input, nullptr) != Ok) return false;
    g_started = true;
    return true;
}

void gdiplus_shutdown() {
    if (!g_started) return;
    GdiplusShutdown(g_token);
    g_started = false;
}

bool render_timetable_png(const TimetableImage& data, const std::string& out_path, std::string* err) {
    if (!gdiplus_init()) {
        if (err) *err = "GDI+ 初始化失败";
        return false;
    }

    const float gridTop = kMargin + kTitleH;
    const float headTop = gridTop;
    const float bodyTop = gridTop + kHeadH;
    const float height = bodyTop + kRowH * kRows + kFootH + kMargin;
    const float gridLeft = kMargin;
    const float colLeft = gridLeft + kLeftW;
    const float gridW = kWidth - kMargin * 2 - kLeftW;
    const float colW = gridW / 7.0f;

    Bitmap bmp((INT)kWidth, (INT)height, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // 背景
    SolidBrush bg(Color(255, 247, 248, 250));
    g.FillRectangle(&bg, RectF(0, 0, kWidth, height));

    std::unique_ptr<FontFamily> fam(pick_family());
    if (!fam) {
        if (err) *err = "字体加载失败";
        return false;
    }

    // ---- 顶部标题区 ----
    fill_round_rect(g, RectF(kMargin, kMargin, kWidth - kMargin * 2, kTitleH - 12), 14.0f,
                    Color(255, 255, 255, 255));

    draw_text(g, fam.get(), wfromu8("湖工大 · 课表"), 30.0f, FontStyleBold,
              Color(255, 24, 32, 44), RectF(kMargin + 26, kMargin + 12, 420, 40));

    std::string line2 = data.semester_label;
    if (!data.student_line.empty()) {
        if (!line2.empty()) line2 += "  ·  ";
        line2 += data.student_line;
    }
    draw_text(g, fam.get(), wfromu8(line2), 15.5f, FontStyleRegular, Color(255, 96, 106, 122),
              RectF(kMargin + 26, kMargin + 54, 700, 24));

    std::string line3 = data.date_range;
    if (data.total_weeks > 0) {
        if (!line3.empty()) line3 += "  ·  ";
        line3 += "共 " + std::to_string(data.total_weeks) + " 周";
    }
    if (!line3.empty()) {
        draw_text(g, fam.get(), wfromu8(line3), 13.5f, FontStyleRegular, Color(255, 148, 156, 168),
                  RectF(kMargin + 26, kMargin + 78, 700, 20));
    }

    // 周次徽标（右上）
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "第 %d 周", data.week);
        RectF badge(kWidth - kMargin - 24 - 136, kMargin + 16, 136, 42);
        fill_round_rect(g, badge, 21.0f, Color(255, 232, 240, 254));
        draw_text(g, fam.get(), wfromu8(buf), 19.0f, FontStyleBold, Color(255, 30, 90, 180), badge,
                  StringAlignmentCenter);
    }

    // ---- 表头 ----
    static const char* kWeekNames[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    for (int d = 0; d < 7; ++d) {
        RectF cell(colLeft + d * colW + 4, headTop, colW - 8, kHeadH - 10);
        bool is_today = (data.today_weekday == d);
        if (is_today) {
            fill_round_rect(g, cell, 12.0f, Color(255, 232, 240, 254));
        }
        draw_text(g, fam.get(), wfromu8(kWeekNames[d]), 17.0f, FontStyleBold,
                  is_today ? Color(255, 30, 90, 180) : Color(255, 60, 68, 80),
                  RectF(cell.X, cell.Y + 8, cell.Width, 26), StringAlignmentCenter);

        // 每列日期
        if ((int)data.day_dates.size() > d) {
            const Date& dd = data.day_dates[(size_t)d];
            char buf[32];
            snprintf(buf, sizeof(buf), "%d/%d", dd.m, dd.d);
            draw_text(g, fam.get(), wfromu8(buf), 13.0f, FontStyleRegular,
                      is_today ? Color(255, 30, 90, 180) : Color(255, 140, 148, 160),
                      RectF(cell.X, cell.Y + 34, cell.Width, 22), StringAlignmentCenter);
        }
    }

    // 左侧节次列
    for (int r = 0; r < kRows; ++r) {
        float y = bodyTop + r * kRowH;
        std::string label = std::to_string(r + 1);
        draw_text(g, fam.get(), wfromu8(label), 15.0f, FontStyleBold, Color(255, 150, 158, 170),
                  RectF(gridLeft, y, kLeftW - 8, kRowH), StringAlignmentCenter);
    }

    // ---- 网格线 ----
    Pen line(Color(255, 232, 235, 240), 1.0f);
    for (int r = 0; r <= kRows; ++r) {
        float y = bodyTop + r * kRowH;
        g.DrawLine(&line, PointF(colLeft, y), PointF(colLeft + gridW, y));
    }
    for (int d = 0; d <= 7; ++d) {
        float x = colLeft + d * colW;
        g.DrawLine(&line, PointF(x, headTop + kHeadH - 6), PointF(x, bodyTop + kRowH * kRows));
    }

    // ---- 课程块 ----
    for (const auto& c : data.courses) {
        int d = c.weekday - 1;
        int r0 = c.period - 1;
        if (d < 0 || d > 6 || r0 < 0 || r0 >= kRows) continue;
        int span = std::max(1, c.count);
        if (r0 + span > kRows) span = kRows - r0;

        RectF rc(colLeft + d * colW + 5, bodyTop + r0 * kRowH + 4, colW - 10, span * kRowH - 9);
        Palette pal = palette_for(c.name);
        fill_round_rect(g, rc, 10.0f, pal.fill);
        // 左侧竖条
        fill_round_rect(g, RectF(rc.X, rc.Y + 6, 4.0f, rc.Height - 12), 2.0f, pal.accent);

        float inner_x = rc.X + 12;
        float inner_w = rc.Width - 20;

        // 课程名（最多两行）
        draw_text(g, fam.get(), wfromu8(c.name), 16.0f, FontStyleBold, pal.text,
                  RectF(inner_x, rc.Y + 7, inner_w, 40), StringAlignmentNear, StringAlignmentNear, true);

        float info_y = rc.Y + 12 + 38;
        if (span >= 3) info_y = rc.Y + 52;

        std::string room = c.room;
        if (room.empty()) room = c.building;
        if (!room.empty() && info_y + 20 < rc.Y + rc.Height) {
            draw_text(g, fam.get(), wfromu8(room), 13.0f, FontStyleRegular, pal.sub,
                      RectF(inner_x, info_y, inner_w, 20), StringAlignmentNear, StringAlignmentNear, true);
            info_y += 19;
        }
        if (!c.teacher.empty() && info_y + 18 < rc.Y + rc.Height) {
            draw_text(g, fam.get(), wfromu8(c.teacher), 12.0f, FontStyleRegular, pal.sub,
                      RectF(inner_x, info_y, inner_w, 18), StringAlignmentNear, StringAlignmentNear, true);
        }
    }

    // 没有任何课
    if (data.courses.empty()) {
        draw_text(g, fam.get(), wfromu8("本周没有排课，好好休息 :)"), 18.0f, FontStyleRegular,
                  Color(255, 160, 168, 180),
                  RectF(colLeft, bodyTop + kRowH * 4, gridW, 40), StringAlignmentCenter);
    }

    // ---- 页脚 ----
    draw_text(g, fam.get(), wfromu8(data.footer), 13.0f, FontStyleRegular, Color(255, 165, 172, 184),
              RectF(kMargin + 4, bodyTop + kRowH * kRows + 14, kWidth - kMargin * 2, 24),
              StringAlignmentNear);

    // ---- 保存 ----
    CLSID png_clsid;
    if (encoder_clsid(L"image/png", &png_clsid) < 0) {
        if (err) *err = "找不到 PNG 编码器";
        return false;
    }
    std::wstring wpath = wfromu8(out_path);
    Status st = bmp.Save(wpath.c_str(), &png_clsid, nullptr);
    if (st != Ok) {
        if (err) *err = "图片保存失败（Status=" + std::to_string((int)st) + "）";
        return false;
    }
    return true;
}

} // namespace jw
