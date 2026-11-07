#include "mgmp_ui.h"
#include "mgmp_lang.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl3.h"

#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_steamlink.h"
#include "mgmp_session.h"
#include "mgmp_lockstep.h"
#include "mgmp_battleid.h"   // kNoBattle
#include "mgmp_follow.h"
#include "mgmp_cutscene.h"
#include "mgmp_overlay.h"
#include "mgmp_combatlock.h"
#include "mgmp_leave.h"
#include "mgmp_peerlog.h"
#include "mgmp_night.h"
#include "mgmp_meow.h"

// Declared by imgui_impl_win32.h's owner rather than the header itself.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HDC(__stdcall* fn_wglGetCurrentDC)(void);

namespace mgmp {
namespace {

// --- state ------------------------------------------------------------------

struct UI {
    bool     enabled   = false;   // ui = 1 and the subclass installed
    bool     visible   = true;
    bool     ready     = false;   // ImGui context + backends alive
    bool     failed    = false;   // init tried and lost; do not retry every frame
    uint32_t toggle_vk = 0x70;

    HWND     hwnd      = nullptr;
    WNDPROC  prev_wndproc = nullptr;
    uint32_t synthetic_keys = 0;   // posted Escape messages still on their way
    uint32_t synthetic_input = 0;  // posted clicks/Space/Enter still on their way
    uint8_t  up_what   = 0;        // ...and the up still to post
    uint32_t up_frames = 0;
    uint32_t up_pos    = 0;

    // The pane's own copy of the log. Fed from log_ring_fetch by cursor, so a
    // quiet frame copies nothing.
    std::deque<LogEntry> lines;
    uint32_t cursor  = 0;
    uint32_t dropped = 0;      // lines that fell out of the ring unseen

    // Filters.
    bool show_level[5] = { false, true, true, true, true };   // Trace off by default
    ImGuiTextFilter text_filter;
    // Tags seen so far, in first-seen order, and whether each is shown.
    std::vector<std::string> tags;
    std::vector<bool>        tag_on;
    bool autoscroll = true;

    float font_scale = 1.0f;
    ImFont* paper_font = nullptr;   // the night's scrap of paper; see draw_night
    float   paper_w    = 0.0f;      // its width, from the widest line last frame
    bool  forced     = false;   // the class menu is up, panel or not

    // Seeded from mgmp.json at ui_init, so the fields already hold whatever the
    // launch was configured for and a reconnect is one click.
    char addr[64] = "127.0.0.1";
    int  port     = 27600;

    // Reused every frame so the filtered pass allocates nothing after warm-up.
    std::vector<uint32_t> filtered;

    // Centre the node list on the marker: once automatically when the map first
    // appears, and again whenever the button is pressed. A 100-node map is
    // otherwise a hunt for the one row that matters.
    bool     scroll_to_marker = true;
    uint32_t last_node_count  = 0;
};

UI g;

const Config& cfg() { return config(); }

// --- colours ----------------------------------------------------------------
//
// The five the user reads by shape before they read the words: gray is volume,
// white is the normal run, green is something agreeing or completing, yellow is
// odd-but-continuing, red is a halt or a feature switching itself off.

ImVec4 level_colour(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return ImVec4(0.50f, 0.50f, 0.52f, 1.0f);
        case LogLevel::Good:  return ImVec4(0.40f, 0.85f, 0.45f, 1.0f);
        case LogLevel::Warn:  return ImVec4(0.95f, 0.80f, 0.25f, 1.0f);
        case LogLevel::Error: return ImVec4(1.00f, 0.35f, 0.35f, 1.0f);
        case LogLevel::Info:
        default:              return ImVec4(0.88f, 0.88f, 0.90f, 1.0f);
    }
}

const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Good:  return "good";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        default:              return "info";
    }
}

