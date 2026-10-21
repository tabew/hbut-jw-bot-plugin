#include "jw_render.hpp"
#include "jw_util.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace jw {
namespace {

ULONG_PTR g_token = 0;
bool g_started = false;

// ---------------- 版式参数 ----------------
constexpr float kWidth = 1420.0f;
constexpr float kMargin = 28.0f;
constexpr float kTitleH = 120.0f;
constexpr float kHeadH = 72.0f;
constexpr float kLeftW = 100.0f;   // 左侧节次 + 上课时间列宽
constexpr float kRowH = 74.0f;     // 每小节行高
constexpr float kFootH = 52.0f;
constexpr int kMinRows = 11;       // 湖工大默认 11 节
constexpr int kMaxRows = 13;

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
    {Color(255, 237, 248, 239), Color(255, 47, 140, 61), Color(255, 25, 92, 40), Color(255, 96, 148, 106)},
    {Color(255, 238, 244, 255), Color(255, 51, 92, 203), Color(255, 30, 58, 140), Color(255, 104, 124, 180)},
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
    const FontFamily* generic = FontFamily::GenericSansSerif();
    return generic ? generic->Clone() : nullptr;
}

void fill_round_rect(Graphics& g, const RectF& rc, float radius, const Color& fill) {
    GraphicsPath path;
    float d = radius * 2;
    if (d > rc.Height) d = rc.Height;
    if (d > rc.Width) d = rc.Width;
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
    if (!wrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), (INT)text.size(), &font, rc, &fmt, &brush);
}

// 单行文字（自动省略号），用于课程块内的教室 / 教师
void draw_single_line(Graphics& g, FontFamily* fam, const std::string& text, float size,
                      const Color& color, const RectF& rc) {
    if (text.empty() || rc.Height < size * 0.8f) return;
    Font font(fam, size, FontStyleRegular, UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentNear);
    fmt.SetLineAlignment(StringAlignmentNear);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    std::wstring w = wfromu8(text);
    g.DrawString(w.c_str(), (INT)w.size(), &font, rc, &fmt, &brush);
}

