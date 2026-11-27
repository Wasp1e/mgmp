#include "mgmp_peerlog.h"
#include "mgmp_net.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>
#include <string>
#include <vector>

namespace mgmp {
namespace {

constexpr uint64_t kAgainMs = 20000;   // one log per burst of trouble

uint64_t g_sent_at  = 0;               // client: the last log out
uint64_t g_asked_at = 0;               // host: the last request out
char     g_saved[MAX_PATH * 2] = {};   // host: the last log in, UTF-8
wchar_t g_report_dir[MAX_PATH * 2]{};
char g_report_path[MAX_PATH * 6]{};
char g_halt_reason[256]{};
std::string g_report_header;
uint64_t g_report_at = 0;
bool g_report_peer[kMaxPeers]{};
bool g_report_expected[kMaxPeers]{};

// The log is opened deny-write, not deny-read, and flushed at every line, so
// this reads exactly what is on disk. Only the end is sent: the trouble is at
// the end, and the frame has a cap.
bool read_tail(uint8_t** out, uint32_t* size) {
    const wchar_t* path = log_file_path();
    if (!path || !path[0]) return false;
    FILE* f = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const long long len = _ftelli64(f);
    const long long n   = len < (long long)kMaxLogBytes ? len : (long long)kMaxLogBytes;
    uint8_t* buf = n > 0 ? (uint8_t*)malloc((size_t)n) : nullptr;
    bool ok = false;
    if (buf) {
        _fseeki64(f, len - n, SEEK_SET);
        ok = fread(buf, 1, (size_t)n, f) == (size_t)n;
    }
    fclose(f);
    if (!ok) { free(buf); return false; }
    *out  = buf;
    *size = (uint32_t)n;
    return true;
}

void send_log(const char* why) {
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!read_tail(&data, &size)) {
        log_line("PEERLOG", "!! could not read this peer's log to send it");
        return;
    }
    LogFileMsg m{};
    m.size = size;
    m.data = data;
    strncpy_s(m.reason, why ? why : "", _TRUNCATE);
    const wchar_t* path  = log_file_path();
    const wchar_t* slash = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, m.name, sizeof(m.name),
                        nullptr, nullptr);
    log_line("PEERLOG", "-> this peer's log to the host, the last %u bytes (%s)", size, m.reason);
    if (!net_send_logfile(m)) log_line("PEERLOG", "!! the log did not go out");
    free(data);
    g_sent_at = GetTickCount64();
}

void ask_host_side(const char* why) {
    ChoiceMsg c{};
    c.kind = kChoiceLogRequest;
    strncpy_s(c.name, why ? why : "", _TRUNCATE);
    net_send_choice(c);
    g_asked_at = GetTickCount64();
    log_line("PEERLOG", "-> asked the other player for their log (%s)", c.name);
}

// File names come from the other machine: only the safe characters stay.
void clean_name(const char* in, wchar_t* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        out[n++] = ok ? (wchar_t)c : L'_';
    }
    out[n] = 0;
    if (!n || wcsstr(out, L"..")) wcsncpy_s(out, cap, L"peer.log", _TRUNCATE);
}

} // namespace

const char* peerlog_halt_reason() { return g_halt_reason; }
const char* peerlog_report_path() { return g_report_path; }

void peerlog_refresh_report() {
    if (!g_report_dir[0]) return;
    wchar_t path[MAX_PATH * 2]{};
    _snwprintf_s(path, _TRUNCATE, L"%s\\report.txt", g_report_dir);
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") || !file) {
        log_line("PEERLOG", "!! could not write HALT report (error %lu)", GetLastError());
        g_report_path[0] = 0;
        return;
    }
    const char bom[] = "\xEF\xBB\xBF";
    fwrite(bom, 1, 3, file);
    fprintf(file, "%s\n", g_report_header.c_str());
    fprintf(file, "Peer logs received");
    for (uint8_t i = 0; i < kMaxPeers; ++i)
        if (g_report_expected[i]) fprintf(file, "\n  peer %u %s", i, g_report_peer[i] ? "received" : "pending/unavailable (connection may be lost)");
    if (net_role() != NetRole::Host) fprintf(file, "\n  Other players' logs are collected on the host.");
    fprintf(file, "\n\nRecent actions and state differences\n");
    uint32_t cursor = 0;
    LogEntry batch[64];
    std::vector<LogEntry> actions, differences;
    while (const uint32_t count = log_ring_fetch(cursor, batch, 64, nullptr)) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = batch[i];
            if (!strcmp(entry.tag, "APPLY") || !strcmp(entry.tag, "DOACTION") ||
                !strcmp(entry.tag, "TRIGGER") || !strcmp(entry.tag, "CHOICE"))
                actions.push_back(entry);
            if (!strcmp(entry.tag, "LOCKSTEP") &&
                (strstr(entry.text, "DIFF") || strstr(entry.text, "SIZE") ||
                 strstr(entry.text, "mismatch") || strstr(entry.text, " cat ") ||
                 strstr(entry.text, "HALT")))
                differences.push_back(entry);
        }
    }
    const size_t start = actions.size() > 32 ? actions.size() - 32 : 0;
    for (size_t i = start; i < actions.size(); ++i)
        fprintf(file, "%06u turn %u %s %s\n", actions[i].seq, actions[i].turn,
                actions[i].tag, actions[i].text);
    fprintf(file, "\nRecent diagnostic context from the local log ring (may include earlier turns)\n");
    for (const auto& entry : differences)
        fprintf(file, "%06u turn %u %s\n", entry.seq, entry.turn, entry.text);
    fprintf(file, "\nEffect names are only known when logged by the game. A state difference alone does not identify the missing status.\n");
    fclose(file);
    _snwprintf_s(path, _TRUNCATE, L"%s\\local.log", g_report_dir);
    CopyFileW(log_file_path(), path, FALSE);
}

