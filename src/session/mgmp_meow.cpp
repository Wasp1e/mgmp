#include "mgmp_meow.h"
#include "mgmp_gpak.h"
#include "mgmp_log.h"
#include "mgmp_config.h"

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
float g_volume      = 1.0f;   // the panel's slider, on top of kGain
bool  g_volume_read = false;

CRITICAL_SECTION g_cs;
bool     g_cs_ready = false;
uint8_t* g_playing  = nullptr;   // PlaySound reads from it until the sound ends
uint64_t g_seq      = 0;         // the newest meow asked for; an older one does not play

// The names come from the seed alone, by splitmix64 rather than the CRT's rand,
// so the two peers turn one seed into the same list and hear the same meow.
uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

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
            const double gain = kGain * g_volume;
            for (uint32_t i = 0; i < n; ++i) s[i] = (int16_t)(s[i] * gain);
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

// mgmp_meow.txt beside mgmp.dll: one number, the volume.
void volume_path(wchar_t* out, size_t cap) {
    wcsncpy_s(out, cap, config().log_path, _TRUNCATE);
    wchar_t* slash = wcsrchr(out, L'\\');
    wcsncpy_s(slash ? slash + 1 : out, cap - (slash ? slash + 1 - out : 0), L"mgmp_meow.txt", _TRUNCATE);
}

void read_volume() {
    if (g_volume_read) return;
    g_volume_read = true;
    wchar_t path[MAX_PATH];
    volume_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return;
    float v = 1.0f;
    if (fscanf_s(f, "%f", &v) == 1 && v >= 0.0f && v <= 1.0f) g_volume = v;
    fclose(f);
}

} // namespace

float meow_volume() { read_volume(); return g_volume; }

void meow_set_volume(float v) {
    read_volume();
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_volume = v;
    wchar_t path[MAX_PATH];
    volume_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") || !f) return;
    fprintf(f, "%.2f\n", v);
    fclose(f);
}

void ensure_meow() {
    if (g_cs_ready) return;
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
    srand((unsigned)GetTickCount64());
}

uint32_t meow_new_seed() {
    ensure_meow();
    uint32_t s = 0;
    while (!s) s = ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 2) ^ (uint32_t)GetTickCount64();
    return s;
}

void meow_play(uint32_t seed) {
    ensure_meow();
    read_volume();
    if (g_volume <= 0.001f) return;   // the slider all the way down: no meow at all
    if (!seed) seed = meow_new_seed();
    // Voice, mood and take, a few of them in case a voice number is not one the
    // game has -- all from the seed, so a miss is the same miss on both peers.
    Job* job = new Job;
    EnterCriticalSection(&g_cs);
    job->seq = ++g_seq;
    LeaveCriticalSection(&g_cs);
    uint64_t s = seed;
    for (int t = 0; t < kTries; ++t) {
        const uint64_t r = splitmix(s);
        _snprintf_s(job->name[t], sizeof(job->name[t]), _TRUNCATE,
                    "audio/voices/%s%d/%s%02d.wav", (r & 1) ? "male" : "female",
                    1 + (int)((r >> 8) % kMaxVoice), kMoods[(r >> 24) % 4],
                    1 + (int)((r >> 32) % 4));
    }
    HANDLE t = CreateThread(nullptr, 0, &meow_thread, job, 0, nullptr);
    if (t) CloseHandle(t);
    else   delete job;
}

} // namespace mgmp