// 课程块：所有内容先 SetClip，再从可用高度里按「名称 → 教室 → 教师」优先级排布，
// 保证任何长度都不会画出格子。
void draw_course_card(Graphics& g, FontFamily* fam, const hbut::Course& c, const RectF& rc) {
    if (rc.Width < 24 || rc.Height < 24) return;

    Palette pal = palette_for(c.name);
    fill_round_rect(g, rc, 10.0f, pal.fill);
    fill_round_rect(g, RectF(rc.X, rc.Y + 6, 4.0f, rc.Height - 12), 2.0f, pal.accent);

    g.SetClip(RectF(rc.X + 1, rc.Y + 1, rc.Width - 2, rc.Height - 2));

    const float x = rc.X + 10.0f;
    const float w = rc.Width - 17.0f;
    const float top = rc.Y + 6.0f;
    const float bottom = rc.Y + rc.Height - 5.0f;

    // 窄格（同格多课分栏后）自动缩小字号，尽量多显示几个字
    const bool narrow = rc.Width < 110.0f;
    const float nameSize = narrow ? 13.5f : 15.5f;
    const float nameLineH = narrow ? 18.0f : 21.0f;
    const float infoSize = narrow ? 11.0f : 12.5f;
    const float infoH = narrow ? 14.0f : 16.0f;

    // ---- 课程名：最多两行 ----
    Font nameFont(fam, nameSize, FontStyleBold, UnitPixel);
    StringFormat nameFmt;
    nameFmt.SetAlignment(StringAlignmentNear);
    nameFmt.SetLineAlignment(StringAlignmentNear);
    nameFmt.SetTrimming(StringTrimmingEllipsisCharacter);
    nameFmt.SetFormatFlags(StringFormatFlagsLineLimit);
    SolidBrush nameBrush(pal.text);

    std::wstring wname = wfromu8(c.name);
    float nameH = nameLineH;
    {
        RectF bound;
        g.MeasureString(wname.c_str(), (INT)wname.size(), &nameFont, RectF(0, 0, w, 400), &nameFmt,
                        &bound);
        if (bound.Height > nameLineH * 1.35f) nameH = nameLineH * 2;
    }
    if (nameH > bottom - top) nameH = bottom - top;
    g.DrawString(wname.c_str(), (INT)wname.size(), &nameFont, RectF(x, top, w, nameH), &nameFmt,
                 &nameBrush);

    float y = top + nameH + (narrow ? 2.0f : 3.0f);

    // ---- 教室 ----
    std::string room = !c.room.empty() ? c.room : c.building;
    if (!room.empty() && bottom - y >= infoH) {
        draw_single_line(g, fam, room, infoSize, pal.sub, RectF(x, y, w, infoH));
        y += infoH + 0.5f;
    }
    // ---- 教师 ----
    if (!c.teacher.empty() && bottom - y >= infoH - 1.0f) {
        draw_single_line(g, fam, c.teacher, infoSize - 1.0f, pal.sub, RectF(x, y, w, infoH - 1.0f));
    }

    g.ResetClip();
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

    // 行数：至少 11 节，课程超出则自动加行
    int rows = kMinRows;
    for (const auto& c : data.courses) {
        int last = c.period + std::max(1, c.count) - 1;
        if (last > rows) rows = last;
    }
    if (rows > kMaxRows) rows = kMaxRows;

    const float gridTop = kMargin + kTitleH;
    const float headTop = gridTop;
    const float bodyTop = gridTop + kHeadH;
    const float gridLeft = kMargin;
    const float colLeft = gridLeft + kLeftW;
    const float gridW = kWidth - kMargin * 2 - kLeftW;
    const float colW = gridW / 7.0f;
    const float height = bodyTop + kRowH * rows + kFootH + kMargin;

    // ---- 同格重叠：先算每门课占第几列，再按「时间簇」分配宽度 ----
    // 只有真正互相重叠的那几门课才分栏，其余课程保持整列宽度。
    struct Placement {
        const hbut::Course* course = nullptr;
        int day = 0;
        int row = 0;
        int span = 1;
        int col = 0;
        int col_count = 1;
    };
    std::vector<Placement> placements;

    for (int day = 0; day < 7; ++day) {
        struct Item {
            const hbut::Course* c = nullptr;
            int r0 = 0;
            int span = 1;
            int col = 0;
        };
        std::vector<Item> items;
        for (const auto& c : data.courses) {
            if (c.weekday - 1 != day) continue;
            int r0 = c.period - 1;
            if (r0 < 0 || r0 >= rows) continue;
            int span = std::max(1, c.count);
            if (r0 + span > rows) span = rows - r0;
            items.push_back({&c, r0, span, 0});
        }
        if (items.empty()) continue;

        // 贪心分列：找第一个不与已占区间冲突的列
        std::vector<std::vector<std::pair<int, int>>> columns;
        for (auto& it : items) {
            int end = it.r0 + it.span - 1;
            int col = 0;
            while (true) {
                if (col >= (int)columns.size()) {
                    columns.resize(col + 1);
                    break;
                }
                bool clash = false;
                for (const auto& iv : columns[col]) {
                    if (!(end < iv.first || it.r0 > iv.second)) {
                        clash = true;
                        break;
                    }
                }
                if (!clash) break;
                ++col;
            }
            columns[col].push_back({it.r0, end});
            it.col = col;
        }

        // 并查集：时间重叠的课程属于同一个簇
        std::vector<int> parent(items.size());
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = (int)i;
        std::function<int(int)> find = [&](int x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        for (size_t i = 0; i < items.size(); ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                int a0 = items[i].r0, a1 = a0 + items[i].span - 1;
                int b0 = items[j].r0, b1 = b0 + items[j].span - 1;
                if (!(a1 < b0 || a0 > b1)) {
                    int ra = find((int)i), rb = find((int)j);
                    if (ra != rb) parent[ra] = rb;
                }
            }
        }
        std::map<int, int> cluster_cols;
        for (size_t i = 0; i < items.size(); ++i) {
            int root = find((int)i);
            auto it2 = cluster_cols.find(root);
            if (it2 == cluster_cols.end() || it2->second < items[i].col + 1) {
                cluster_cols[root] = items[i].col + 1;
            }
        }

        for (size_t i = 0; i < items.size(); ++i) {
            Placement p;
            p.course = items[i].c;
            p.day = day;
            p.row = items[i].r0;
            p.span = items[i].span;
            p.col = items[i].col;
            p.col_count = cluster_cols[find((int)i)];
            placements.push_back(p);
        }
    }

    Bitmap bmp((INT)kWidth, (INT)height, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

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
    draw_text(g, fam.get(), wfromu8("湖工大 · 课表"), 30.0f, FontStyleBold, Color(255, 24, 32, 44),
              RectF(kMargin + 26, kMargin + 12, 420, 40));

    std::string line2 = data.semester_label;
    if (!data.student_line.empty()) {
        if (!line2.empty()) line2 += "  ·  ";
        line2 += data.student_line;
    }
    draw_text(g, fam.get(), wfromu8(line2), 15.5f, FontStyleRegular, Color(255, 96, 106, 122),
              RectF(kMargin + 26, kMargin + 54, 760, 24));

    std::string line3 = data.date_range;
    if (data.total_weeks > 0) {
        if (!line3.empty()) line3 += "  ·  ";
        line3 += "共 " + std::to_string(data.total_weeks) + " 周";
    }
    if (!line3.empty()) {
        draw_text(g, fam.get(), wfromu8(line3), 13.5f, FontStyleRegular, Color(255, 148, 156, 168),
                  RectF(kMargin + 26, kMargin + 78, 760, 20));
    }

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "第 %d 周", data.week);
        RectF badge(kWidth - kMargin - 24 - 136, kMargin + 16, 136, 42);
        fill_round_rect(g, badge, 21.0f, Color(255, 232, 240, 254));
        draw_text(g, fam.get(), wfromu8(buf), 19.0f, FontStyleBold, Color(255, 30, 90, 180), badge,
                  StringAlignmentCenter);
    }

    // ---- 表头：星期 + 日期 ----
    static const char* kWeekNames[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    for (int d = 0; d < 7; ++d) {
        RectF cell(colLeft + d * colW + 4, headTop, colW - 8, kHeadH - 10);
        bool is_today = (data.today_weekday == d);
        if (is_today) fill_round_rect(g, cell, 12.0f, Color(255, 232, 240, 254));
        draw_text(g, fam.get(), wfromu8(kWeekNames[d]), 17.0f, FontStyleBold,
                  is_today ? Color(255, 30, 90, 180) : Color(255, 60, 68, 80),
                  RectF(cell.X, cell.Y + 9, cell.Width, 24), StringAlignmentCenter);
        if ((int)data.day_dates.size() > d) {
            const Date& dd = data.day_dates[(size_t)d];
            char buf[24];
            snprintf(buf, sizeof(buf), "%d/%d", dd.m, dd.d);
            draw_text(g, fam.get(), wfromu8(buf), 12.5f, FontStyleRegular,
                      is_today ? Color(255, 30, 90, 180) : Color(255, 148, 156, 168),
                      RectF(cell.X, cell.Y + 34, cell.Width, 20), StringAlignmentCenter);
        }
    }
    // 左上角：节次 / 时间
    draw_text(g, fam.get(), wfromu8("节次"), 12.0f, FontStyleRegular, Color(255, 168, 176, 188),
              RectF(gridLeft, headTop + 12, kLeftW - 12, 18), StringAlignmentCenter);
    draw_text(g, fam.get(), wfromu8("时间"), 12.0f, FontStyleRegular, Color(255, 168, 176, 188),
              RectF(gridLeft, headTop + 32, kLeftW - 12, 18), StringAlignmentCenter);

    // ---- 左侧节次 + 上课时间 ----
    const std::vector<PeriodTime>& times = data.period_times;
    for (int r = 0; r < rows; ++r) {
        float y = bodyTop + r * kRowH;
        draw_text(g, fam.get(), wfromu8(std::to_string(r + 1)), 15.0f, FontStyleBold,
                  Color(255, 120, 130, 145), RectF(gridLeft, y + 5, kLeftW - 12, 20),
                  StringAlignmentCenter);
        const PeriodTime* t = nullptr;
        for (const auto& item : times) {
            if (item.p == r + 1) {
                t = &item;
                break;
            }
        }
        if (t) {
            draw_text(g, fam.get(), wfromu8(t->start), 10.5f, FontStyleRegular,
                      Color(255, 152, 160, 172), RectF(gridLeft, y + 27, kLeftW - 12, 14),
                      StringAlignmentCenter);
            draw_text(g, fam.get(), wfromu8(t->end), 10.5f, FontStyleRegular,
                      Color(255, 176, 183, 194), RectF(gridLeft, y + 43, kLeftW - 12, 14),
                      StringAlignmentCenter);
        }
    }

    // ---- 网格 ----
    Pen line(Color(255, 231, 234, 240), 1.0f);
    for (int r = 0; r <= rows; ++r) {
        float y = bodyTop + r * kRowH;
        g.DrawLine(&line, PointF(gridLeft, y), PointF(colLeft + gridW, y));
    }
    for (int d = 0; d <= 7; ++d) {
        float x = colLeft + d * colW;
        g.DrawLine(&line, PointF(x, headTop + kHeadH - 6), PointF(x, bodyTop + kRowH * rows));
    }

    // ---- 课程块 ----
    for (const auto& p : placements) {
        const float gap = 5.0f;
        float dayW = colW - gap * 2;
        int cols = std::max(1, p.col_count);
        float subW = dayW / cols;
        float x = colLeft + p.day * colW + gap + subW * p.col;
        float y = bodyTop + p.row * kRowH + 4.0f;
        RectF rc(x, y, subW - (cols > 1 ? 3.0f : 0.0f), p.span * kRowH - 9.0f);
        draw_course_card(g, fam.get(), *p.course, rc);
    }

    if (data.courses.empty()) {
        draw_text(g, fam.get(), wfromu8("本周没有排课，好好休息 :)"), 18.0f, FontStyleRegular,
                  Color(255, 160, 168, 180), RectF(colLeft, bodyTop + kRowH * 3, gridW, 40),
                  StringAlignmentCenter);
    }

    // ---- 页脚 ----
    draw_text(g, fam.get(), wfromu8(data.footer), 13.0f, FontStyleRegular, Color(255, 165, 172, 184),
              RectF(kMargin + 4, bodyTop + kRowH * rows + 14, kWidth - kMargin * 2, 24),
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