// A stable colour per tag, so LOCKSTEP and SAVEFILE are told apart at a glance
// in an interleaved log. Hashed rather than tabled: there are 24 tags today and
// a table would need editing every time one is added, which is exactly the kind
// of maintenance that silently stops happening.
ImVec4 tag_colour(const char* tag) {
    static const ImVec4 kPalette[] = {
        ImVec4(0.45f, 0.72f, 1.00f, 1.0f),  ImVec4(0.98f, 0.62f, 0.40f, 1.0f),
        ImVec4(0.60f, 0.85f, 0.60f, 1.0f),  ImVec4(0.85f, 0.60f, 0.95f, 1.0f),
        ImVec4(0.40f, 0.85f, 0.85f, 1.0f),  ImVec4(0.95f, 0.80f, 0.50f, 1.0f),
        ImVec4(0.75f, 0.75f, 0.45f, 1.0f),  ImVec4(0.55f, 0.65f, 0.95f, 1.0f),
        ImVec4(0.95f, 0.55f, 0.65f, 1.0f),  ImVec4(0.50f, 0.90f, 0.70f, 1.0f),
    };
    uint32_t h = 2166136261u;                       // FNV-1a
    for (const char* p = tag; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

// --- pulling the ring -------------------------------------------------------

void note_tag(const char* tag) {
    if (!tag[0]) return;
    for (const auto& t : g.tags) if (t == tag) return;
    g.tags.emplace_back(tag);
    g.tag_on.push_back(true);
}

bool tag_shown(const char* tag) {
    if (!tag[0]) return true;   // banner lines have no tag and are always shown
    for (size_t i = 0; i < g.tags.size(); ++i)
        if (g.tags[i] == tag) return g.tag_on[i];
    return true;
}

void pump_log() {
    LogEntry batch[256];
    uint32_t dropped = 0;
    for (;;) {
        uint32_t n = log_ring_fetch(g.cursor, batch, 256, &dropped);
        g.dropped += dropped;
        for (uint32_t i = 0; i < n; ++i) {
            note_tag(batch[i].tag);
            g.lines.push_back(batch[i]);
        }
        if (n < 256) break;   // drained
    }
    while (g.lines.size() > tune::kUiLogLines) g.lines.pop_front();
}

// --- the log window ---------------------------------------------------------

void draw_log() {
    ImGui::SetNextWindowSize(ImVec2(980, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("mgmp -- log")) { ImGui::End(); return; }

    // Severity row. Each checkbox is drawn in its own colour, so the legend and
    // the control are the same widget.
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, level_colour((LogLevel)i));
        ImGui::Checkbox(level_name((LogLevel)i), &g.show_level[i]);
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();
    ImGui::Checkbox("autoscroll", &g.autoscroll);
    ImGui::SameLine();
    if (ImGui::Button("clear view")) g.lines.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("(the file keeps everything)");

    g.text_filter.Draw("filter", 240.0f);

    if (ImGui::TreeNode("tags")) {
        for (size_t i = 0; i < g.tags.size(); ++i) {
            if (i && (i % 6)) ImGui::SameLine();
            bool on = g.tag_on[i];
            ImGui::PushStyleColor(ImGuiCol_Text, tag_colour(g.tags[i].c_str()));
            if (ImGui::Checkbox(g.tags[i].c_str(), &on)) g.tag_on[i] = on;
            ImGui::PopStyleColor();
        }
        ImGui::NewLine();
        if (ImGui::SmallButton("all"))  for (size_t i = 0; i < g.tag_on.size(); ++i) g.tag_on[i] = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("none")) for (size_t i = 0; i < g.tag_on.size(); ++i) g.tag_on[i] = false;
        ImGui::TreePop();
    }

    if (g.dropped) {
        ImGui::PushStyleColor(ImGuiCol_Text, level_colour(LogLevel::Warn));
        ImGui::Text("%u line(s) were written faster than this pane read them and "
                    "fell out of the ring", g.dropped);
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // Rebuilt every frame rather than cached against a filter signature: the
    // predicate is three comparisons over at most ui_log_lines entries, and a
    // cache whose invalidation is wrong shows stale lines during a desync,
    // which is precisely when the pane has to be trusted.
    g.filtered.clear();
    for (uint32_t i = 0; i < (uint32_t)g.lines.size(); ++i) {
        const LogEntry& e = g.lines[i];
        if (!g.show_level[(int)e.level]) continue;
        if (!tag_shown(e.tag)) continue;
        if (g.text_filter.IsActive() &&
            !g.text_filter.PassFilter(e.text) && !g.text_filter.PassFilter(e.tag))
            continue;
        g.filtered.push_back(i);
    }

    ImGui::BeginChild("scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));

    ImGuiListClipper clipper;
    clipper.Begin((int)g.filtered.size());
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const LogEntry& e = g.lines[g.filtered[row]];

            ImGui::TextDisabled("%06u", e.seq);
            ImGui::SameLine();
            ImGui::TextDisabled("%04u", e.turn);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, tag_colour(e.tag));
            ImGui::Text("%-9s", e.tag);
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Text, level_colour(e.level));
            ImGui::TextUnformatted(e.text);
            ImGui::PopStyleColor();
        }
    }
    clipper.End();

    ImGui::PopStyleVar();
    if (g.autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

// --- the session window -----------------------------------------------------

const char* state_name(NetState s) {
    switch (s) {
        case NetState::Idle:       return "idle";
        case NetState::Listening:  return "listening";
        case NetState::Connecting: return "connecting";
        case NetState::Connected:  return "connected";
        case NetState::Ready:      return "READY";
        case NetState::Failed:     return "FAILED";
        case NetState::Closed:     return "closed";
    }
    return "?";
}

ImVec4 state_colour(NetState s) {
    switch (s) {
        case NetState::Ready:                             return level_colour(LogLevel::Good);
        case NetState::Failed:  case NetState::Closed:    return level_colour(LogLevel::Error);
        case NetState::Idle:                              return level_colour(LogLevel::Trace);
        default:                                          return level_colour(LogLevel::Warn);
    }
}

// THE NIGHT ON A SCRAP OF PAPER, as the game shows it (the user's screenshot,
// 2026-11-05): a light grey sheet with torn edges and a dark outline, the text
// in capitals, centred, in a hand-drawn face. The game's own font is a vector
// font inside its swfs, so Segoe Print Bold stands in -- every Windows 10 has
// it, Cyrillic included.

// Upper case, the game's way of writing these.
std::string upper_utf8(const char* s) {
    const int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wn <= 1) return s;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], wn);
    CharUpperBuffW(&w[0], (DWORD)(wn - 1));
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], n, nullptr, nullptr);
    out.resize(strlen(out.c_str()));
    return out;
}

// One paragraph, broken into lines no wider than `width`, each centred.
float g_widest = 0.0f;   // the widest line draw_night laid out this frame

void centred_lines(const std::string& text, float width, float left, float right) {
    std::string line;
    size_t at = 0;
    auto emit = [&](const std::string& l) {
        const ImVec2 sz = ImGui::CalcTextSize(l.c_str());
        if (sz.x > g_widest) g_widest = sz.x;
        ImGui::SetCursorPosX(left + (right - left - sz.x) * 0.5f);
        const ImVec2 at0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
        // Bolder than Segoe Print Bold, as the game's lettering is.
        static const float kOff[][2] = { {0, 0}, {1.1f, 0}, {0, 1.0f}, {1.1f, 1.0f}, {0.55f, 0.5f} };
        for (const auto& o : kOff) dl->AddText(ImVec2(at0.x + o[0], at0.y + o[1]), ink, l.c_str());
        ImGui::Dummy(ImVec2(sz.x + 1.1f, sz.y + 1.0f));
    };
    while (at <= text.size()) {
        size_t sp = text.find(' ', at);
        if (sp == std::string::npos) sp = text.size();
        const std::string word = text.substr(at, sp - at);
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && ImGui::CalcTextSize(next.c_str()).x > width) {
            emit(line);
            line = word;
        } else {
            line = next;
        }
        at = sp + 1;
    }
    if (!line.empty()) emit(line);
}

