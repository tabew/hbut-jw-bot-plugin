#include "jw_render.hpp"
#include "jw_util.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace jw {
namespace {

ULONG_PTR g_token = 0;
bool g_started = false;

// ---------------- 版式参数（对齐 mini-hbut 的课表视觉） ----------------
constexpr float kWidth = 1000.0f;
constexpr float kMargin = 20.0f;
constexpr float kInfoH = 100.0f;     // 顶部信息条
constexpr float kDateH = 92.0f;      // 日期头
constexpr float kAxisW = 92.0f;      // 左侧时间轴宽度
constexpr float kRowH = 100.0f;      // 每节行高
constexpr float kFootH = 40.0f;
constexpr int kMinRows = 11;
constexpr int kMaxRows = 13;

// mini-hbut constants.ts 的 courseThemes（12 组）
struct CourseColor {
    Color bg;
    Color text;
    Color border;
};
const CourseColor kThemes[] = {
    {Color(255, 231, 244, 255), Color(255, 15, 93, 168), Color(255, 114, 185, 255)},
    {Color(255, 255, 240, 232), Color(255, 203, 79, 47), Color(255, 255, 179, 144)},
    {Color(255, 239, 233, 255), Color(255, 95, 82, 207), Color(255, 184, 170, 255)},
    {Color(255, 255, 244, 219), Color(255, 190, 122, 7), Color(255, 239, 196, 101)},
    {Color(255, 255, 234, 242), Color(255, 195, 63, 115), Color(255, 243, 168, 196)},
    {Color(255, 232, 250, 245), Color(255, 17, 127, 103), Color(255, 138, 220, 196)},
    {Color(255, 232, 239, 255), Color(255, 51, 92, 203), Color(255, 158, 180, 255)},
    {Color(255, 255, 241, 245), Color(255, 182, 63, 88), Color(255, 240, 172, 187)},
    {Color(255, 237, 248, 239), Color(255, 47, 140, 61), Color(255, 157, 215, 167)},
    {Color(255, 232, 249, 255), Color(255, 0, 120, 147), Color(255, 132, 214, 236)},
    {Color(255, 244, 237, 255), Color(255, 117, 72, 193), Color(255, 198, 173, 241)},
    {Color(255, 255, 242, 226), Color(255, 176, 92, 22), Color(255, 239, 182, 127)},
};
constexpr int kThemeCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));

CourseColor theme_for(const std::string& key) {
    unsigned hash = 2166136261u;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 16777619u;
    }
    return kThemes[hash % kThemeCount];
}

// 面板配色（mini-hbut 的 grid 用色）
const Color kPageBg(255, 255, 255, 255);
const Color kAxisBg(255, 249, 250, 251);
const Color kLineSoft(255, 240, 240, 240);
const Color kGridDash(255, 229, 231, 235);
const Color kTextStrong(255, 17, 24, 39);
const Color kTextMuted(255, 156, 163, 175);
const Color kAxisTimeStart(255, 148, 163, 184);
const Color kAxisTimeEnd(255, 203, 213, 225);
const Color kAxisPeriod(255, 51, 65, 85);
const Color kTodayBg(255, 219, 234, 254);
const Color kTodayText(255, 37, 99, 235);

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

void stroke_round_rect(Graphics& g, const RectF& rc, float radius, const Color& color, float width) {
    GraphicsPath path;
    float d = radius * 2;
    if (d > rc.Height) d = rc.Height;
    if (d > rc.Width) d = rc.Width;
    // 内缩半个线宽，避免描边糊到相邻格
    RectF r(rc.X + width / 2, rc.Y + width / 2, rc.Width - width, rc.Height - width);
    d = radius * 2;
    if (d > r.Height) d = r.Height;
    if (d > r.Width) d = r.Width;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
    Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

void draw_text(Graphics& g, FontFamily* fam, const std::wstring& text, float size, int style,
               const Color& color, const RectF& rc, StringAlignment align = StringAlignmentNear,
               StringAlignment line = StringAlignmentCenter, bool nowrap = true) {
    Font font(fam, size, style, UnitPixel);
    SolidBrush brush(color);
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(line);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    if (nowrap) fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), (INT)text.size(), &font, rc, &fmt, &brush);
}

