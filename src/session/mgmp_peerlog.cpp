#include "mgmp_peerlog.h"
#include "mgmp_net.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <share.h>

namespace mgmp {
namespace {

constexpr uint64_t kAgainMs = 20000;   // one log per burst of trouble

uint64_t g_sent_at  = 0;               // client: the last log out
uint64_t g_asked_at = 0;               // host: the last request out
char     g_saved[MAX_PATH * 2] = {};   // host: the last log in, UTF-8

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
        if (GetTickCount64() - g_sent_at >= 2000) send_log(m.name);
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
}

const char* peerlog_last_saved() { return g_saved; }

} // namespace mgmp