// A deterministic wobble, so the tear holds still from frame to frame.
float tear(uint32_t i, uint32_t salt) {
    uint32_t h = i * 2654435761u ^ salt * 2246822519u;
    h ^= h >> 15; h *= 2246822507u; h ^= h >> 13;
    return (float)(h & 0xFFFF) / 65535.0f;
}

void paper(ImDrawList* dl, ImVec2 a, ImVec2 b, bool inked = false) {
    std::vector<ImVec2> pts;
    const float step = 26.0f;
    // top, left to right
    for (float x = a.x; x < b.x; x += step)
        pts.push_back(ImVec2(x, a.y + (tear((uint32_t)(x - a.x), 1) - 0.5f) * 7.0f));
    // right side, a couple of nicks
    for (float y = a.y; y < b.y; y += step * 1.6f)
        pts.push_back(ImVec2(b.x + (tear((uint32_t)(y - a.y), 2) - 0.5f) * 6.0f, y));
    // bottom, right to left, with a few long tears hanging off it, here and there
    uint32_t k = 0;
    for (float x = b.x; x > a.x; x -= step, ++k) {
        float y = b.y + (tear(k, 3) - 0.5f) * 8.0f;
        if (tear(k, 6) > 0.84f) y += 14.0f + tear(k, 4) * 24.0f;
        pts.push_back(ImVec2(x, y));
    }
    // left side
    for (float y = b.y; y > a.y; y -= step * 1.6f)
        pts.push_back(ImVec2(a.x + (tear((uint32_t)(y - a.y), 5) - 0.5f) * 6.0f, y));
    const ImU32 shade   = IM_COL32(0, 0, 0, 90);
    const ImU32 fill    = IM_COL32(222, 222, 218, 250);
    const ImU32 outline = IM_COL32(40, 38, 36, 255);
    std::vector<ImVec2> sh(pts);
    if (inked) {
        // The game's own outline: the shape in black a few pixels all round,
        // and once more a little down and right.
        constexpr float kInk = 4.0f;
        const ImU32 ink = IM_COL32(18, 16, 16, 255);
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < 12; ++i) {
                const float t = i * 6.2831853f / 12;
                const float dx = cosf(t) * kInk + (pass ? 3.0f : 0.0f);
                const float dy = sinf(t) * kInk + (pass ? 4.0f : 0.0f);
                for (size_t j = 0; j < pts.size(); ++j) sh[j] = ImVec2(pts[j].x + dx, pts[j].y + dy);
                dl->AddConcavePolyFilled(sh.data(), (int)sh.size(), ink);
            }
    } else {
        for (ImVec2& p : sh) { p.x += 5; p.y += 6; }
        dl->AddConcavePolyFilled(sh.data(), (int)sh.size(), shade);
    }
    dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), fill);
    dl->AddPolyline(pts.data(), (int)pts.size(), outline, ImDrawFlags_Closed, 3.0f);
}

void draw_night(const char* const* lines, uint32_t n, bool over_loading) {
    const ImGuiIO& io = ImGui::GetIO();
    // The loading screen takes the focus every frame; this goes on top of it.
    if (over_loading) ImGui::SetNextWindowFocus();
    // As wide as its widest line and no wider (2026-11-05: a big sheet with a
    // little text on it), up to most of the screen; long lines wrap there.
    constexpr float kPadX = 56.0f;
    const float most  = io.DisplaySize.x * 0.82f;
    const float width = g.paper_w > 0.0f ? (g.paper_w < most ? g.paper_w : most) : most;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.04f + 12.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, 34));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.09f, 0.08f, 1.0f));
    bool open = true;
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##mgmp_night", nullptr, f)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        if (g.paper_font) ImGui::PushFont(g.paper_font, 32.0f);
        const float left  = ImGui::GetCursorPosX();
        const float right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        const float wrap  = most - 2.0f * kPadX;   // break at the widest the sheet may be
        g_widest = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            if (i) ImGui::Dummy(ImVec2(0, 14));
            const std::string up = upper_utf8(lines[i]);
            size_t at = 0;
            while (at <= up.size()) {
                size_t br = up.find('\n', at);
                if (br == std::string::npos) br = up.size();
                centred_lines(up.substr(at, br - at), wrap, left, right);
                at = br + 1;
            }
        }
        ImGui::Dummy(ImVec2(0, 10));
        const char* ok = tr(u8"\u041e\u041a", "OK");
        const ImVec2 okw = ImGui::CalcTextSize(ok);
        ImGui::SetCursorPosX(left + (right - left - okw.x - 24) * 0.5f);
        g.paper_w = g_widest + 2.0f * kPadX + 4.0f;
        if (g.paper_w < 360.0f) g.paper_w = 360.0f;
        if (ImGui::InvisibleButton("ok", ImVec2(okw.x + 24, okw.y + 6))) open = false;
        const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        const bool hot = ImGui::IsItemHovered();
        for (int o = 0; o < 4; ++o)
            dl->AddText(ImVec2(r0.x + 12 + (o & 1) * 1.1f, r0.y + 3 + (o >> 1) * 1.0f),
                        hot ? IM_COL32(150, 40, 30, 255) : IM_COL32(26, 23, 20, 255), ok);
        if (g.paper_font) ImGui::PopFont();
        dl->ChannelsSetCurrent(0);
        // The tears and the shadow go past the window's edges; the window
        // clips to itself, and on 2026-11-05 that cut the paper off square.
        dl->PushClipRectFullScreen();
        const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        paper(dl, ImVec2(p.x + 10, p.y + 8), ImVec2(p.x + s.x - 10, p.y + s.y - 14), true);
        dl->PopClipRect();
        dl->ChannelsMerge();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!open) night_dismiss();
}

