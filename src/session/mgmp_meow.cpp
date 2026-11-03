#include "mgmp_meow.h"
#include "mgmp_gpak.h"
#include "mgmp_log.h"

#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mgmp {
namespace {

// MainMenu's meow button (sub_1401B9140) picks the title cat's voice and one of
// these four moods; here the voice is random too. Voices run male1.. and
// female1.., 205 of them in this build, not every number taken, so a miss just
// draws again.
const char* const kMoods[] = { "angry", "happy", "normal", "sad" };
constexpr int kMaxVoice = 145;
constexpr int kTries    = 8;
// The files are mastered for the game's mixer, which plays them under the
// music; on their own through the system they stand out.
constexpr double kGain = 0.6;

CRITICAL_SECTION g_cs;
bool     g_cs_ready = false;
uint8_t* g_playing  = nullptr;   // PlaySound reads from it until the sound ends
volatile LONG g_busy = 0;

bool find_meow(uint8_t** data, uint32_t* size, char* name, size_t cap) {
    for (int t = 0; t < kTries; ++t) {
        const char* sex  = (rand() & 1) ? "male" : "female";
        const int   n    = 1 + rand() % kMaxVoice;
        const char* mood = kMoods[rand() % 4];
        _snprintf_s(name, cap, _TRUNCATE, "audio/voices/%s%d/%s%02d.wav", sex, n, mood,
                    1 + rand() % 4);
        if (gpak_read(name, data, size)) return true;
    }
    return false;
}

// 16-bit PCM only; anything else plays as it is.
void soften(uint8_t* wav, uint32_t size) {
    if (size < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return;
    uint16_t bits = 0;
    uint32_t p = 12;
    while (p + 8 <= size) {
        uint32_t len = 0;
        memcpy(&len, wav + p + 4, 4);
        if (!memcmp(wav + p, "fmt ", 4) && p + 8 + 16 <= size) memcpy(&bits, wav + p + 8 + 14, 2);
        if (!memcmp(wav + p, "data", 4)) {
            if (bits != 16) return;
            int16_t* s = (int16_t*)(wav + p + 8);
            const uint32_t n = (len < size - p - 8 ? len : size - p - 8) / 2;
            for (uint32_t i = 0; i < n; ++i) s[i] = (int16_t)(s[i] * kGain);
            return;
        }
        p += 8 + len + (len & 1);
    }
}

DWORD WINAPI meow_thread(LPVOID) {
    uint8_t* data = nullptr;
    uint32_t size = 0;
    char     name[96];
    if (find_meow(&data, &size, name, sizeof(name))) {
        soften(data, size);
        EnterCriticalSection(&g_cs);
        PlaySoundW(nullptr, nullptr, 0);   // the last one stops, so its buffer can go
        free(g_playing);
        g_playing = data;
        PlaySoundW((LPCWSTR)data, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        LeaveCriticalSection(&g_cs);
    } else {
        log_line("FOLLOW", "no meow found in %d tries", kTries);
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

} // namespace

void meow_play() {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
        srand((unsigned)GetTickCount64());
    }
    // One at a time: two picks in the same instant are one meow.
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    HANDLE t = CreateThread(nullptr, 0, &meow_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else   InterlockedExchange(&g_busy, 0);
}

} // namespace mgmp
