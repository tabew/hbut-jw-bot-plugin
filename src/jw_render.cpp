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

// ---------------- 版式参数（对照 HBUT 手机端课表：6 大节，每大节 2 小节） ----------------
constexpr float kWidth = 1000.0f;
constexpr float kPad = 16.0f;
constexpr float kHeadH = 158.0f;      // 顶部绿色区（标题 + 日期头）
constexpr float kDateH = 92.0f;       // 日期头（在 kHeadH 内）
constexpr float kAxisW = 84.0f;       // 左侧时间轴
constexpr float kRowH = 168.0f;       // 每大节行高
constexpr float kFootH = 36.0f;
constexpr int kPeriodsPerRow = 2;     // 一大节 = 2 小节

// 主色（对照截图的青绿色头）
const Color kBrand(255, 67, 198, 165);
const Color kBrandDark(255, 30, 150, 120);
const Color kPageBg(255, 255, 255, 255);
const Color kAxisBg(255, 250, 251, 252);
const Color kLineSoft(255, 236, 238, 241);
const Color kGridDash(255, 226, 229, 234);
const Color kTextStrong(255, 24, 32, 44);
const Color kTextMuted(255, 140, 148, 160);
const Color kAxisTime(255, 160, 168, 180);

// 课程卡配色：饱和底 + 白字（与手机端一致）
const Color kCardColors[] = {
    Color(255, 76, 143, 208),  // 蓝
    Color(255, 224, 169, 59),  // 金
    Color(255, 86, 180, 91),   // 绿
    Color(255, 154, 106, 207), // 紫
    Color(255, 228, 114, 154), // 粉
    Color(255, 69, 183, 196),  // 青
    Color(255, 238, 142, 142), // 浅红
    Color(255, 124, 107, 214), // 靛
    Color(255, 217, 130, 75),  // 橙
    Color(255, 90, 169, 160),  // 青绿
};
constexpr int kCardColorCount = (int)(sizeof(kCardColors) / sizeof(kCardColors[0]));

Color card_color_for(const std::string& key) {
    unsigned hash = 2166136261u;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 16777619u;
    }
    return kCardColors[hash % kCardColorCount];
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

// 课程卡：饱和色圆角块 + 白色居中文字，内容全部裁剪在卡片内
void draw_course_card(Graphics& g, FontFamily* fam, const hbut::Course& c, const RectF& rc) {
    if (rc.Width < 24 || rc.Height < 24) return;

    fill_round_rect(g, rc, 9.0f, card_color_for(c.name));

    g.SetClip(RectF(rc.X + 1, rc.Y + 1, rc.Width - 2, rc.Height - 2));

    const bool narrow = rc.Width < 100.0f;
    const float pad = narrow ? 5.0f : 7.0f;
    const float innerW = rc.Width - pad * 2;
    const float nameSize = narrow ? 15.0f : 17.5f;
    const float nameLineH = narrow ? 20.0f : 24.0f;
    const float infoSize = narrow ? 11.5f : 13.0f;
    const float infoLineH = narrow ? 15.0f : 18.0f;

    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    center.SetTrimming(StringTrimmingEllipsisCharacter);
    center.SetFormatFlags(StringFormatFlagsLineLimit);

    Font nameFont(fam, nameSize, FontStyleBold, UnitPixel);
    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush softWhite(Color(235, 255, 255, 255));

    std::wstring wname = wfromu8(c.name);
    float nameH = nameLineH;
    {
        RectF bound;
        g.MeasureString(wname.c_str(), (INT)wname.size(), &nameFont, RectF(0, 0, innerW, 600),
                        &center, &bound);
        int lines = (int)(bound.Height / (nameLineH * 1.15f) + 0.45f);
        if (lines < 1) lines = 1;
        if (lines > 3) lines = 3;
        nameH = nameLineH * lines;
    }

    std::string room = !c.room.empty() ? c.room : c.building;
    bool showRoom = !room.empty();
    bool showTeacher = !c.teacher.empty();
    float infoH = (showRoom ? infoLineH : 0.0f) + (showTeacher ? infoLineH : 0.0f);

    // 空间不够时按「教师 → 教室」顺序舍弃
    float avail = rc.Height - pad * 2;
    if (nameH + infoH > avail) {
        if (showTeacher) {
            showTeacher = false;
            infoH = showRoom ? infoLineH : 0.0f;
        }
        if (nameH + infoH > avail) {
            showRoom = false;
            infoH = 0.0f;
        }
    }
    if (nameH > avail - infoH) nameH = std::max(nameLineH, avail - infoH);

    float total = nameH + infoH;
    float y = rc.Y + (rc.Height - total) / 2.0f;

    g.DrawString(wname.c_str(), (INT)wname.size(), &nameFont, RectF(rc.X + pad, y, innerW, nameH),
                 &center, &white);
    y += nameH;

    if (showRoom) {
        Font rf(fam, infoSize, FontStyleRegular, UnitPixel);
        std::wstring wroom = wfromu8(room);
        g.DrawString(wroom.c_str(), (INT)wroom.size(), &rf,
                     RectF(rc.X + pad, y, innerW, infoLineH), &center, &softWhite);
        y += infoLineH;
    }
    if (showTeacher) {
        Font tf(fam, infoSize - 0.5f, FontStyleRegular, UnitPixel);
        std::wstring wt = wfromu8(c.teacher);
        g.DrawString(wt.c_str(), (INT)wt.size(), &tf, RectF(rc.X + pad, y, innerW, infoLineH),
                     &center, &softWhite);
    }

    g.ResetClip();
}