// 课程卡：迷你圆角卡片 + 居中排版，内容严格裁剪在卡片内（对齐 mini-hbut course-card）
void draw_course_card(Graphics& g, FontFamily* fam, const hbut::Course& c, const RectF& rc) {
    if (rc.Width < 26 || rc.Height < 26) return;

    CourseColor cc = theme_for(c.name);
    fill_round_rect(g, rc, 14.0f, cc.bg);
    stroke_round_rect(g, rc, 14.0f, cc.border, 1.2f);

    g.SetClip(RectF(rc.X + 1.5f, rc.Y + 1.5f, rc.Width - 3.0f, rc.Height - 3.0f));

    const bool narrow = rc.Width < 100.0f;
    const float pad = narrow ? 5.0f : 8.0f;
    const float innerW = rc.Width - pad * 2;
    const float nameSize = narrow ? 15.5f : 19.0f;
    const float nameLineH = narrow ? 21.0f : 26.0f;
    const float infoSize = narrow ? 11.0f : 14.0f;
    const float infoLineH = narrow ? 15.0f : 18.0f;

    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    center.SetTrimming(StringTrimmingEllipsisCharacter);
    center.SetFormatFlags(StringFormatFlagsLineLimit);

    // 名称：最多两行
    Font nameFont(fam, nameSize, FontStyleBold, UnitPixel);
    SolidBrush nameBrush(cc.text);
    std::wstring wname = wfromu8(c.name);
    float nameH = nameLineH;
    {
        RectF bound;
        g.MeasureString(wname.c_str(), (INT)wname.size(), &nameFont, RectF(0, 0, innerW, 400),
                        &center, &bound);
        if (bound.Height > nameLineH * 1.35f) nameH = nameLineH * 2;
    }
    const float maxNameH = rc.Height - pad * 2 - infoLineH; // 至少给教室留一行
    if (nameH > maxNameH) nameH = std::max(nameLineH, maxNameH);

    // 教室 / 教师（按空间取舍）
    std::string room = !c.room.empty() ? c.room : c.building;
    bool showRoom = !room.empty();
    bool showTeacher = !c.teacher.empty();
    float infoH = 0.0f;
    if (showRoom) infoH += infoLineH;
    if (showTeacher && rc.Height - pad * 2 - nameH >= infoLineH * 2) infoH += infoLineH;
    else showTeacher = false;

    float total = nameH + infoH;
    float y = rc.Y + (rc.Height - total) / 2.0f;

    g.DrawString(wname.c_str(), (INT)wname.size(), &nameFont, RectF(rc.X + pad, y, innerW, nameH),
                 &center, &nameBrush);
    y += nameH;

    Color subColor(255, cc.text.GetR(), cc.text.GetG(), cc.text.GetB());
    if (showRoom) {
        Font rf(fam, infoSize, FontStyleRegular, UnitPixel);
        SolidBrush rb(subColor);
        std::wstring wroom = wfromu8(room);
        g.DrawString(wroom.c_str(), (INT)wroom.size(), &rf,
                     RectF(rc.X + pad, y, innerW, infoLineH), &center, &rb);
        y += infoLineH;
    }
    if (showTeacher) {
        Font tf(fam, infoSize - 0.5f, FontStyleRegular, UnitPixel);
        SolidBrush tb(Color(205, cc.text.GetR(), cc.text.GetG(), cc.text.GetB()));
        std::wstring wt = wfromu8(c.teacher);
        g.DrawString(wt.c_str(), (INT)wt.size(), &tf, RectF(rc.X + pad, y, innerW, infoLineH),
                     &center, &tb);
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

    // ---- 行数：至少 11 节，课程超出自动加行 ----
    int rows = kMinRows;
    for (const auto& c : data.courses) {
        int last = c.period + std::max(1, c.count) - 1;
        if (last > rows) rows = last;
    }
    if (rows > kMaxRows) rows = kMaxRows;

    const float gridLeft = kMargin;
    const float axisRight = gridLeft + kAxisW;
    const float dateTop = kMargin + kInfoH;
    const float bodyTop = dateTop + kDateH;
    const float gridW = kWidth - kMargin * 2 - kAxisW;
    const float colW = gridW / 7.0f;
    const float height = bodyTop + kRowH * rows + kFootH + kMargin;

    // ---- 同格重叠：时间簇分栏（只有真正重叠的课程才变窄） ----
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
            auto itc = cluster_cols.find(root);
            if (itc == cluster_cols.end() || itc->second < items[i].col + 1) {
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

    // ---- 画布 ----
    Bitmap bmp((INT)kWidth, (INT)height, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    SolidBrush pageBg(kPageBg);
    g.FillRectangle(&pageBg, RectF(0, 0, kWidth, height));

    std::unique_ptr<FontFamily> fam(pick_family());
    if (!fam) {
        if (err) *err = "字体加载失败";
        return false;
    }

    // ---- 顶部信息条 ----
    draw_text(g, fam.get(), wfromu8("湖工大 · 课表"), 26.0f, FontStyleBold, kTextStrong,
              RectF(kMargin, kMargin + 8, 420, 36));

    std::string sub = data.semester_label;
    if (!data.student_line.empty()) {
        if (!sub.empty()) sub += "   ";
        sub += data.student_line;
    }
    draw_text(g, fam.get(), wfromu8(sub), 13.0f, FontStyleRegular, kTextMuted,
              RectF(kMargin, kMargin + 48, 700, 22));

    if (!data.date_range.empty()) {
        draw_text(g, fam.get(), wfromu8(data.date_range), 13.0f, FontStyleRegular, kTextMuted,
                  RectF(kMargin, kMargin + 72, 700, 20));
    }

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "第 %d 周", data.week);
        std::string weekText = buf;
        if (data.total_weeks > 0) weekText += " / 共 " + std::to_string(data.total_weeks) + " 周";
        RectF badge(kWidth - kMargin - 200, kMargin + 12, 200, 38);
        fill_round_rect(g, badge, 18.0f, Color(255, 239, 246, 255));
        draw_text(g, fam.get(), wfromu8(weekText), 15.0f, FontStyleBold, kTodayText, badge,
                  StringAlignmentCenter);
    }

    // ---- 日期头 ----
    SolidBrush axisBg(kAxisBg);
    g.FillRectangle(&axisBg, RectF(gridLeft, dateTop, kAxisW, kDateH));

    // 月列
    if ((int)data.day_dates.size() >= 1) {
        const Date& first = data.day_dates[0];
        char mbuf[16];
        snprintf(mbuf, sizeof(mbuf), "%d月", first.m);
        draw_text(g, fam.get(), wfromu8(mbuf), 21.0f, FontStyleBold, kTextStrong,
                  RectF(gridLeft, dateTop + 22, kAxisW, 28), StringAlignmentCenter);
        draw_text(g, fam.get(), wfromu8("本周"), 12.0f, FontStyleRegular, kTextMuted,
                  RectF(gridLeft, dateTop + 52, kAxisW, 18), StringAlignmentCenter);
    }

    static const char* kWeekNames[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    for (int d = 0; d < 7; ++d) {
        float x = axisRight + d * colW;
        bool is_today = (data.today_weekday == d);
        RectF cell(x + 2, dateTop + 4, colW - 4, kDateH - 10);
        if (is_today) {
            fill_round_rect(g, cell, 10.0f, kTodayBg);
        }
        char dbuf[16];
        if ((int)data.day_dates.size() > d) {
            snprintf(dbuf, sizeof(dbuf), "%d", data.day_dates[(size_t)d].d);
        } else {
            snprintf(dbuf, sizeof(dbuf), "%d", d + 1);
        }
        draw_text(g, fam.get(), wfromu8(dbuf), 24.0f, is_today ? FontStyleBold : FontStyleRegular,
                  is_today ? kTodayText : kTextStrong, RectF(x, dateTop + 14, colW, 30),
                  StringAlignmentCenter);
        draw_text(g, fam.get(), wfromu8(kWeekNames[d]), 13.5f, FontStyleRegular,
                  is_today ? kTodayText : kTextMuted, RectF(x, dateTop + 48, colW, 20),
                  StringAlignmentCenter);
    }

    // 日期头下边线
    {
        Pen pen(kLineSoft, 1.0f);
        g.DrawLine(&pen, PointF(gridLeft, bodyTop), PointF(gridLeft + kAxisW + gridW, bodyTop));
    }

    // ---- 今天列淡蓝底 ----
    if (data.today_weekday >= 0 && data.today_weekday < 7) {
        SolidBrush todayBg(Color(46, 219, 234, 254));
        float x = axisRight + data.today_weekday * colW;
        g.FillRectangle(&todayBg, RectF(x, bodyTop, colW, kRowH * rows));
        Pen edge(Color(30, 37, 99, 235), 1.0f);
        g.DrawLine(&edge, PointF(x, bodyTop), PointF(x, bodyTop + kRowH * rows));
        g.DrawLine(&edge, PointF(x + colW, bodyTop), PointF(x + colW, bodyTop + kRowH * rows));
    }

    // ---- 左侧时间轴（时间 / 节次 / 时间） ----
    g.FillRectangle(&axisBg, RectF(gridLeft, bodyTop, kAxisW, kRowH * rows));
    {
        Pen right(kLineSoft, 1.0f);
        g.DrawLine(&right, PointF(axisRight, bodyTop), PointF(axisRight, bodyTop + kRowH * rows));
    }

    const std::vector<PeriodTime>& times = data.period_times;
    for (int r = 0; r < rows; ++r) {
        float y = bodyTop + r * kRowH;
        const PeriodTime* t = nullptr;
        for (const auto& item : times) {
            if (item.p == r + 1) {
                t = &item;
                break;
            }
        }
        if (t) {
            draw_text(g, fam.get(), wfromu8(t->start), 12.0f, FontStyleRegular, kAxisTimeStart,
                      RectF(gridLeft, y + 18, kAxisW - 6, 16), StringAlignmentCenter);
        }
        draw_text(g, fam.get(), wfromu8(std::to_string(r + 1)), 20.0f, FontStyleBold, kAxisPeriod,
                  RectF(gridLeft, y + 37, kAxisW - 6, 26), StringAlignmentCenter);
        if (t) {
            draw_text(g, fam.get(), wfromu8(t->end), 12.0f, FontStyleRegular, kAxisTimeEnd,
                      RectF(gridLeft, y + 64, kAxisW - 6, 16), StringAlignmentCenter);
        }
    }

    // ---- 横向虚线（8px 实 / 8px 空，同 mini-hbut 的 dashed 视觉） ----
    {
        Pen dash(kGridDash, 1.0f);
        REAL pattern[] = {8.0f, 8.0f};
        dash.SetDashPattern(pattern, 2);
        for (int r = 1; r < rows; ++r) {
            float y = bodyTop + r * kRowH;
            g.DrawLine(&dash, PointF(axisRight, y), PointF(axisRight + gridW, y));
        }
    }

    // ---- 课程卡 ----
    for (const auto& p : placements) {
        const float gap = 3.0f;
        float dayW = colW - gap * 2;
        int cols = std::max(1, p.col_count);
        float subW = dayW / cols;
        float x = axisRight + p.day * colW + gap + subW * p.col;
        float y = bodyTop + p.row * kRowH + 3.0f;
        RectF rc(x, y, subW - (cols > 1 ? 2.0f : 0.0f), p.span * kRowH - 6.0f);
        draw_course_card(g, fam.get(), *p.course, rc);
    }

    if (data.courses.empty()) {
        draw_text(g, fam.get(), wfromu8("本周没有排课  :)"), 19.0f, FontStyleRegular, kTextMuted,
                  RectF(axisRight, bodyTop + kRowH * 3, gridW, 36), StringAlignmentCenter);
    }

    // ---- 页脚 ----
    draw_text(g, fam.get(), wfromu8(data.footer), 12.5f, FontStyleRegular, kTextMuted,
              RectF(kMargin, bodyTop + kRowH * rows + 10, kWidth - kMargin * 2, 22),
              StringAlignmentCenter);

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