void peerlog_halt_report(const char* reason, uint64_t battle, uint32_t turn) {
    g_report_path[0] = 0;
    const char* summary = "The battle stopped because the players' simulations disagreed.";
    if (reason && strstr(reason, "state only"))
        summary = "The players have different battle state. See the cat and field differences in the report.";
    else if (reason && strstr(reason, "hash mismatch (rng"))
        summary = "The players' random-number sequences diverged. See the report for state differences.";
    else if (reason && strstr(reason, "hash mismatch (queue"))
        summary = "The players have different pending battle actions. See the report for details.";
    strncpy_s(g_halt_reason, summary, _TRUNCATE);
    const wchar_t* source = log_file_path();
    if (!source || !source[0]) return;
    wcsncpy_s(g_report_dir, source, _TRUNCATE);
    wchar_t* slash = wcsrchr(g_report_dir, L'\\');
    if (!slash) { g_report_dir[0] = 0; return; }
    *slash = 0;
    wcscat_s(g_report_dir, L"\\reports");
    if (!CreateDirectoryW(g_report_dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_report_dir[0] = 0;
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t directory[128]{};
    _snwprintf_s(directory, _TRUNCATE, L"\\halt_%04u%02u%02u_%02u%02u%02u_%03u_%016llx_%u",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                 now.wMilliseconds, (unsigned long long)battle, turn);
    wcscat_s(g_report_dir, directory);
    if (!CreateDirectoryW(g_report_dir, nullptr)) { g_report_dir[0] = 0; return; }
    memset(g_report_peer, 0, sizeof(g_report_peer));
    memset(g_report_expected, 0, sizeof(g_report_expected));
    uint8_t ids[kMaxPeers]{};
    if (net_role() == NetRole::Host && net_peer_ids(ids, kMaxPeers))
        for (uint8_t i = 0; i < net_peer_count() && i < kMaxPeers; ++i)
            if (ids[i] < kMaxPeers && ids[i] != net_self()) g_report_expected[ids[i]] = true;
    g_report_at = GetTickCount64();
    WideCharToMultiByte(CP_UTF8, 0, g_report_dir, -1, g_report_path, sizeof(g_report_path), nullptr, nullptr);
    char header[1200]{};
    snprintf(header, sizeof(header),
             "MGMP HALT report\nProtocol %u\nBuild %s %s\nRole %s\nLocal peer %u\nBattle %016llx\nTurn %u\n"
             "Stop reason\n%s\n\nlocal.log is the local log at report refresh. peer_N.log contains the received log tail (up to %u bytes).\n",
             kProtoVersion, __DATE__, __TIME__, net_role() == NetRole::Host ? "host" : "client",
             net_self(), (unsigned long long)battle, turn, reason ? reason : "State mismatch", kMaxLogBytes);
    g_report_header = header;
    peerlog_refresh_report();
    log_line("PEERLOG", "HALT report saved to %s", g_report_path);
}

void peerlog_trouble(const char* why) {
    if (!net_active() || net_peer_count() < 2) return;
    const uint64_t now = GetTickCount64();
    if (net_role() == NetRole::Host) {
        if (now - g_asked_at >= kAgainMs) ask_host_side(why);
    } else if (now - g_sent_at >= kAgainMs) {
        send_log(why);
    }
}

void peerlog_ask(const char* why) {
    if (!net_active()) return;
    if (net_role() == NetRole::Host) ask_host_side(why);
    else                              send_log(why);
}

bool peerlog_on_choice(const ChoiceMsg& m) {
    if (m.kind != kChoiceLogRequest) return false;
    if (net_role() != NetRole::Host) {
        // Asked by hand is always answered; asked by trouble, once a burst.
        if (strstr(m.name, "halt") || GetTickCount64() - g_sent_at >= 2000) send_log(m.name);
    }
    return true;
}

void peerlog_on_message(const LogFileMsg& m, uint8_t from) {
    if (net_role() != NetRole::Host || !m.data || !m.size) return;
    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, log_file_path(), _TRUNCATE);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    wcsncat_s(dir, L"\\friend", _TRUNCATE);
    CreateDirectoryW(dir, nullptr);

    wchar_t name[96];
    clean_name(m.name, name, 96);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\friend%u_got%02d%02d%02d_%s", dir, (unsigned)from,
                 t.wHour, t.wMinute, t.wSecond, name);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) {
        log_line("PEERLOG", "!! could not save the other player's log");
        return;
    }
    fwrite(m.data, 1, m.size, f);
    fclose(f);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, g_saved, sizeof(g_saved), nullptr, nullptr);
    log_line("PEERLOG", "<- the other player's log (%u bytes, %s) saved to %s", m.size,
             m.reason, g_saved);
    if (g_report_dir[0] && strstr(m.reason, "halt") &&
        GetTickCount64() - g_report_at < 120000 && from < kMaxPeers && g_report_expected[from]) {
        wchar_t report_peer[MAX_PATH * 2]{};
        _snwprintf_s(report_peer, _TRUNCATE, L"%s\\peer_%u.log", g_report_dir, from);
        if (CopyFileW(path, report_peer, FALSE)) {
            if (from < kMaxPeers) g_report_peer[from] = true;
            peerlog_refresh_report();
        }
    }
}

const char* peerlog_last_saved() { return g_saved; }

} // namespace mgmp