// A one-line scrap at the top, no buttons, not taking the mouse.
// `beside_round`: in a fight, top right under the game's turn order (the
// user's pick, 2026-11-05), not over the round counter; otherwise centred.
void draw_paper_note(const char* text, bool beside_round = false) {
    const ImGuiIO& io = ImGui::GetIO();
    if (beside_round)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 24.0f, io.DisplaySize.y * 0.15f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    else
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.04f + 12.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40, 26));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.09f, 0.08f, 1.0f));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##mgmp_endday", nullptr, f)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        if (g.paper_font) ImGui::PushFont(g.paper_font, 32.0f);
        const std::string t = upper_utf8(text);
        const ImVec2 sz = ImGui::CalcTextSize(t.c_str());
        const ImVec2 at0 = ImGui::GetCursorScreenPos();
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
        for (int o = 0; o < 4; ++o)
            dl->AddText(ImVec2(at0.x + (o & 1) * 1.1f, at0.y + (o >> 1) * 1.0f), ink, t.c_str());
        ImGui::Dummy(ImVec2(sz.x + 1.1f, sz.y + 1.0f));
        if (g.paper_font) ImGui::PopFont();
        dl->ChannelsSetCurrent(0);
        dl->PushClipRectFullScreen();
        const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        paper(dl, ImVec2(p.x + 10, p.y + 8), ImVec2(p.x + s.x - 10, p.y + s.y - 12), true);
        dl->PopClipRect();
        dl->ChannelsMerge();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void draw_loading(const char* step) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 1.0f));
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                               ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin("##mgmp_loading", nullptr, f)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f);
        const float  r = 34.0f * g.font_scale;
        const double t = ImGui::GetTime();
        constexpr int kDots = 12;
        for (int i = 0; i < kDots; ++i) {
            const float a   = (float)(i * 2 * 3.14159265 / kDots - t * 4.0);
            const float age = (float)fmod((double)i / kDots + t * 0.65, 1.0);
            const ImVec4 col(0.95f, 0.62f, 0.25f, 0.15f + 0.85f * age);
            dl->AddCircleFilled(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r),
                                (2.5f + 3.5f * age) * g.font_scale, ImGui::GetColorU32(col));
        }
        auto centred = [&](float y, const ImVec4& col, const char* text) {
            const ImVec2 sz = ImGui::CalcTextSize(text);
            dl->AddText(ImVec2(c.x - sz.x * 0.5f, y), ImGui::GetColorU32(col), text);
        };
        centred(c.y + r + 28, ImVec4(0.96f, 0.90f, 0.80f, 1.0f), step ? step : "");
        centred(c.y + r + 54, ImVec4(0.55f, 0.50f, 0.45f, 1.0f), tr(u8"\u041c\u043e\u0434 \u0432\u043e\u0437\u0432\u0440\u0430\u0449\u0430\u0435\u0442 \u0442\u0435\u0431\u044f \u0432 \u0438\u0433\u0440\u0443 \u0445\u043e\u0441\u0442\u0430, \u043d\u0438\u0447\u0435\u0433\u043e \u043d\u0430\u0436\u0438\u043c\u0430\u0442\u044c \u043d\u0435 \u043d\u0443\u0436\u043d\u043e",
                    "The mod is taking you back into the host's game -- no need to press anything"));

        // The mod's last lines, small and dim along the bottom.
        const float line = ImGui::GetTextLineHeight();
        const size_t n   = g.lines.size() < 6 ? g.lines.size() : 6;
        float y = io.DisplaySize.y - 14 - line * (float)n;
        for (size_t i = g.lines.size() - n; i < g.lines.size(); ++i, y += line) {
            char text[300];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%-9s %s", g.lines[i].tag, g.lines[i].text);
            dl->AddText(ImVec2(18, y), ImGui::GetColorU32(ImVec4(0.42f, 0.40f, 0.38f, 1.0f)), text);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void row(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

// The map, and a button to enter any node on it.
//
// This EDITS THE RUN, which nothing else on this panel does, and it is here
// under protest: the reason it exists is that a run can end up standing at a
// node it has already entered but not resolved, unable to go in and unable to
// move on. Re-entering that node is the way out.
//
// Host only -- entering a node is a run decision. It goes through the ordinary
// EnterNode hook, so it publishes ENTERNODE and the client follows exactly as
// if the node had been clicked; and it is applied at the tail of
// MapScreen::update, the site the game's own UI callback uses.
void draw_nodes() {
    if (net_role() == NetRole::Client) return;   // the host owns the run

    const uint32_t count = follow_node_count();
    if (!ImGui::CollapsingHeader("map nodes")) return;

    if (!count) {
        ImGui::TextDisabled("no map screen yet -- the nodes appear once the run "
                            "is on the map");
        return;
    }

    // Three different answers to "which node", and they disagree in exactly the
    // situation the panel exists for.
    //
    //   marker    where the cats are STANDING -- read live from the game, so it
    //             is right for a run that was just loaded from disk
    //   selected  what is highlighted on the map
    //   entered   what this module watched somebody enter THIS SESSION, which
    //             is nothing at all after a reload
    uint32_t marker = 0, selected = 0, entered = 0;
    const bool have_marker   = follow_marker_node(marker);
    const bool have_selected = follow_selected_node(selected);
    const bool have_entered  = follow_current_node(entered);

    // A new map (different node count) re-arms the auto-centre.
    if (count != g.last_node_count) {
        g.last_node_count  = count;
        g.scroll_to_marker = true;
    }

    if (have_marker) {
        ImGui::TextColored(level_colour(LogLevel::Good),
                           "cats are standing at node %u of %u", marker, count);
        ImGui::SameLine();
        if (ImGui::SmallButton("scroll to")) g.scroll_to_marker = true;
    }
    if (!have_marker)
        ImGui::TextDisabled("marker node unknown (map not up, or the offset drifted)");

    if (have_selected && (!have_marker || selected != marker))
        ImGui::Text("selected: node %u", selected);
    if (have_entered && (!have_marker || entered != marker))
        ImGui::TextDisabled("last entered this session: node %u", entered);

    ImGui::TextColored(level_colour(LogLevel::Warn),
                       "entering a node from here is not a move the player made");

    if (!follow_on_map()) {
        ImGui::TextColored(level_colour(LogLevel::Trace),
                           "the map is not ticking -- a node is in progress; "
                           "finish it before jumping");
    }

    ImGui::BeginChild("nodes", ImVec2(0, 200), ImGuiChildFlags_Borders);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t    type = 0;
        uint64_t    seed = 0;
        const char* name = "?";
        if (!follow_node_info(i, type, seed, &name)) continue;

        ImGui::PushID((int)i);
        if (ImGui::SmallButton("enter")) {
            follow_request_jump(i);
            log_line_lvl(LogLevel::Warn, "UI",
                         "panel requested a jump to node %u (%s)", i, name);
        }
        ImGui::SameLine();

        // The marker wins the colour: it is the one that answers "where am I".
        const bool is_marker   = have_marker   && i == marker;
        const bool is_selected = have_selected && i == selected;
        const bool is_entered  = have_entered  && i == entered;

        char mark[40] = {};
        if (is_marker)   strcat_s(mark, "  <== CATS ARE HERE");
        if (is_selected) strcat_s(mark, is_marker ? " (selected)" : "  <- selected");
        if (is_entered && !is_marker) strcat_s(mark, "  (entered)");

        const ImVec4 col = is_marker   ? level_colour(LogLevel::Good)
                         : is_selected ? level_colour(LogLevel::Warn)
                                       : level_colour(LogLevel::Info);
        ImGui::TextColored(col, "%3u  %-16s %016llx%s", i, name,
                           (unsigned long long)seed, mark);
        if (is_marker && g.scroll_to_marker) {
            ImGui::SetScrollHereY(0.5f);
            g.scroll_to_marker = false;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// The connect controls.
//
// Every button here only ever RECORDS a request; session_update applies it at
// the top of the next frame. Calling net_shutdown from inside the present path
// would tear a session down underneath a half-submitted frame.
void draw_connect() {
    ImGui::SeparatorText("connect");

    const bool busy = session_request_pending();
    const bool live = net_active();

    bool steam = config_steam();
    ImGui::BeginDisabled(busy || live);
    if (ImGui::Checkbox("through Steam", &steam)) config_set_steam(steam);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Connect through Steam's relays. Nobody forwards a port;"
                          " both players must be Steam friends.");

    if (steam) {
        const uint64_t self = steamlink_self_id();
        if (self) {
            char id[32];
            _snprintf_s(id, sizeof(id), _TRUNCATE, "%llu", (unsigned long long)self);
            ImGui::Text("your ID: %s", id);
            ImGui::SameLine();
            if (ImGui::SmallButton("copy")) ImGui::SetClipboardText(id);
            ImGui::SetItemTooltip("The host sends this to the friend who is joining.");
        } else {
            ImGui::TextDisabled("Steam is not reachable");
        }
    }

    ImGui::SetNextItemWidth(steam ? 240.0f : 150.0f);
    ImGui::InputText(steam ? "host's ID" : "addr", g.addr, sizeof(g.addr));
    if (!steam) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("port", &g.port, 0, 0);
    }
    if (g.port < 1)     g.port = 1;
    if (g.port > 65535) g.port = 65535;

    ImGui::BeginDisabled(busy);

    if (ImGui::Button("host")) session_request_host((uint16_t)g.port);
    ImGui::SetItemTooltip(steam ? "Wait for a Steam friend to join with your ID. If a"
                                  " session is already up it is torn down first."
                                : "Listen on this port. If a session is already up it is"
                                  " torn down first -- 'host' means start over.");
    ImGui::SameLine();

    if (ImGui::Button("join")) session_request_join(g.addr, (uint16_t)g.port);
    ImGui::SetItemTooltip(steam ? "Connect to the SteamID above. A client that only lost"
                                  " its connection keeps its run."
                                : "Dial addr:port. A client that only lost its socket"
                                  " keeps its run and declines the host's save.");
    ImGui::SameLine();

    ImGui::BeginDisabled(!live);
    if (ImGui::Button("disconnect")) session_request_disconnect();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Drop the session. Pair it with 'join' to exercise"
                          " reconnect without restarting the process.");

    // The friend's log, by hand: it also comes on its own after a halt or a
    // node hash mismatch. See mgmp_peerlog.h.
    ImGui::BeginDisabled(!live);
    const bool host_side = net_role() == NetRole::Host;
    if (ImGui::Button(host_side ? tr(u8"\u041b\u043e\u0433 \u0434\u0440\u0443\u0433\u0430", "Friend's log")
                                : tr(u8"\u041b\u043e\u0433 \u0445\u043e\u0441\u0442\u0443", "Log to host")))
        peerlog_ask("asked from the panel");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(host_side
        ? tr(u8"\u0417\u0430\u0431\u0440\u0430\u0442\u044c \u043b\u043e\u0433 \u0434\u0440\u0443\u0433\u0430 \u0432 logs\\friend",
             "Fetch the friend's log into logs\\friend")
        : tr(u8"\u041e\u0442\u043f\u0440\u0430\u0432\u0438\u0442\u044c \u0441\u0432\u043e\u0439 \u043b\u043e\u0433 \u0445\u043e\u0441\u0442\u0443",
             "Send your log to the host"));
    if (host_side && peerlog_last_saved()[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", peerlog_last_saved());
    }

    ImGui::EndDisabled();

    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("applying...");
    }

    // ARM THE LEAVE PATH BY HAND. This exists because the feature has three
    // stages that fail identically from the outside -- the host never announced,
    // the client declined, or the client armed and never saw the button -- and
    // pressing this skips the first two. If the pause menu is open and this does
    // nothing, the fault is the click; if it works, the fault is upstream.
    if (ImGui::Button("arm leave")) leave_request_local();
    ImGui::SetItemTooltip("Pretend the host announced that it left the run:"
                          " open the pause menu and the mod presses Quit To Menu."
                          " A test of the click half on its own.");
    ImGui::SameLine();
    // Live, beside the button, because every stage of this fails invisibly and
    // reading it out of the log afterwards has already cost two rounds of "it
    // did not work" with no way to say which half.
    char leave[192];
    leave_status(leave, sizeof(leave));
    ImGui::TextDisabled("%s", leave);

    if (session_last_action()[0]) {
        const char* a = session_last_action();
        ImGui::TextColored(a[0] == '!' ? level_colour(LogLevel::Error)
                                       : level_colour(LogLevel::Good),
                           "%s", a);
    }

    // Reconnect is the one open item with no live evidence behind it, and the
    // order below is the part that is easy to get wrong: a client that only
    // lost its socket DECLINES the host's save and keeps its run, so testing
    // the save transfer needs a peer whose process actually restarted.
    if (ImGui::TreeNode("what to expect")) {
        ImGui::TextWrapped(
            "Reconnect (socket only): the client keeps its run and declines the "
            "save. The host flushes its live run to disk, then sends CATDATA, "
            "INVENTORY, an ENTERNODE for the node the run is standing in, and "
            "every ACTION of the battle in progress. A joiner visibly replays "
            "the fight from turn 0.\n\n"
            "If turn 0 disagrees, check first whether CatData is written during "
            "a battle -- the replay rebuilds the fight from the cats as pushed, "
            "and nobody has confirmed the game leaves CatData alone mid-fight.");
        ImGui::TreePop();
    }
}

void draw_session() {
    ImGui::SetNextWindowSize(ImVec2(420, 260), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("mgmp -- session")) { ImGui::End(); return; }

    const NetState st = net_state();
    ImGui::TextDisabled("state");
    ImGui::SameLine();
    ImGui::TextColored(state_colour(st), "%s", state_name(st));
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", session_status());

    if (st == NetState::Failed && net_error()[0])
        ImGui::TextColored(level_colour(LogLevel::Error), "%s", net_error());

    if (lockstep_halted())
        ImGui::TextColored(level_colour(LogLevel::Error),
                           "HALTED -- the peers disagreed and this one stopped");

    ImGui::Separator();

    const NetStats s = net_stats();
    if (ImGui::BeginTable("session", 2, ImGuiTableFlags_SizingStretchProp)) {
        const NetRole r = net_role();
        row("role", "%s", r == NetRole::Host   ? "host"
                        : r == NetRole::Client ? "client" : "off");

        // "id 1" and NOT "1 of 2". The id is an identity, not an ordinal, and
        // the ordinal reading is wrong in a way that looks like a bug: peer 0
        // is ALWAYS the host, so a client correctly showing id 1 reads as
        // "1 of 2" -- which sounds like it is the first of two rather than the
        // second peer. The count belongs on its own row.
        row("this peer", "id %u%s", (unsigned)net_self(),
            net_self() == kHostPeer ? "  (peer 0 is always the host)" : "");

        uint8_t ids[8] = {};
        if (net_peer_ids(ids, 8) && net_peer_count()) {
            char buf[64]; int n = 0;
            for (uint8_t i = 0; i < net_peer_count() && i < 8; ++i)
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                                 i ? ", %u" : "%u", (unsigned)ids[i]);
            row("session", "%u peer(s): [%s]", (unsigned)net_peer_count(), buf);
        } else {
            row("session", "%u peer(s)", (unsigned)net_peer_count());
        }

        // Not the id. Once anyone has disconnected the two diverge, and it is
        // this -- the position in the sorted membership list -- that the
        // control split is computed from, so the two are worth showing apart.
        row("split index", "%u", (unsigned)net_peer_pos());

        row("messages", "%u sent / %u received / %u dropped",
            s.sent, s.received, s.dropped);
        row("bytes", "%llu out / %llu in",
            (unsigned long long)s.bytes_sent, (unsigned long long)s.bytes_received);

        ImGui::EndTable();
    }

    ImGui::SeparatorText("battle");

    if (ImGui::BeginTable("battle", 2, ImGuiTableFlags_SizingStretchProp)) {
        const bool active = lockstep_active();
        row("lockstep", "%s", active ? "active" : "idle");
        // The node seed, which is what battle identity IS -- both peers read it
        // out of MapNode+0x118 entering the same node. Two peers showing
        // different ids here are in different battles, and that alone explains
        // most "the battle just sits there" reports.
        const uint64_t bid = lockstep_battle_id();
        if (active && bid != kNoBattle) row("battle id", "%016llx", (unsigned long long)bid);
        else                            row("battle id", "--");
        row("turn", "%u", log_turn());
        row("local actor", "%s", lockstep_local_actor() ? "yes -- this peer decides"
                                                        : "no");
        // Reads the LAST combat-menu tick, not the current frame -- the scope
        // the Button hook runs in is closed by the time the panel draws. Out of
        // a battle it holds whatever the last bar said.
        row("ability bar", "%s", combatlock_engaged()
                                     ? "greyed (a peer owns this cat)" : "live");
        row("map input", "%s", follow_suppresses_local_input()
                                   ? "suppressed (following the host)" : "local");
        ImGui::EndTable();
    }

    draw_connect();
    draw_nodes();

    ImGui::SeparatorText(tr(u8"\u0437\u0432\u0443\u043a", "sound"));
    {
        // The map's meows, the other player's clicks and this one's.
        int pct = (int)(meow_volume() * 100.0f + 0.5f);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderInt(tr(u8"\u041c\u044f\u0443\u043a\u0430\u043d\u044c\u0435 \u043d\u0430 \u043a\u0430\u0440\u0442\u0435", "Map meows"),
                             &pct, 0, 100, "%d%%"))
            meow_set_volume(pct / 100.0f);
        ImGui::SetItemTooltip(lang_ru() ? u8"\u0413\u0440\u043e\u043c\u043a\u043e\u0441\u0442\u044c \u043c\u044f\u0443\u043a\u0430\u043d\u044c\u044f, \u043a\u043e\u0433\u0434\u0430 \u043a\u0442\u043e-\u0442\u043e \u0432\u044b\u0431\u0438\u0440\u0430\u0435\u0442 \u0443\u0437\u0435\u043b \u043a\u0430\u0440\u0442\u044b. 0%% -- \u0431\u0435\u0437 \u0437\u0432\u0443\u043a\u0430."
                                         : "How loud the meow is when someone picks a map node. 0%% -- silent.");
    }

    ImGui::SeparatorText("panel");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("scale", &g.font_scale, 0.75f, 2.5f, "%.2f"))
        ImGui::GetStyle().FontScaleMain = g.font_scale;
    ImGui::TextDisabled("VK 0x%02X toggles this panel", (unsigned)g.toggle_vk);

    ImGui::End();
}

