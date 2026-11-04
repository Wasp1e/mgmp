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
uint64_t g_seq      = 0;         // the newest meow asked for; an older one does not play

// The draws are made on the CALLER's thread and handed over: the CRT's rand
// state is per thread, and a fresh thread's starts at seed 1 -- every meow made
// in one would have been the same meow.
struct Job {
    uint64_t seq;
    char     name[kTries][96];
};

bool find_meow(const Job& job, uint8_t** data, uint32_t* size, const char** name) {
    for (int t = 0; t < kTries; ++t) {
        if (gpak_read(job.name[t], data, size)) { *name = job.name[t]; return true; }
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

DWORD WINAPI meow_thread(LPVOID p) {
    Job* job = (Job*)p;
    uint8_t*    data = nullptr;
    uint32_t    size = 0;
    const char* name = nullptr;
    if (find_meow(*job, &data, &size, &name)) {
        soften(data, size);
        EnterCriticalSection(&g_cs);
        // Clicks faster than a file is read: the last click's meow is the one heard.
        if (job->seq >= g_seq) {
            PlaySoundW(nullptr, nullptr, 0);   // the last one stops, so its buffer can go
            free(g_playing);
            g_playing = data;
            data = nullptr;
            PlaySoundW((LPCWSTR)g_playing, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
        LeaveCriticalSection(&g_cs);
        free(data);
    } else {
        log_line("FOLLOW", "no meow found in %d tries", kTries);
    }
    delete job;
    return 0;
}

} // namespace

void meow_play() {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
        srand((unsigned)GetTickCount64());
    }
    // A fresh draw for every click: voice, mood and take, a few of them in case
    // a voice number is not one the game has.
    Job* job = new Job;
    EnterCriticalSection(&g_cs);
    job->seq = ++g_seq;
    LeaveCriticalSection(&g_cs);
    for (int t = 0; t < kTries; ++t)
        _snprintf_s(job->name[t], sizeof(job->name[t]), _TRUNCATE,
                    "audio/voices/%s%d/%s%02d.wav", (rand() & 1) ? "male" : "female",
                    1 + rand() % kMaxVoice, kMoods[rand() % 4], 1 + rand() % 4);
    HANDLE t = CreateThread(nullptr, 0, &meow_thread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else   delete job;
}

} // namespace mgmp