// 小节课次 → 大节行（0 基）
int big_row_of(int period) {
    if (period < 1) period = 1;
    return (period - 1) / kPeriodsPerRow;
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

    const std::vector<PeriodTime>& times = data.period_times;

    // 小节总数：默认 11 节，作息表更长时以作息表为准
    int totalPeriods = 11;
    for (const auto& t : times) {
        if (t.p > totalPeriods) totalPeriods = t.p;
    }
    int rows = (totalPeriods + kPeriodsPerRow - 1) / kPeriodsPerRow;
    if (rows < 1) rows = 1;
    if (rows > 7) rows = 7;

    const float gridLeft = kPad;
    const float axisRight = gridLeft + kAxisW;
    const float dateTop = kHeadH - kDateH;
    const float bodyTop = kHeadH;
    const float gridW = kWidth - gridLeft - kPad - kAxisW;
    const float colW = gridW / 7.0f;
    const float height = bodyTop + kRowH * rows + kFootH + kPad;

    // ---- 重叠分栏：把课程换算到大节行，再按时间簇分栏 ----
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
            // 优先用接口给的大节号 djs；没有才按小节折算
            int r0 = c.big_section > 0 ? c.big_section - 1 : big_row_of(c.period);
            if (r0 < 0 || r0 >= rows) continue;
            // 教务一条记录就是一个大节（count=1）；自定义课程可用 count 跨多行
            int span = std::max(1, c.count);
            if (r0 + span > rows) span = rows - r0;
            if (span < 1) span = 1;

            // 教务有时把一门连堂课拆成多条小节记录（如第1节、第2节各一条）。
            // 同名课程在同一大节内：已被覆盖就跳过，首尾相接就合并，避免同格被劈成两半。
            bool skip = false;
            for (auto& it : items) {
                if (it.c->name != c.name) continue;
                int itEnd = it.r0 + it.span - 1;
                if (r0 >= it.r0 && r0 <= itEnd) {
                    skip = true;
                    break;
                }
                if (r0 == itEnd + 1) {
                    it.span += span;
                    skip = true;
                    break;
                }
            }
            if (skip) continue;

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

    // ---- 顶部绿色头 ----
    {
        SolidBrush brand(kBrand);
        g.FillRectangle(&brand, RectF(0, 0, kWidth, kHeadH));

        const Color white(255, 255, 255, 255);
        const Color whiteDim(230, 255, 255, 255);

        draw_text(g, fam.get(), wfromu8("湖工大 · 课表"), 22.0f, FontStyleBold, white,
                  RectF(gridLeft, 14, 320, 30), StringAlignmentNear, StringAlignmentCenter);

        std::string sub = data.semester_label;
        if (!data.student_line.empty()) {
            if (!sub.empty()) sub += "  ·  ";
            sub += data.student_line;
        }
        draw_text(g, fam.get(), wfromu8(sub), 12.0f, FontStyleRegular, whiteDim,
                  RectF(gridLeft, 44, 700, 18));

        if (!data.date_range.empty()) {
            draw_text(g, fam.get(), wfromu8(data.date_range), 11.5f, FontStyleRegular, whiteDim,
                      RectF(gridLeft, 63, 700, 16));
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "第 %d 周", data.week);
        std::string weekText = buf;
        if (data.total_weeks > 0) weekText += " / " + std::to_string(data.total_weeks);
        RectF badge(kWidth - kPad - 150, 16, 150, 32);
        fill_round_rect(g, badge, 16.0f, Color(66, 255, 255, 255));
        draw_text(g, fam.get(), wfromu8(weekText), 13.0f, FontStyleBold, white, badge,
                  StringAlignmentCenter);

        // 日期头：左「月」+ 7 天
        if (!data.day_dates.empty()) {
            char mbuf[16];
            snprintf(mbuf, sizeof(mbuf), "%d月", data.day_dates[0].m);
            draw_text(g, fam.get(), wfromu8(mbuf), 13.0f, FontStyleRegular, whiteDim,
                      RectF(gridLeft, dateTop + 8, kAxisW, 30), StringAlignmentCenter);
            draw_text(g, fam.get(), wfromu8("节次"), 11.0f, FontStyleRegular,
                      Color(210, 255, 255, 255), RectF(gridLeft, dateTop + 40, kAxisW, 16),
                      StringAlignmentCenter);
        }

        static const char* kWeekShort[] = {"一", "二", "三", "四", "五", "六", "日"};
        for (int d = 0; d < 7; ++d) {
            float x = axisRight + d * colW;
            bool is_today = (data.today_weekday == d);
            char dbuf[16];
            if ((int)data.day_dates.size() > d) {
                snprintf(dbuf, sizeof(dbuf), "%d", data.day_dates[(size_t)d].d);
            } else {
                snprintf(dbuf, sizeof(dbuf), "%d", d + 1);
            }
            if (is_today) {
                RectF circle(x + colW / 2 - 17, dateTop + 6, 34, 34);
                fill_round_rect(g, circle, 17.0f, Color(255, 255, 255, 255));
                draw_text(g, fam.get(), wfromu8(dbuf), 17.0f, FontStyleBold, kBrandDark, circle,
                          StringAlignmentCenter);
            } else {
                draw_text(g, fam.get(), wfromu8(dbuf), 17.0f, FontStyleBold, white,
                          RectF(x, dateTop + 8, colW, 30), StringAlignmentCenter);
            }
            draw_text(g, fam.get(), wfromu8(kWeekShort[d]), 12.0f, FontStyleRegular,
                      is_today ? white : Color(225, 255, 255, 255),
                      RectF(x, dateTop + 48, colW, 18), StringAlignmentCenter);
        }
    }

    // ---- 左侧时间轴（大节号 / 上课 / 下课） ----
    {
        SolidBrush axisBg(kAxisBg);
        g.FillRectangle(&axisBg, RectF(gridLeft, bodyTop, kAxisW, kRowH * rows));
        Pen right(kLineSoft, 1.0f);
        g.DrawLine(&right, PointF(axisRight, bodyTop), PointF(axisRight, bodyTop + kRowH * rows));
    }

    // 08:20 → 8:20（手机端不带前导零）
    auto short_clock = [](std::string v) {
        if (v.size() == 5 && v[0] == '0') v.erase(0, 1);
        return v;
    };

    auto time_of = [&times](int p) -> const PeriodTime* {
        for (const auto& t : times) {
            if (t.p == p) return &t;
        }
        return nullptr;
    };

    for (int r = 0; r < rows; ++r) {
        float y = bodyTop + r * kRowH;
        int first = r * kPeriodsPerRow + 1;
        int last = first + kPeriodsPerRow - 1;
        const PeriodTime* t1 = time_of(first);
        const PeriodTime* t2 = time_of(last);
        std::string start = t1 ? short_clock(t1->start) : std::string();
        std::string end = t2 ? short_clock(t2->end) : (t1 ? short_clock(t1->end) : std::string());

        draw_text(g, fam.get(), wfromu8(std::to_string(r + 1)), 21.0f, FontStyleBold, kTextStrong,
                  RectF(gridLeft, y + 30, kAxisW - 4, 26), StringAlignmentCenter);
        if (!start.empty()) {
            draw_text(g, fam.get(), wfromu8(start), 11.5f, FontStyleRegular, kAxisTime,
                      RectF(gridLeft, y + 62, kAxisW - 4, 16), StringAlignmentCenter);
        }
        if (!end.empty()) {
            draw_text(g, fam.get(), wfromu8(end), 11.5f, FontStyleRegular, kAxisTime,
                      RectF(gridLeft, y + 80, kAxisW - 4, 16), StringAlignmentCenter);
        }
    }

    // ---- 横向虚线 ----
    {
        Pen dash(kGridDash, 1.0f);
        REAL pattern[] = {7.0f, 7.0f};
        dash.SetDashPattern(pattern, 2);
        for (int r = 1; r < rows; ++r) {
            float y = bodyTop + r * kRowH;
            g.DrawLine(&dash, PointF(axisRight, y), PointF(axisRight + gridW, y));
        }
    }

    // ---- 课程卡 ----
    for (const auto& p : placements) {
        const float gap = 4.0f;
        float dayW = colW - gap * 2;
        int cols = std::max(1, p.col_count);
        float subW = dayW / cols;
        float x = axisRight + p.day * colW + gap + subW * p.col;
        float y = bodyTop + p.row * kRowH + 3.0f;
        RectF rc(x, y, subW - (cols > 1 ? 2.0f : 0.0f), p.span * kRowH - 6.0f);
        draw_course_card(g, fam.get(), *p.course, rc);
    }

    if (data.courses.empty()) {
        draw_text(g, fam.get(), wfromu8("本周没有排课  :)"), 17.0f, FontStyleRegular, kTextMuted,
                  RectF(axisRight, bodyTop + kRowH, gridW, 40), StringAlignmentCenter);
    }

    // ---- 页脚 ----
    draw_text(g, fam.get(), wfromu8(data.footer), 11.0f, FontStyleRegular, kTextMuted,
              RectF(kPad, bodyTop + kRowH * rows + 8, kWidth - kPad * 2, 20),
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