// --- input ------------------------------------------------------------------

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Keys the mod posted for the game itself. A friend who has just used the
    // panel to rejoin still has it open, and it must not eat the Escape that
    // takes him out to load the host's run.
    if (g.synthetic_keys && (msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_ESCAPE) {
        --g.synthetic_keys;
        return CallWindowProcW(g.prev_wndproc, hwnd, msg, wp, lp);
    }
    // The host's cutscene inputs, posted by ui_post_input and counted like the
    // Escapes. A real click landing in between passes in a posted one's place
    // and that one is swallowed after it: the count of clicks is the same.
    if (g.synthetic_input &&
        (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
         ((msg == WM_KEYDOWN || msg == WM_KEYUP) && (wp == VK_SPACE || wp == VK_RETURN)))) {
        --g.synthetic_input;
        return CallWindowProcW(g.prev_wndproc, hwnd, msg, wp, lp);
    }

    // The toggle is read before ImGui sees anything, so the panel can always be
    // dismissed even if it has somehow captured the keyboard.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == (WPARAM)g.toggle_vk) {
        g.visible = !g.visible;
        if (g.ready) {
            // Drop any half-finished input state, or a button held when the
            // panel was dismissed stays held when it comes back.
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        return 0;
    }

    if (g.ready && (g.visible || g.forced)) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
            return 1;

        // Swallow what the panel is using, so a click on a button does not also
        // land on the board behind it. Only when ImGui actually wants it: the
        // game must keep receiving everything else, and swallowing a button-up
        // it saw the down for would leave it stuck.
        //
        // Safe for lockstep by construction -- suppressing local input can only
        // ever mean "this player did not act", which the protocol already
        // handles everywhere (Brain::GetChoice returns type=1 and the game
        // waits exactly as it waits for a person).
        const ImGuiIO& io = ImGui::GetIO();
        switch (msg) {
            case WM_MOUSEMOVE:   case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:   case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:   case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
            case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
                if (io.WantCaptureMouse) return 1;
                break;
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                if (io.WantCaptureKeyboard) return 1;
                break;
            default: break;
        }
    }

    // A cutscene moves on the host's clicks: the client's own are not the game's.
    if (cutscene_filter_input(msg, (uintptr_t)wp, (intptr_t)lp)) return 0;

    return CallWindowProcW(g.prev_wndproc, hwnd, msg, wp, lp);
}

// --- bring-up ---------------------------------------------------------------

HWND game_window() {
    HMODULE ogl = GetModuleHandleA("opengl32.dll");
    if (!ogl) return nullptr;
    auto get_dc = (fn_wglGetCurrentDC)GetProcAddress(ogl, "wglGetCurrentDC");
    if (!get_dc) return nullptr;
    HDC dc = get_dc();
    return dc ? WindowFromDC(dc) : nullptr;
}

// Runs on the first swap, because that is the first moment a GL context is
// current -- the font atlas is a texture and cannot be built before one exists.
bool bring_up() {
    g.hwnd = game_window();
    if (!g.hwnd) {
        log_line("UI", "!! no window behind the current GL DC -- the panel is OFF");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // No imgui.ini. The working directory is the game's, and the loopback test
    // runs two instances out of it -- they would fight over one settings file
    // and each would restore the other's window layout.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // The game hides the OS cursor and draws its own from textures/cursor/*.png
    // (see glaiel::SetCursor), so an OS-cursor-driven pointer would be
    // invisible over the panel. ImGui draws its own instead.
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().FontScaleMain = g.font_scale;
    // The default font has no Cyrillic, and cat names and the class menu are
    // Russian. 1.92 loads glyphs as they are drawn, so no range is needed.
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f))
        io.Fonts->AddFontDefault();
    // The sex signs come from Segoe UI Symbol, merged into both faces.
    ImFontConfig merge;
    merge.MergeMode = true;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 18.0f, &merge);
    g.paper_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeprb.ttf", 27.0f);
    if (!g.paper_font) g.paper_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\comicbd.ttf", 27.0f);
    if (g.paper_font) io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 27.0f, &merge);

    if (!ImGui_ImplWin32_Init(g.hwnd)) {
        log_line("UI", "!! ImGui_ImplWin32_Init failed -- the panel is OFF");
        ImGui::DestroyContext();
        return false;
    }
    // GLSL 150 -- ApplicationBase::RefreshWindow asks for a CORE 3.2 context,
    // and core profile has no compatibility fallback to fall back to.
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        log_line("UI", "!! ImGui_ImplOpenGL3_Init failed -- the panel is OFF");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    log_line("UI", "panel up -- imgui %s, win32 + gl3 core, hwnd %p, VK 0x%02X toggles",
             IMGUI_VERSION, (void*)g.hwnd, (unsigned)g.toggle_vk);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

void ui_init() {
    g.enabled   = cfg().ui;
    g.visible   = cfg().ui_visible;
    g.toggle_vk = cfg().ui_key;
    _snprintf_s(g.addr, sizeof(g.addr), _TRUNCATE, "%s", cfg().net_addr);
    g.port = (int)cfg().net_port;

    if (!g.enabled) {
        log_line("UI", "panel OFF (set ui.enabled in mgmp.json)");
        return;
    }

    // The subclass goes on now rather than at bring-up, because the toggle key
    // has to work even if the panel starts hidden -- and a hidden panel never
    // reaches the swap path that would install it.
    HWND hwnd = game_window();
    if (!hwnd) {
        // Not fatal: ui_on_swap retries, and on the very first frames there may
        // be no current DC on this thread yet.
        log_line("UI", "window not resolvable yet -- input hookup deferred to the first swap");
        return;
    }
    g.hwnd = hwnd;
    g.prev_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&wndproc);
    if (!g.prev_wndproc)
        log_line("UI", "!! could not subclass the game window -- the panel will draw"
                       " but will not take input");
}

void ui_shutdown() {
    // The WndProc is deliberately NOT restored, for the same reason
    // overlay_shutdown leaves the SDL slot alone: a session can end and begin
    // again inside one process, and a panel that silently loses its input the
    // second time is the asymmetry that is easy to write and hard to notice.
    // The mod is injected and never unloaded.
    //
    // The ImGui context is left standing too -- destroying it frees GL objects,
    // and shutdown can run from a thread with no current context.
    g.visible = false;
}

void* ui_window() { return g.hwnd; }

void ui_post_escape() {
    if (!g.hwnd) return;
    const LPARAM down = 1 | ((LPARAM)MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC) << 16);
    g.synthetic_keys += 2;
    PostMessageW(g.hwnd, WM_KEYDOWN, VK_ESCAPE, down);
    PostMessageW(g.hwnd, WM_KEYUP,   VK_ESCAPE, down | 0xC0000000);
}

void post_input(uint8_t what, bool down, uint32_t pos) {
    if (what == 0) {
        POINT pt{};
        RECT  rc{};
        if (pos && GetClientRect(g.hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
            // The host's spot. A move first: the game takes a click's place from
            // where the mouse last moved, not from the click's own lParam.
            pt.x = (LONG)(((pos >> 16) & 0xFFFF) * (uint64_t)rc.right / 65535);
            pt.y = (LONG)((pos & 0xFFFF) * (uint64_t)rc.bottom / 65535);
            PostMessageW(g.hwnd, WM_MOUSEMOVE, down ? 0 : MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        } else {
            GetCursorPos(&pt);
            ScreenToClient(g.hwnd, &pt);
        }
        PostMessageW(g.hwnd, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0,
                     MAKELPARAM(pt.x, pt.y));
    } else {
        const UINT   vk   = what == 1 ? VK_SPACE : VK_RETURN;
        const LPARAM base = 1 | ((LPARAM)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
        PostMessageW(g.hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk,
                     down ? base : (base | 0xC0000000));
    }
}

bool ui_post_input(uint8_t what, uint32_t pos) {
    if (!g.hwnd || g.up_frames) return false;
    ++g.synthetic_input;
    post_input(what, true, pos);
    g.up_what   = what;
    g.up_pos    = pos;
    g.up_frames = 3;
    return true;
}

uint32_t ui_pack_pos(intptr_t lparam) {
    RECT rc{};
    if (!g.hwnd || !GetClientRect(g.hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return 0;
    int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
    x = x < 0 ? 0 : x > rc.right ? rc.right : x;
    y = y < 0 ? 0 : y > rc.bottom ? rc.bottom : y;
    const uint32_t px = (uint32_t)((uint64_t)x * 65535 / rc.right);
    const uint32_t py = (uint32_t)((uint64_t)y * 65535 / rc.bottom);
    return ((px << 16) | py) | (px == 0 && py == 0 ? 1u : 0u);   // never 0: 0 is "no place"
}

bool ui_captures_input() {
    if (!g.ready || !(g.visible || g.forced)) return false;
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void ui_on_swap(void* window) {
    (void)window;
    // Before the panel's own early returns: the up has to follow its down
    // whether or not the panel is enabled or drawn.
    if (g.up_frames && --g.up_frames == 0) {
        ++g.synthetic_input;
        post_input(g.up_what, false, g.up_pos);
    }
    if (!g.enabled || g.failed) return;

    // Deferred from ui_init when there was no current DC then.
    if (!g.prev_wndproc) {
        HWND hwnd = game_window();
        if (hwnd) {
            g.hwnd = hwnd;
            g.prev_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                        (LONG_PTR)&wndproc);
            if (g.prev_wndproc)
                log_line("UI", "input hooked up on the first swap (hwnd %p)", (void*)hwnd);
        }
    }

    const char* step = nullptr;
    const bool loading = leave_loading(&step);
    // The night's window comes up with F1 closed too, like the class chooser.
    uint32_t nights = 0;
    const char* const* night = night_view(&nights);
    const bool refused = night_end_day_refused();
    // In a fight: the other player not in it yet (every decision held), or the
    // cat whose turn it is theirs (its bar greyed) -- a note at the top.
    const char* note = nullptr;
    if (refused)
        note = tr(u8"\u0414\u0435\u043d\u044c \u0437\u0430\u043a\u0430\u043d\u0447\u0438\u0432\u0430\u0435\u0442 \u0445\u043e\u0441\u0442",
                  "The host ends the day");
    else if (lockstep_in_battle() && lockstep_barrier_holding())
        note = tr(u8"\u0416\u0434\u0451\u043c \u0434\u0440\u0443\u0433\u043e\u0433\u043e \u0438\u0433\u0440\u043e\u043a\u0430 \u2014 \u043e\u043d \u0435\u0449\u0451 \u043d\u0435 \u0432 \u0431\u043e\u044e",
                  u8"Waiting for another player \u2014 not in the fight yet");
    else if (lockstep_in_battle() && combatlock_menu_live() && combatlock_engaged())
        note = tr(u8"\u0425\u043e\u0434\u0438\u0442 \u0434\u0440\u0443\u0433\u043e\u0439 \u0438\u0433\u0440\u043e\u043a", "Another player's turn");
    g.forced = loading || night || note;
    if (!g.visible && !g.forced) return;

    if (!g.ready) {
        if (!bring_up()) { g.failed = true; return; }
        g.ready = true;
    }

    // Cheap, and it keeps the pane current even on frames where nothing is
    // drawn because a window is collapsed.
    pump_log();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g.visible) {
        draw_session();
        draw_log();
    }
    if (loading) draw_loading(step);
    if (night) draw_night(night, nights, loading);
    else if (note && !loading) draw_paper_note(note, !refused);

    ImGui::Render();
    // RenderDrawData backs up and restores the GL state it touches, which is
    // the same discipline mgmp_overlay follows by hand -- the game's next frame
    // inherits whatever is left behind.
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace mgmp
