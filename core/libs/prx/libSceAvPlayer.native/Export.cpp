// libSceAvPlayer: movie playback with FFmpeg. A source is opened through the title's own file callbacks when it
// gives some (GT Sport's movies sit inside its archive volumes) or from the host file otherwise, its H.264 / HEVC
// video and AAC audio are decoded on a guest thread of the player's own (the file callbacks are guest code), and
// the getters hand out NV12 frames in the title's texture memory and 16-bit PCM in its memory pool, on the
// player's media clock. The end of the stream is reported (STOP) by the poll that finds nothing left.
//
// When a source cannot be decoded (no demuxer, no supported stream, no frame storage), the player falls back to
// the old behaviour: a short blank clip on a wall clock, then STOP, so a title's movie sequence still ends.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fstream>
#include <iterator>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "SceTypes.hpp"
#include "HitLog.hpp"

extern "C" int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
extern "C" int APS5_VABI scePthreadDetach(Pthread thread);
extern "C" int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
#include "prx/libc/include/General.hpp"

namespace {
constexpr std::int32_t kEventStop = 0x01;
constexpr std::int32_t kEventReady = 0x02;
constexpr std::int32_t kEventPlay = 0x03;
constexpr std::int32_t kEventPause = 0x04;
constexpr std::int32_t kEventWarning = 0x20;
constexpr std::int32_t kErrInvalidParams = static_cast<std::int32_t>(0x806a0001u);
constexpr std::int32_t kErrOperationFailed = static_cast<std::int32_t>(0x806a0002u);
constexpr std::uint32_t kWarningJumpComplete = 0x806a00a3u;
constexpr std::uint32_t kWarningLoopingBack = 0x806a00a2u;
constexpr std::uint64_t kMagic = 0x5041564c50594c41ull;

// SceAvPlayerStreamType
constexpr std::uint32_t kStreamVideo = 0;
constexpr std::uint32_t kStreamAudio = 1;

// The blank fallback clip.
constexpr std::uint32_t kFrameWidth = 1920;
constexpr std::uint32_t kFrameHeight = 1080;
constexpr std::uint32_t kFramePitch = (kFrameWidth + 255u) & ~255u;
constexpr std::size_t kFrameBytes = static_cast<std::size_t>(kFramePitch) * kFrameHeight * 3u / 2u;
// Limited-range black: Y=U=V=0 converts to mid green.
constexpr std::uint8_t kBlackLuma = 0x10;
constexpr std::uint8_t kBlackChroma = 0x80;
constexpr std::uint64_t kFrameIntervalMs = 33;
constexpr std::uint64_t kFrameBudget = 120;
constexpr std::uint64_t kStreamDurationMs = kFrameBudget * kFrameIntervalMs;
// The skipped opening movie: about 0.2 s of black.
constexpr std::uint64_t kSkipFrames = 6;
constexpr double kFrameRate = 1000.0 / static_cast<double>(kFrameIntervalMs);

// Decoded audio frames are at most this many samples per channel (AAC: 1024), 8 channels of 32-bit at most.
constexpr std::size_t kAudioBufferBytes = 8u * 4u * 2048u;

using EventCallback = void (APS5_VABI *)(void*, std::int32_t, std::int32_t, void*);
using MemAlloc = void* (APS5_VABI *)(void*, std::uint32_t, std::uint32_t);
using MemFree = void (APS5_VABI *)(void*, void*);
using FileOpen = std::int32_t (APS5_VABI *)(void*, const char*);
using FileClose = std::int32_t (APS5_VABI *)(void*);
using FileReadOffset = std::int32_t (APS5_VABI *)(void*, std::uint8_t*, std::uint64_t, std::uint32_t);
using FileSize = std::uint64_t (APS5_VABI *)(void*);

// SceAvPlayerVideoEx. The offsets are asserted below because the guest reads them by position, not by name.
struct VideoDetailsEx {
    std::uint32_t width;
    std::uint32_t height;
    float aspectRatio;
    std::uint8_t language[4];
    std::uint8_t reserved0[4];
    std::uint32_t cropLeftOffset;
    std::uint32_t cropRightOffset;
    std::uint32_t cropTopOffset;
    std::uint32_t cropBottomOffset;
    std::uint32_t pitch;
    std::uint8_t lumaBitDepth;
    std::uint8_t chromaBitDepth;
    std::uint8_t fullRange;
    std::uint8_t reserved1[5];
    double frameRate;
    std::uint32_t colourPrimaries;
    std::uint32_t transferCharacteristics;
    std::uint8_t reserved2[16];
};
static_assert(sizeof(VideoDetailsEx) == 80, "SceAvPlayerVideoEx ABI");
static_assert(offsetof(VideoDetailsEx, cropLeftOffset) == 20, "SceAvPlayerVideoEx ABI");
static_assert(offsetof(VideoDetailsEx, pitch) == 36, "SceAvPlayerVideoEx ABI");
static_assert(offsetof(VideoDetailsEx, frameRate) == 48, "SceAvPlayerVideoEx ABI");

// SceAvPlayerAudio (the audio member of the 16-byte details union).
struct AudioDetails {
    std::uint16_t channelCount;
    std::uint8_t reserved[2];
    std::uint32_t sampleRate;
    std::uint32_t size;
    std::uint8_t language[4];
};
static_assert(sizeof(AudioDetails) == 16, "SceAvPlayerAudio ABI");

// SceAvPlayerFrameInfo / SceAvPlayerFrameInfoEx: both put their timestamp at 16 and their details at 24.
struct FrameInfo {
    void* data;
    std::uint8_t reserved[4];
    std::uint64_t timestamp;
    std::uint8_t details[16];
};
struct FrameInfoEx {
    void* data;
    std::uint8_t reserved[4];
    std::uint64_t timestamp;
    VideoDetailsEx video;
};
static_assert(sizeof(FrameInfo) == 40 && offsetof(FrameInfo, timestamp) == 16, "SceAvPlayerFrameInfo ABI");
static_assert(sizeof(FrameInfoEx) == 104 && offsetof(FrameInfoEx, video) == 24, "SceAvPlayerFrameInfoEx ABI");

// The allocator, file and event blocks shared by sceAvPlayerInitData and sceAvPlayerInitDataEx.
struct Replacements {
    void* memObject;
    void* alloc;
    void* dealloc;
    void* allocTexture;
    void* deallocTexture;
    void* fileObject;
    void* fileOpen;
    void* fileClose;
    void* fileRead;
    void* fileSize;
    void* eventObject;
    void* eventCallback;
};
static_assert(sizeof(Replacements) == 96, "SceAvPlayerInitData replacement blocks");

struct Player;

// One decoded picture or block of samples, in guest memory owned by the player.
struct Frame {
    std::uint8_t* data = nullptr;
    std::uint64_t timestamp = 0;
    std::uint32_t bytes = 0;
};

// The movie being decoded. Everything but the queues is touched by the decoder thread only, after AddSource.
struct Decoder {
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    // Title file callbacks (guest code: only ever called on guest threads).
    void* fileObject = nullptr;
    FileOpen fileOpen = nullptr;
    FileClose fileClose = nullptr;
    FileReadOffset fileRead = nullptr;
    FileSize fileSize = nullptr;
    bool fileOpened = false;
    std::uint64_t filePosition = 0;
    std::uint64_t fileLength = 0;

    struct Stream {
        int index;          // FFmpeg stream index
        std::uint32_t type; // kStreamVideo / kStreamAudio
        bool enabled = false;
    };
    std::vector<Stream> streams;
    std::uint64_t durationMs = 0;

    int videoIndex = -1;
    AVCodecContext* videoCodec = nullptr;
    SwsContext* sws = nullptr;
    std::uint32_t width = 0, height = 0;   // visible picture
    std::uint32_t alignedWidth = 0, alignedHeight = 0, pitch = 0;
    std::size_t frameBytes = 0;
    float aspect = 16.0f / 9.0f;
    double frameRate = 30.0;

    int audioIndex = -1;
    AVCodecContext* audioCodec = nullptr;
    SwrContext* swr = nullptr;
    std::uint32_t channels = 0, sampleRate = 0;

    // Shared with the getters (under mutex).
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::uint8_t*> freeVideo, freeAudio;
    std::vector<std::uint8_t*> allVideo, allAudio;
    std::deque<Frame> video, audio;
    Frame currentVideo, currentAudio;
    bool haveCurrentVideo = false, haveCurrentAudio = false;
    bool eof = false;
    bool stop = false;
    bool failed = false;
    bool looping = false;
    Pthread thread = nullptr; // reader (guest thread: the file callbacks are guest code)
    std::thread videoThread, audioThread;
    // Demuxed packets waiting for their decoder, with the loop pass they belong to (looped sources restart
    // their timestamps; each pass is shifted by the duration). A null packet asks the decoder to drain.
    struct Packet {
        AVPacket* packet;
        std::uint32_t loop;
    };
    std::deque<Packet> videoPackets, audioPackets;
    bool readerDone = false, videoDone = false, audioDone = false;
    std::uint64_t framesDecoded = 0;
    // Audio clock: the timestamp of the last audio block the title took, and when. Video is shown against it,
    // so pictures follow the sound the title is playing rather than wall time (the decoder can only run as far
    // ahead as the title takes audio; against wall time, video fell behind and most frames were skipped).
    bool audioClock = false;
    std::uint64_t audioTs = 0;
    std::chrono::steady_clock::time_point audioAt{};
    // Statistics (logged every 2 s while playing).
    std::uint64_t statDecodeUs = 0, statStoreUs = 0, statWaitUs = 0, statFrames = 0;
    std::atomic<std::uint64_t> statDelivered{0}, statSkipped{0}, statMissed{0};
    // PRT-AVSYNC: snaps of the video clock to audio and the last drift seen (video minus audio, ms).
    std::atomic<std::uint64_t> statSnaps{0};
    std::atomic<std::int64_t> statDriftMs{0};
    std::chrono::steady_clock::time_point statSince = std::chrono::steady_clock::now();
    // PRT-AVTEX (Build B3): frames shown stay out of the free pool for `hold` more takes (RYGT_AVP_HOLD), and
    // the free pool is taken oldest first while holding. releasedAt: when each buffer stopped being shown.
    std::uint32_t hold = 0;
    std::deque<std::uint8_t*> heldVideo;
    std::vector<std::pair<std::uint8_t*, std::chrono::steady_clock::time_point>> releasedAt;
    std::uint64_t statRewrite20 = 0, statRewrite50 = 0; // buffers rewritten within 20 / 50 ms of being shown
    std::uint64_t texLuma = 0, texAll = 0, texFaults = 0; // renderer counters at the last 2 s line
    // PRT-AVAUDIO (Build B3): the first 5 s of audio pulls after playback starts.
    std::chrono::steady_clock::time_point avaStart{}, avaLastPull{};
    bool avaLogged = false;
    std::uint64_t avaPulls = 0, avaEmpty = 0, avaGaps = 0, avaJumps = 0, avaRepeats = 0;
    std::int64_t avaMinQueuedMs = -1, avaFirstPullMs = -1;
    std::uint64_t avaLastTs = 0, avaLastDurMs = 0;
    bool avaHaveLast = false;
    std::uint64_t avaPullerCores = 0;
    // PRT-AVAUDIO v2 (Build B4): when each pull gap over 30 ms came and how long it was (first 16), and the
    // first block handed out.
    std::string avaGapList;
    unsigned avaGapListed = 0;
    std::int64_t avaFirstTs = -1, avaFirstDurMs = -1;
    // PRT-AVFADE (Build B4): samples per channel already faded in at the start of the stream.
    std::uint64_t fadeDone = 0;
    std::atomic<std::uint64_t> avaVideoCores{0}, avaAudioCores{0};
};

std::uint64_t MicrosSince(std::chrono::steady_clock::time_point t) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
}

struct Player {
    std::uint64_t magic = kMagic;
    void* eventObject = nullptr;
    EventCallback callback = nullptr;
    void* memObject = nullptr;
    MemAlloc alloc = nullptr;
    MemFree dealloc = nullptr;
    MemAlloc allocTexture = nullptr;
    MemFree deallocTexture = nullptr;
    void* fileObject = nullptr;
    FileOpen fileOpen = nullptr;
    FileClose fileClose = nullptr;
    FileReadOffset fileRead = nullptr;
    FileSize fileSize = nullptr;
    bool autoStart = false;
    bool haveSource = false;
    bool playing = false;
    bool paused = false;
    bool stopFired = false;
    bool looping = false;
    bool noStorage = false;
    bool skipped = false; // the opening movie, turned off in the launcher: ends at once
    std::int32_t numFramebuffers = 0;
    // Media clock: bankedMs is the media time reached at clockAnchor; a cleared anchor means stopped.
    std::chrono::steady_clock::time_point clockAnchor{};
    std::uint64_t bankedMs = 0;
    std::uint64_t framesDelivered = 0;
    std::string path;
    std::vector<std::uint8_t*> frames; // blank clip
    Decoder* decoder = nullptr;        // null: blank clip
    std::mutex lock;
};

Player* Check(void* handle) {
    Player* p = static_cast<Player*>(handle);
    return p != nullptr && p->magic == kMagic ? p : nullptr;
}

// The callback runs guest code and may re-enter the player, so it is always invoked without the player lock held.
void Fire(Player* p, std::int32_t event, void* data = nullptr) {
    if (p->callback == nullptr) return;
    static std::atomic<unsigned> fired[64];
    const auto slot = static_cast<unsigned>(event) < 64 ? static_cast<unsigned>(event) : 0u;
    if (fired[slot].fetch_add(1) < 3)
        std::fprintf(stderr, "[AVP] event 0x%02x\n", event);
    p->callback(p->eventObject, event, 0, data);
}

// The end of a movie is found inside the title's own polls, on its movie thread. On the console the STOP event
// comes from the player's own thread; GT Sport's handler joins the movie thread that polls, so delivered on that
// thread it joined itself. The end is reported from a thread of its own.
struct AsyncEvent {
    EventCallback callback;
    void* object;
    std::int32_t event;
};
void* APS5_VABI AsyncEventRunner(void* arg) {
    auto* e = static_cast<AsyncEvent*>(arg);
    e->callback(e->object, e->event, 0, nullptr);
    delete e;
    return nullptr;
}
void FireAsync(Player* p, std::int32_t event) {
    if (p->callback == nullptr) return;
    std::fprintf(stderr, "[AVP] event 0x%02x (from its own thread)\n", event);
    auto* e = new AsyncEvent{p->callback, p->eventObject, event};
    Pthread thread{};
    if (scePthreadCreate(&thread, nullptr, AsyncEventRunner, e, "AvPlayerEvent") != 0) {
        delete e;
        Fire(p, event);
        return;
    }
    scePthreadDetach(thread);
}

// PRT-AVSYNC (pr-tests): RYGT_AVP_SMOOTH_CLOCK (on by default, =0 old behaviour). The video clock slews toward
// the audio clock (at most 25% fast or slow) and only snaps past 500 ms of drift; frames are dropped only when
// more than 100 ms late. The old clock snapped at 80 ms drift and dropped at 50 ms, which skipped 3 to 6 frames
// every 2 s in 60 fps MP4 cutscenes.
bool AvpSmoothClock() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_AVP_SMOOTH_CLOCK");
        const bool v = !(env && env[0] == '0');
        std::fprintf(stderr, "[AVP] PRT-AVSYNC: smooth clock %s (RYGT_AVP_SMOOTH_CLOCK)\n", v ? "on" : "off");
        return v;
    }();
    return on;
}

// Build B3 switches (pr-tests, all off by default).
// RYGT_AVP_HOLD=N: a frame buffer the title was shown stays out of the decoder's reach for N more takes, and the
// free pool is taken oldest first. Without it the buffer just released was the next one overwritten, ~2 ms later,
// while the GPU could still be reading it (the decoder waits ~1.7 s of every 2 s for a free buffer).
std::uint32_t AvpHold() {
    static const std::uint32_t n = [] {
        const char* env = std::getenv("RYGT_AVP_HOLD");
        const int v = env ? std::atoi(env) : 0;
        const auto r = static_cast<std::uint32_t>(std::clamp(v, 0, 8));
        std::fprintf(stderr, "[AVP] PRT-AVTEX: frame hold %u (RYGT_AVP_HOLD)\n", r);
        return r;
    }();
    return n;
}
// RYGT_AVP_INVALIDATE=1: the renderer is told before the decoder writes a frame into guest memory (as file reads
// do), so the texture is marked changed at once instead of on ~760 write faults per frame.
bool AvpInvalidate() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_AVP_INVALIDATE");
        const bool v = env && env[0] == '1';
        std::fprintf(stderr, "[AVP] PRT-AVTEX: invalidate before write %s (RYGT_AVP_INVALIDATE)\n", v ? "on" : "off");
        return v;
    }();
    return on;
}
// RYGT_AVP_PREFILL_MS=N: Start waits (at most 500 ms) until N ms of audio and a picture are decoded, and the
// clock starts after that.
// RYGT_AVP_AUDIO_FADEIN_MS=N (PRT-AVFADE, Build B4, off): the first N ms of a cutscene's sound rise from
// silence, against the crack at its start.
std::uint32_t AvpFadeInMs() {
    static const std::uint32_t n = [] {
        const char* env = std::getenv("RYGT_AVP_AUDIO_FADEIN_MS");
        const int v = env ? std::atoi(env) : 0;
        const auto r = static_cast<std::uint32_t>(std::clamp(v, 0, 200));
        std::fprintf(stderr, "[AVP] PRT-AVFADE: audio fade-in %u ms (RYGT_AVP_AUDIO_FADEIN_MS)\n", r);
        return r;
    }();
    return n;
}
std::uint32_t AvpPrefillMs() {
    static const std::uint32_t n = [] {
        const char* env = std::getenv("RYGT_AVP_PREFILL_MS");
        const int v = env ? std::atoi(env) : 0;
        const auto r = static_cast<std::uint32_t>(std::clamp(v, 0, 400));
        std::fprintf(stderr, "[AVP] PRT-AVAUDIO: audio prefill %u ms (RYGT_AVP_PREFILL_MS)\n", r);
        return r;
    }();
    return n;
}

// The renderer's exports (libSceVideoOut.prx is the renderer); null when an older renderer lacks them.
template <typename F>
F RendererExport(const char* name) {
    HMODULE renderer = GetModuleHandleA("libSceVideoOut.prx");
    return renderer != nullptr ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(renderer, name))) : nullptr;
}
void GpuInvalidate(void* buffer, std::size_t bytes) {
    using Fn = void (*)(std::uint64_t, std::uint64_t);
    static const Fn fn = RendererExport<Fn>("RytyGpuInvalidateMemory");
    if (fn != nullptr && buffer != nullptr && bytes != 0) fn(reinterpret_cast<std::uint64_t>(buffer), bytes);
}
void GpuAvTexRange(void* buffer, std::size_t bytes, bool add) {
    using Fn = void (*)(std::uint64_t, std::uint64_t, int);
    static const Fn fn = RendererExport<Fn>("RytyGpuAvTexRange");
    if (fn != nullptr && buffer != nullptr) fn(reinterpret_cast<std::uint64_t>(buffer), bytes, add ? 1 : 0);
}
bool GpuAvTexStats(std::uint64_t& luma, std::uint64_t& all, std::uint64_t& faults) {
    using Fn = void (*)(std::uint64_t*, std::uint64_t*, std::uint64_t*);
    static const Fn fn = RendererExport<Fn>("RytyGpuAvTexStats");
    if (fn == nullptr) return false;
    fn(&luma, &all, &faults);
    return true;
}
std::uint64_t CoreBit() {
    const DWORD n = GetCurrentProcessorNumber();
    return n < 64 ? (1ull << n) : 0;
}

// Caller holds d->mutex.
std::uint64_t QueuedAudioMs(const Decoder& d) {
    const std::uint64_t perMs = static_cast<std::uint64_t>(d.channels) * 2u * d.sampleRate / 1000u;
    if (perMs == 0) return 0;
    std::uint64_t bytes = 0;
    for (const auto& f : d.audio) bytes += f.bytes;
    return bytes / perMs;
}

// Caller holds d->mutex. A buffer that stopped being shown: held for `hold` takes, then free.
void ReleaseShown(Decoder& d, std::uint8_t* buffer) {
    const auto now = std::chrono::steady_clock::now();
    bool found = false;
    for (auto& r : d.releasedAt)
        if (r.first == buffer) { r.second = now; found = true; break; }
    if (!found) d.releasedAt.emplace_back(buffer, now);
    if (d.hold == 0) {
        d.freeVideo.push_back(buffer);
        return;
    }
    d.heldVideo.push_back(buffer);
    while (d.heldVideo.size() > d.hold) {
        d.freeVideo.push_back(d.heldVideo.front());
        d.heldVideo.pop_front();
    }
}

void LogAudioStart(Decoder& d, const char* why) {
    if (d.avaLogged) return;
    d.avaLogged = true;
    std::fprintf(stderr, "[AVP] PRT-AVAUDIO (%s): %llu pulls, %llu empty, lowest queued %lld ms, %llu gaps over 30 ms "
                 "between pulls, first pull %lld ms after start, %llu timestamp jumps, %llu repeats; cores: title "
                 "puller 0x%llx, audio decoder 0x%llx, video decoder 0x%llx (SceSndzAudioOutMain on P-cores)\n",
                 why, static_cast<unsigned long long>(d.avaPulls), static_cast<unsigned long long>(d.avaEmpty),
                 static_cast<long long>(d.avaMinQueuedMs), static_cast<unsigned long long>(d.avaGaps),
                 static_cast<long long>(d.avaFirstPullMs), static_cast<unsigned long long>(d.avaJumps),
                 static_cast<unsigned long long>(d.avaRepeats), static_cast<unsigned long long>(d.avaPullerCores),
                 static_cast<unsigned long long>(d.avaAudioCores.load()),
                 static_cast<unsigned long long>(d.avaVideoCores.load()));
    std::fprintf(stderr, "[AVP] PRT-AVAUDIO v2: first block %lld ms long at timestamp %lld ms; gaps over 30 ms "
                 "(ms after start: length):%s\n",
                 static_cast<long long>(d.avaFirstDurMs), static_cast<long long>(d.avaFirstTs),
                 d.avaGapList.empty() ? " none" : d.avaGapList.c_str());
    std::fflush(stderr);
}

// Caller holds d->mutex. One audio pull of the title while playing (got: a block was handed out).
void NoteAudioPull(Decoder& d, bool got) {
    if (d.avaLogged || d.avaStart == std::chrono::steady_clock::time_point{}) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - d.avaStart >= std::chrono::seconds(5)) {
        LogAudioStart(d, "first 5 s");
        return;
    }
    if (d.avaPulls == 0)
        d.avaFirstPullMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - d.avaStart).count();
    else if (now - d.avaLastPull > std::chrono::milliseconds(30)) {
        ++d.avaGaps;
        if (d.avaGapListed++ < 16) {
            char text[64];
            std::snprintf(text, sizeof(text), " %lld: %lld",
                          static_cast<long long>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(now - d.avaStart).count()),
                          static_cast<long long>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(now - d.avaLastPull).count()));
            d.avaGapList += text;
        }
    }
    d.avaLastPull = now;
    ++d.avaPulls;
    d.avaPullerCores |= CoreBit();
    if (!got) {
        if (!d.eof) ++d.avaEmpty;
        return;
    }
    const auto queued = static_cast<std::int64_t>(QueuedAudioMs(d));
    if (d.avaMinQueuedMs < 0 || queued < d.avaMinQueuedMs) d.avaMinQueuedMs = queued;
    const std::uint64_t ts = d.currentAudio.timestamp;
    const std::uint64_t perMs = static_cast<std::uint64_t>(d.channels) * 2u * d.sampleRate / 1000u;
    const std::uint64_t dur = perMs != 0 ? d.currentAudio.bytes / perMs : 0;
    if (d.avaHaveLast) {
        if (ts <= d.avaLastTs) ++d.avaRepeats;
        else if (ts > d.avaLastTs + d.avaLastDurMs + 5) ++d.avaJumps;
    }
    if (d.avaFirstTs < 0) {
        d.avaFirstTs = static_cast<std::int64_t>(ts);
        d.avaFirstDurMs = static_cast<std::int64_t>(dur);
    }
    d.avaLastTs = ts;
    d.avaLastDurMs = dur;
    d.avaHaveLast = true;
}

// Caller holds the lock.
std::uint64_t MediaTime(Player& p) {
    const auto now = std::chrono::steady_clock::now();
    if (p.decoder != nullptr && p.playing && !p.paused) {
        Decoder* d = p.decoder;
        std::lock_guard lk(d->mutex);
        if (d->audioClock && now - d->audioAt < std::chrono::milliseconds(500)) {
            // The wall clock, kept within 80 ms of the audio clock. The title takes audio in bursts of 21 ms
            // blocks, so following the audio clock exactly made the video clock jump and frames come due in pairs.
            const auto since = static_cast<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - d->audioAt).count());
            const std::int64_t audio = static_cast<std::int64_t>(d->audioTs) + since;
            std::int64_t wall = audio;
            if (p.clockAnchor != std::chrono::steady_clock::time_point{})
                wall = static_cast<std::int64_t>(p.bankedMs) +
                       std::chrono::duration_cast<std::chrono::milliseconds>(now - p.clockAnchor).count();
            if (AvpSmoothClock() && p.clockAnchor != std::chrono::steady_clock::time_point{}) {
                // Rate-based slew: the clock runs at most 25% fast or slow toward audio, however often it is
                // polled. The anchor only moves by whole ms so frequent polls lose no time.
                const std::int64_t elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - p.clockAnchor).count();
                wall = static_cast<std::int64_t>(p.bankedMs) + elapsed;
                const std::int64_t drift = wall - audio;
                d->statDriftMs.store(drift, std::memory_order_relaxed);
                if (drift < -500 || drift > 500) {
                    wall = audio;
                    d->statSnaps.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const std::int64_t cap = elapsed / 4;
                    wall -= std::clamp<std::int64_t>(drift, -cap, cap);
                }
                p.bankedMs = static_cast<std::uint64_t>(std::max<std::int64_t>(wall, 0));
                p.clockAnchor += std::chrono::milliseconds(elapsed);
                return p.bankedMs;
            } else if (wall < audio - 80 || wall > audio + 80) {
                wall = audio;
                d->statSnaps.fetch_add(1, std::memory_order_relaxed);
            }
            p.bankedMs = static_cast<std::uint64_t>(std::max<std::int64_t>(wall, 0));
            p.clockAnchor = now;
            return p.bankedMs;
        }
    }
    if (p.clockAnchor == std::chrono::steady_clock::time_point{}) {
        if (!p.playing || p.paused) return p.bankedMs;
        p.clockAnchor = now;
    }
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - p.clockAnchor).count());
    return p.bankedMs + elapsed;
}

void HaltClock(Player& p) {
    p.bankedMs = MediaTime(p);
    p.clockAnchor = {};
}

std::uint64_t Duration(Player& p) {
    if (p.skipped) return kSkipFrames * kFrameIntervalMs;
    return p.decoder != nullptr ? p.decoder->durationMs : kStreamDurationMs;
}

// ---------------------------------------------------------------------------------------------------------------
// Decoder

int ReadPacket(void* opaque, std::uint8_t* buffer, int size) {
    auto* d = static_cast<Decoder*>(opaque);
    if (d->filePosition >= d->fileLength) return AVERROR_EOF;
    const std::uint64_t left = d->fileLength - d->filePosition;
    const auto want = static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(size), left));
    const std::int32_t got = d->fileRead(d->fileObject, buffer, d->filePosition, want);
    if (got <= 0) return want == 0 ? AVERROR_EOF : AVERROR(EIO);
    d->filePosition += static_cast<std::uint64_t>(got);
    return got;
}

std::int64_t SeekPacket(void* opaque, std::int64_t offset, int whence) {
    auto* d = static_cast<Decoder*>(opaque);
    if (whence & AVSEEK_SIZE) return static_cast<std::int64_t>(d->fileLength);
    std::int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<std::int64_t>(d->filePosition); break;
    case SEEK_END: base = static_cast<std::int64_t>(d->fileLength); break;
    default: return -1;
    }
    const std::int64_t target = std::clamp<std::int64_t>(base + offset, 0, static_cast<std::int64_t>(d->fileLength));
    d->filePosition = static_cast<std::uint64_t>(target);
    return target;
}

std::uint64_t ToMillis(std::int64_t ts, AVRational tb) {
    if (ts == AV_NOPTS_VALUE || ts <= 0 || tb.num <= 0 || tb.den <= 0) return 0;
    const auto ms = av_rescale_q(ts, tb, AVRational{1, 1000});
    return ms > 0 ? static_cast<std::uint64_t>(ms) : 0;
}

std::uint64_t FrameMillis(const AVFrame& f, AVRational tb) {
    std::int64_t ts = f.best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f.pts;
    if (ts == AV_NOPTS_VALUE) ts = f.pkt_dts;
    return ToMillis(ts, tb);
}

void FreeDecoderMedia(Decoder* d) {
    if (d->videoCodec != nullptr) avcodec_free_context(&d->videoCodec);
    if (d->audioCodec != nullptr) avcodec_free_context(&d->audioCodec);
    if (d->sws != nullptr) { sws_freeContext(d->sws); d->sws = nullptr; }
    if (d->swr != nullptr) swr_free(&d->swr);
    if (d->format != nullptr) avformat_close_input(&d->format);
    if (d->io != nullptr) {
        av_freep(&d->io->buffer);
        avio_context_free(&d->io);
    }
    if (d->fileOpened && d->fileClose != nullptr) d->fileClose(d->fileObject);
    d->fileOpened = false;
}

// Opens the source and lists its streams. Runs on the caller's (guest) thread. False: not decodable.
bool OpenDecoder(Player* p, Decoder* d, const char* path) {
    d->fileObject = p->fileObject;
    d->fileOpen = p->fileOpen;
    d->fileClose = p->fileClose;
    d->fileRead = p->fileRead;
    d->fileSize = p->fileSize;
    d->format = avformat_alloc_context();
    if (d->format == nullptr) return false;
    int res;
    if (d->fileOpen != nullptr && d->fileRead != nullptr && d->fileSize != nullptr) {
        if (d->fileOpen(d->fileObject, path) < 0) {
            std::fprintf(stderr, "[AVP] the title's file callback could not open '%s'\n", path);
            avformat_free_context(d->format);
            d->format = nullptr;
            return false;
        }
        d->fileOpened = true;
        d->fileLength = d->fileSize(d->fileObject);
        constexpr int kIoBuffer = 64 * 1024;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(kIoBuffer));
        d->io = avio_alloc_context(buffer, kIoBuffer, 0, d, &ReadPacket, nullptr, &SeekPacket);
        d->format->pb = d->io;
        d->format->flags |= AVFMT_FLAG_CUSTOM_IO;
        res = avformat_open_input(&d->format, nullptr, nullptr, nullptr);
    } else {
        const auto host = ResolvePath_nid_no_patch(path).string();
        res = avformat_open_input(&d->format, host.c_str(), nullptr, nullptr);
    }
    if (res < 0) {
        std::fprintf(stderr, "[AVP] could not open '%s' as a movie (%d)\n", path, res);
        return false; // avformat_open_input frees the context on failure
    }
    if (avformat_find_stream_info(d->format, nullptr) < 0) return false;
    for (unsigned i = 0; i < d->format->nb_streams; ++i) {
        const auto* par = d->format->streams[i]->codecpar;
        const bool video = par->codec_type == AVMEDIA_TYPE_VIDEO &&
                           (par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC);
        const bool audio = par->codec_type == AVMEDIA_TYPE_AUDIO && par->codec_id == AV_CODEC_ID_AAC;
        if (!video && !audio) continue;
        d->streams.push_back({static_cast<int>(i), video ? kStreamVideo : kStreamAudio});
        const auto* stream = d->format->streams[i];
        std::uint64_t ms = ToMillis(stream->duration, stream->time_base);
        if (ms == 0) ms = ToMillis(d->format->duration, AVRational{1, AV_TIME_BASE});
        d->durationMs = std::max(d->durationMs, ms);
        if (video) {
            d->width = static_cast<std::uint32_t>(par->width);
            d->height = static_cast<std::uint32_t>(par->height);
            d->alignedWidth = (d->width + 15u) & ~15u;
            d->alignedHeight = (d->height + 15u) & ~15u;
            d->pitch = (d->width + 63u) & ~63u;
            d->frameBytes = static_cast<std::size_t>(d->pitch) * d->alignedHeight * 3u / 2u;
            d->aspect = d->height != 0 ? static_cast<float>(d->width) / static_cast<float>(d->height) : 16.0f / 9.0f;
            if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
                d->frameRate = av_q2d(stream->avg_frame_rate);
        } else {
            d->channels = static_cast<std::uint32_t>(par->ch_layout.nb_channels);
            d->sampleRate = static_cast<std::uint32_t>(par->sample_rate);
        }
    }
    return !d->streams.empty();
}

AVCodecContext* OpenCodec(AVFormatContext* format, int index) {
    const auto* par = format->streams[index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (codec == nullptr) return nullptr;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr) return nullptr;
    ctx->thread_count = 0; // FFmpeg picks
    if (avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

// Waits for a free buffer of the kind; null once the player stops. With a frame hold, video buffers are taken
// oldest first (PRT-AVTEX).
std::uint8_t* TakeBuffer(Decoder* d, std::vector<std::uint8_t*>& pool, bool video) {
    std::unique_lock lk(d->mutex);
    d->cv.wait(lk, [&] { return d->stop || !pool.empty(); });
    if (d->stop) return nullptr;
    std::uint8_t* buffer;
    if (video && d->hold != 0) {
        buffer = pool.front();
        pool.erase(pool.begin());
    } else {
        buffer = pool.back();
        pool.pop_back();
    }
    if (video) {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& r : d->releasedAt) {
            if (r.first != buffer) continue;
            const auto since = now - r.second;
            if (since < std::chrono::milliseconds(20)) ++d->statRewrite20;
            if (since < std::chrono::milliseconds(50)) ++d->statRewrite50;
            break;
        }
    }
    return buffer;
}

void Release(Decoder* d, std::vector<std::uint8_t*>& pool, std::uint8_t* buffer) {
    std::lock_guard lk(d->mutex);
    pool.push_back(buffer);
    d->cv.notify_all();
}

// Writes a decoded picture as NV12 (luma, then interleaved chroma, both `pitch` wide) into guest memory.
bool StoreVideo(Decoder* d, const AVFrame& f, std::uint8_t* out) {
    std::uint8_t* planes[4] = {out, out + static_cast<std::size_t>(d->pitch) * d->alignedHeight, nullptr, nullptr};
    int strides[4] = {static_cast<int>(d->pitch), static_cast<int>(d->pitch), 0, 0};
    d->sws = sws_getCachedContext(d->sws, f.width, f.height, static_cast<AVPixelFormat>(f.format),
                                  static_cast<int>(d->width), static_cast<int>(d->height), AV_PIX_FMT_NV12,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (d->sws == nullptr) return false;
    if (sws_scale(d->sws, f.data, f.linesize, 0, f.height, planes, strides) < 0) return false;
    // Repeat the last rows into the alignment padding (it is reported as cropped, but a title may sample it).
    for (std::uint32_t y = d->height; y < d->alignedHeight; ++y)
        std::memcpy(out + static_cast<std::size_t>(y) * d->pitch,
                    out + static_cast<std::size_t>(d->height - 1) * d->pitch, d->pitch);
    std::uint8_t* chroma = planes[1];
    for (std::uint32_t y = d->height / 2; y < d->alignedHeight / 2; ++y)
        std::memcpy(chroma + static_cast<std::size_t>(y) * d->pitch,
                    chroma + static_cast<std::size_t>(d->height / 2 - 1) * d->pitch, d->pitch);
    return true;
}

// Converts decoded samples to interleaved signed 16-bit in guest memory. Returns the byte count.
std::uint32_t StoreAudio(Decoder* d, const AVFrame& f, std::uint8_t* out) {
    if (d->swr == nullptr) {
        AVChannelLayout layout = f.ch_layout;
        if (swr_alloc_set_opts2(&d->swr, &layout, AV_SAMPLE_FMT_S16, f.sample_rate, &layout,
                                static_cast<AVSampleFormat>(f.format), f.sample_rate, 0, nullptr) < 0 ||
            swr_init(d->swr) < 0) {
            swr_free(&d->swr);
            return 0;
        }
    }
    const int channels = f.ch_layout.nb_channels;
    const int capacity = static_cast<int>(kAudioBufferBytes / (static_cast<std::size_t>(channels) * 2u));
    std::uint8_t* planes[1] = {out};
    const int samples = swr_convert(d->swr, planes, capacity, const_cast<const std::uint8_t**>(f.extended_data),
                                    f.nb_samples);
    if (samples <= 0) return 0;
    return static_cast<std::uint32_t>(samples * channels * 2);
}

// Sends one packet (null: drain) and stores every frame it yields. False when the player is stopping.
bool DecodePacket(Decoder* d, AVCodecContext* codec, const AVPacket* packet, bool video, std::uint64_t offsetMs) {
    if (avcodec_send_packet(codec, packet) < 0 && packet != nullptr) return true; // skip a bad packet
    AVFrame* frame = av_frame_alloc();
    bool keepGoing = true;
    auto start = std::chrono::steady_clock::now();
    while (avcodec_receive_frame(codec, frame) >= 0) {
        if (video) d->statDecodeUs += MicrosSince(start);
        auto& pool = video ? d->freeVideo : d->freeAudio;
        const auto waitStart = std::chrono::steady_clock::now();
        std::uint8_t* buffer = TakeBuffer(d, pool, video);
        if (video) d->statWaitUs += MicrosSince(waitStart);
        if (buffer == nullptr) { keepGoing = false; break; }
        Frame out;
        out.data = buffer;
        const auto tb = d->format->streams[video ? d->videoIndex : d->audioIndex]->time_base;
        out.timestamp = FrameMillis(*frame, tb) + offsetMs;
        bool ok;
        if (video) {
            const auto storeStart = std::chrono::steady_clock::now();
            if (AvpInvalidate()) GpuInvalidate(buffer, d->frameBytes); // PRT-AVTEX
            d->avaVideoCores.fetch_or(CoreBit(), std::memory_order_relaxed);
            ok = StoreVideo(d, *frame, buffer);
            d->statStoreUs += MicrosSince(storeStart);
            ++d->statFrames;
            out.bytes = static_cast<std::uint32_t>(d->frameBytes);
        } else {
            d->avaAudioCores.fetch_or(CoreBit(), std::memory_order_relaxed);
            out.bytes = StoreAudio(d, *frame, buffer);
            ok = out.bytes != 0;
            // PRT-AVFADE: a linear rise over the first N ms of the stream's sound.
            if (const std::uint32_t fadeMs = AvpFadeInMs(); ok && fadeMs != 0 && d->channels > 0) {
                const std::uint64_t fadeSamples = static_cast<std::uint64_t>(d->sampleRate) * fadeMs / 1000u;
                if (d->fadeDone < fadeSamples) {
                    auto* pcm = reinterpret_cast<std::int16_t*>(buffer);
                    const std::uint64_t ch = d->channels;
                    const std::uint64_t perChannel = out.bytes / (2u * ch);
                    for (std::uint64_t i = 0; i < perChannel && d->fadeDone < fadeSamples; ++i, ++d->fadeDone) {
                        const float gain = static_cast<float>(d->fadeDone) / static_cast<float>(fadeSamples);
                        for (std::uint64_t c = 0; c < ch; ++c) {
                            auto& v = pcm[i * ch + c];
                            v = static_cast<std::int16_t>(static_cast<float>(v) * gain);
                        }
                    }
                }
            }
        }
        std::lock_guard lk(d->mutex);
        if (!ok) {
            pool.push_back(buffer);
            continue;
        }
        (video ? d->video : d->audio).push_back(out);
        if (video) ++d->framesDecoded;
        d->cv.notify_all();
        start = std::chrono::steady_clock::now();
    }
    av_frame_free(&frame);
    return keepGoing;
}

// One decoder per stream, on host threads (no guest calls), so a long run of one stream's packets in the file
// never holds up the other: the movies interleave sound and pictures in chunks of half a second and more, and a
// single decoder that stalled on sound buffers delivered pictures late (most of them were dropped).
void StreamDecoder(Decoder* d, bool video) {
    AVCodecContext* codec = video ? d->videoCodec : d->audioCodec;
    auto& queue = video ? d->videoPackets : d->audioPackets;
    std::uint32_t loop = 0;
    for (;;) {
        Decoder::Packet item{};
        {
            std::unique_lock lk(d->mutex);
            d->cv.wait(lk, [&] { return d->stop || !queue.empty() || d->readerDone; });
            if (d->stop) break;
            if (queue.empty()) break; // the reader stopped early: nothing more will come
            item = queue.front();
            queue.pop_front();
            d->cv.notify_all();
        }
        if (item.loop != loop) {
            avcodec_flush_buffers(codec);
            loop = item.loop;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(item.loop) * d->durationMs;
        const bool keepGoing = DecodePacket(d, codec, item.packet, video, offset);
        if (item.packet == nullptr) break; // drained: the end
        av_packet_free(&item.packet);
        if (!keepGoing) break;
    }
    std::lock_guard lk(d->mutex);
    (video ? d->videoDone : d->audioDone) = true;
    if (d->readerDone && (d->videoCodec == nullptr || d->videoDone) && (d->audioCodec == nullptr || d->audioDone))
        d->eof = true;
    d->cv.notify_all();
}

void* APS5_VABI ReaderThread(void* arg) {
    auto* d = static_cast<Decoder*>(arg);
    // Packets are small; this many per stream is several seconds of either.
    constexpr std::size_t kMaxQueued = 600;
    std::uint32_t loop = 0;
    for (;;) {
        {
            std::unique_lock lk(d->mutex);
            d->cv.wait(lk, [&] {
                return d->stop || (d->videoPackets.size() < kMaxQueued && d->audioPackets.size() < kMaxQueued);
            });
            if (d->stop) break;
        }
        AVPacket* packet = av_packet_alloc();
        const int res = av_read_frame(d->format, packet);
        if (res < 0) {
            av_packet_free(&packet);
            bool loopNow;
            {
                std::lock_guard lk(d->mutex);
                loopNow = d->looping && !d->stop && res == AVERROR_EOF && d->durationMs != 0;
            }
            if (loopNow) {
                av_seek_frame(d->format, -1, 0, AVSEEK_FLAG_BACKWARD);
                ++loop;
                continue;
            }
            // End of the file: ask each decoder to drain.
            std::lock_guard lk(d->mutex);
            if (d->videoCodec != nullptr) d->videoPackets.push_back({nullptr, loop});
            if (d->audioCodec != nullptr) d->audioPackets.push_back({nullptr, loop});
            d->cv.notify_all();
            break;
        }
        if (MicrosSince(d->statSince) >= 2000000) {
            const double frames = static_cast<double>(std::max<std::uint64_t>(d->statFrames, 1));
            std::fprintf(stderr, "[AVP] 2 s: %llu frames decoded (decode %.1f ms, convert %.1f ms each), "
                         "video decoder waited %.0f ms for buffers; title took %llu, %llu skipped late, "
                         "%llu polls found none due, audio drift %lld ms, %llu clock snaps\n",
                         static_cast<unsigned long long>(d->statFrames), d->statDecodeUs / frames / 1000.0,
                         d->statStoreUs / frames / 1000.0, d->statWaitUs / 1000.0,
                         static_cast<unsigned long long>(d->statDelivered.exchange(0)),
                         static_cast<unsigned long long>(d->statSkipped.exchange(0)),
                         static_cast<unsigned long long>(d->statMissed.exchange(0)),
                         static_cast<long long>(d->statDriftMs.load()),
                         static_cast<unsigned long long>(d->statSnaps.exchange(0)));
            // PRT-AVTEX: renderer uploads of the frame buffers (luma: uploads starting at a buffer, one per shown
            // frame expected; all: every texture over them) and write faults on them, against the takes above.
            {
                std::uint64_t luma = 0, all = 0, faults = 0;
                const bool have = GpuAvTexStats(luma, all, faults);
                std::size_t held, freeCount, queued;
                std::uint64_t r20, r50;
                {
                    std::lock_guard lk(d->mutex);
                    held = d->heldVideo.size();
                    freeCount = d->freeVideo.size();
                    queued = d->video.size();
                    r20 = d->statRewrite20;
                    r50 = d->statRewrite50;
                    d->statRewrite20 = d->statRewrite50 = 0;
                }
                if (have)
                    std::fprintf(stderr, "[AVP] PRT-AVTEX 2 s: renderer uploads %llu luma, %llu all, %llu write faults; "
                                 "hold %u: %zu held, %zu free, %zu queued; %llu buffers rewritten within 20 ms of "
                                 "being shown, %llu within 50 ms\n",
                                 static_cast<unsigned long long>(luma - d->texLuma),
                                 static_cast<unsigned long long>(all - d->texAll),
                                 static_cast<unsigned long long>(faults - d->texFaults), d->hold, held, freeCount,
                                 queued, static_cast<unsigned long long>(r20), static_cast<unsigned long long>(r50));
                else
                    std::fprintf(stderr, "[AVP] PRT-AVTEX 2 s: renderer counters n/a; hold %u: %zu held, %zu free, "
                                 "%zu queued; %llu buffers rewritten within 20 ms of being shown, %llu within 50 ms\n",
                                 d->hold, held, freeCount, queued, static_cast<unsigned long long>(r20),
                                 static_cast<unsigned long long>(r50));
                d->texLuma = luma;
                d->texAll = all;
                d->texFaults = faults;
            }
            d->statDecodeUs = d->statStoreUs = d->statWaitUs = d->statFrames = 0;
            d->statSince = std::chrono::steady_clock::now();
        }
        std::lock_guard lk(d->mutex);
        if (packet->stream_index == d->videoIndex && d->videoCodec != nullptr)
            d->videoPackets.push_back({packet, loop});
        else if (packet->stream_index == d->audioIndex && d->audioCodec != nullptr)
            d->audioPackets.push_back({packet, loop});
        else
            av_packet_free(&packet);
        d->cv.notify_all();
    }
    std::lock_guard lk(d->mutex);
    d->readerDone = true;
    if ((d->videoCodec == nullptr || d->videoDone) && (d->audioCodec == nullptr || d->audioDone)) d->eof = true;
    d->cv.notify_all();
    return nullptr;
}

// Opens the enabled streams' decoders, allocates the frame storage and starts the decoder thread.
bool StartDecoder(Player* p, Decoder* d) {
    // A title that starts without enabling any stream gets all of them.
    if (std::none_of(d->streams.begin(), d->streams.end(), [](const Decoder::Stream& s) { return s.enabled; }))
        for (auto& s : d->streams) s.enabled = true;
    for (const auto& s : d->streams) {
        if (!s.enabled) continue;
        if (s.type == kStreamVideo && d->videoCodec == nullptr) {
            d->videoIndex = s.index;
            d->videoCodec = OpenCodec(d->format, s.index);
        } else if (s.type == kStreamAudio && d->audioCodec == nullptr) {
            d->audioIndex = s.index;
            d->audioCodec = OpenCodec(d->format, s.index);
        }
    }
    if (d->videoCodec == nullptr && d->audioCodec == nullptr) return false;
    if (d->videoCodec != nullptr) {
        if (p->allocTexture == nullptr) return false;
        // At least 8 (decoding headroom at 60 fps), plus the frame the title holds.
        unsigned count = p->numFramebuffers > 0 ? static_cast<unsigned>(p->numFramebuffers) : 2u;
        count = std::clamp(count, 8u, 16u) + 1u;
        for (unsigned i = 0; i < count; ++i) {
            auto* buffer = static_cast<std::uint8_t*>(
                p->allocTexture(p->memObject, 0x100, static_cast<std::uint32_t>(d->frameBytes)));
            if (buffer == nullptr) break;
            d->allVideo.push_back(buffer);
            d->freeVideo.push_back(buffer);
        }
        if (d->allVideo.size() < 2) return false;
        // PRT-AVTEX: hold at most count - 3 (one shown, two to decode into).
        const std::uint32_t room = d->allVideo.size() > 3 ? static_cast<std::uint32_t>(d->allVideo.size() - 3) : 0u;
        d->hold = std::min(AvpHold(), room);
        std::uint64_t luma = 0, all = 0, faults = 0;
        if (GpuAvTexStats(luma, all, faults)) {
            d->texLuma = luma;
            d->texAll = all;
            d->texFaults = faults;
        }
        for (auto* buffer : d->allVideo) GpuAvTexRange(buffer, d->frameBytes, true);
    }
    if (d->audioCodec != nullptr && p->alloc != nullptr) {
        for (unsigned i = 0; i < 32; ++i) {
            auto* buffer = static_cast<std::uint8_t*>(
                p->alloc(p->memObject, 0x10, static_cast<std::uint32_t>(kAudioBufferBytes)));
            if (buffer == nullptr) break;
            d->allAudio.push_back(buffer);
            d->freeAudio.push_back(buffer);
        }
    }
    if (d->audioCodec != nullptr && d->allAudio.empty()) {
        avcodec_free_context(&d->audioCodec);
        d->audioIndex = -1;
    }
    d->looping = p->looping;
    if (d->videoCodec != nullptr) d->videoThread = std::thread(StreamDecoder, d, true);
    if (d->audioCodec != nullptr) d->audioThread = std::thread(StreamDecoder, d, false);
    if (scePthreadCreate(&d->thread, nullptr, ReaderThread, d, "AvPlayerReader") != 0) {
        d->thread = nullptr;
        {
            std::lock_guard lk(d->mutex);
            d->stop = true;
            d->cv.notify_all();
        }
        if (d->videoThread.joinable()) d->videoThread.join();
        if (d->audioThread.joinable()) d->audioThread.join();
        return false;
    }
    std::fprintf(stderr, "[AVP] decoding '%s': %ux%u video%s, %u Hz x%u audio%s, %llu ms, %zu frame buffers, hold %u\n",
                 p->path.c_str(), d->width, d->height, d->videoCodec ? "" : " (off)", d->sampleRate, d->channels,
                 d->audioCodec ? "" : " (off)", static_cast<unsigned long long>(d->durationMs), d->allVideo.size(),
                 d->hold);
    std::fflush(stderr);
    return true;
}

// Stops the decoder thread and gives every buffer back to the title. Called without the player lock.
void DestroyDecoder(Player* p, Decoder* d) {
    if (d == nullptr) return;
    {
        std::lock_guard lk(d->mutex);
        d->stop = true;
        d->cv.notify_all();
    }
    if (d->thread != nullptr) scePthreadJoin(d->thread, nullptr);
    if (d->videoThread.joinable()) d->videoThread.join();
    if (d->audioThread.joinable()) d->audioThread.join();
    for (auto& item : d->videoPackets) av_packet_free(&item.packet);
    for (auto& item : d->audioPackets) av_packet_free(&item.packet);
    if (d->avaPulls != 0) LogAudioStart(*d, "stopped before 5 s");
    for (auto* buffer : d->allVideo) GpuAvTexRange(buffer, d->frameBytes, false); // PRT-AVTEX
    FreeDecoderMedia(d);
    if (p->deallocTexture != nullptr)
        for (auto* buffer : d->allVideo) p->deallocTexture(p->memObject, buffer);
    if (p->dealloc != nullptr)
        for (auto* buffer : d->allAudio) p->dealloc(p->memObject, buffer);
    delete d;
}

// True once the decoder has finished and every frame has been handed out.
bool DecoderDrained(Decoder* d) {
    std::lock_guard lk(d->mutex);
    return (d->eof || d->failed) && d->video.empty() && d->audio.empty();
}

// ---------------------------------------------------------------------------------------------------------------
// Blank fallback clip

std::uint64_t CurrentFrame(Player& p) {
    const auto frame = MediaTime(p) / kFrameIntervalMs;
    return frame > kFrameBudget ? kFrameBudget : frame;
}

bool StreamExhausted(Player& p) {
    // A skipped movie shows a few black frames first: a stream that ended as it started left the title waiting.
    if (p.skipped) return p.noStorage || (p.framesDelivered > 0 && CurrentFrame(p) >= kSkipFrames);
    if (p.decoder != nullptr) return DecoderDrained(p.decoder);
    return p.noStorage || CurrentFrame(p) >= kFrameBudget;
}

// Returns true when this call ends the stream, so the caller fires STOP outside the lock.
bool EndStream(Player* p) {
    std::lock_guard g(p->lock);
    if (!p->playing || p->paused || p->stopFired) return false;
    if (!StreamExhausted(*p)) return false;
    std::fprintf(stderr, "[AVP] end of stream: clock at %llu ms\n", static_cast<unsigned long long>(MediaTime(*p)));
    std::fflush(stderr);
    HaltClock(*p);
    p->playing = false;
    p->stopFired = true;
    return true;
}

bool PrepareFrames(Player& p) {
    if (!p.frames.empty() || p.noStorage) return !p.frames.empty();
    if (p.allocTexture == nullptr || p.memObject == nullptr) {
        p.noStorage = true;
        return false;
    }
    unsigned requested = p.numFramebuffers > 0 ? static_cast<unsigned>(p.numFramebuffers) : 2u;
    requested = std::clamp(requested, 2u, 8u);
    const auto planeBytes = static_cast<std::size_t>(kFramePitch) * kFrameHeight;
    for (unsigned i = 0; i < requested; ++i) {
        auto* frame = static_cast<std::uint8_t*>(p.allocTexture(p.memObject, 0x100, static_cast<std::uint32_t>(kFrameBytes)));
        if (frame == nullptr) break;
        std::memset(frame, kBlackLuma, planeBytes);
        std::memset(frame + planeBytes, kBlackChroma, kFrameBytes - planeBytes);
        p.frames.push_back(frame);
    }
    if (p.frames.empty()) {
        p.noStorage = true;
        return false;
    }
    return true;
}

bool DeliverBlankFrame(Player& p, void*& data, std::uint64_t& timestamp) {
    if (!p.playing || p.paused || !PrepareFrames(p)) return false;
    const auto frame = CurrentFrame(p);
    if (frame >= (p.skipped ? kSkipFrames : kFrameBudget)) return false;
    data = p.frames[frame % p.frames.size()];
    timestamp = frame * kFrameIntervalMs;
    p.framesDelivered = std::max(p.framesDelivered, frame + 1);
    return true;
}

void WriteBlankDetails(VideoDetailsEx& out) {
    out = {};
    out.width = kFramePitch;
    out.height = kFrameHeight;
    out.aspectRatio = static_cast<float>(kFrameWidth) / static_cast<float>(kFrameHeight);
    out.cropRightOffset = kFramePitch - kFrameWidth;
    out.pitch = kFramePitch;
    out.lumaBitDepth = 8;
    out.chromaBitDepth = 8;
    out.frameRate = kFrameRate;
}

void WriteDecoderDetails(const Decoder& d, VideoDetailsEx& out) {
    out = {};
    out.width = d.alignedWidth;
    out.height = d.alignedHeight;
    out.aspectRatio = d.aspect;
    out.cropRightOffset = d.pitch - d.width;
    out.cropBottomOffset = d.alignedHeight - d.height;
    out.pitch = d.pitch;
    out.lumaBitDepth = 8;
    out.chromaBitDepth = 8;
    out.frameRate = d.frameRate;
}

// Takes the next due video frame of the decoder; the previous one goes back to the pool. Caller holds p->lock.
bool DeliverDecodedFrame(Player& p, void*& data, std::uint64_t& timestamp) {
    if (!p.playing || p.paused) return false;
    Decoder* d = p.decoder;
    const std::uint64_t now = MediaTime(p);
    std::lock_guard lk(d->mutex);
    if (d->video.empty()) {
        ++d->statMissed;
        return false;
    }
    Frame next = d->video.front();
    if (next.timestamp > now) return false;
    // Each poll shows the oldest due frame; frames more than 50 ms late are dropped to catch up. (Always taking
    // the newest due frame dropped one whenever a poll came a little late.)
    d->video.pop_front();
    const std::uint64_t lateMs = AvpSmoothClock() ? 100 : 50;
    while (!d->video.empty() && d->video.front().timestamp <= now && next.timestamp + lateMs < now) {
        d->freeVideo.push_back(next.data);
        ++d->statSkipped;
        next = d->video.front();
        d->video.pop_front();
    }
    if (d->haveCurrentVideo) ReleaseShown(*d, d->currentVideo.data); // PRT-AVTEX hold
    d->currentVideo = next;
    d->haveCurrentVideo = true;
    ++d->statDelivered;
    d->cv.notify_all();
    data = next.data;
    timestamp = next.timestamp;
    ++p.framesDelivered;
    return true;
}

Bool PollVideo(Player* p, void* frame_info, bool extended) {
    if (frame_info == nullptr) return 0;
    {
        std::lock_guard g(p->lock);
        void* data = nullptr;
        std::uint64_t timestamp = 0;
        const bool delivered = p->decoder != nullptr ? DeliverDecodedFrame(*p, data, timestamp)
                                                     : DeliverBlankFrame(*p, data, timestamp);
        if (delivered) {
            VideoDetailsEx details;
            if (p->decoder != nullptr) WriteDecoderDetails(*p->decoder, details);
            else WriteBlankDetails(details);
            if (extended) {
                FrameInfoEx out = {};
                out.data = data;
                out.timestamp = timestamp;
                out.video = details;
                std::memcpy(frame_info, &out, sizeof(out));
            } else {
                FrameInfo out = {};
                out.data = data;
                out.timestamp = timestamp;
                std::memcpy(out.details + 0, &details.width, 4);
                std::memcpy(out.details + 4, &details.height, 4);
                std::memcpy(out.details + 8, &details.aspectRatio, 4);
                std::memcpy(frame_info, &out, sizeof(out));
            }
            return 1;
        }
    }
    if (EndStream(p)) FireAsync(p, kEventStop);
    return 0;
}

void LogLifecycleCall(const char* site, Player* p) {
    std::ostringstream tid;
    tid << std::this_thread::get_id();
    std::lock_guard g(p->lock);
    std::fprintf(stderr, "[AVPLIFE] %s p=%p tid=%s playing=%d paused=%d stopFired=%d haveSource=%d decoding=%d\n",
        site, static_cast<void*>(p), tid.str().c_str(), p->playing ? 1 : 0, p->paused ? 1 : 0,
        p->stopFired ? 1 : 0, p->haveSource ? 1 : 0, p->decoder != nullptr ? 1 : 0);
    std::fflush(stderr);
}

Player* CreateFrom(const Replacements* r, std::int32_t numFramebuffers, bool autoStart) {
    Player* p = new Player();
    if (r != nullptr) {
        p->memObject = r->memObject;
        p->alloc = reinterpret_cast<MemAlloc>(r->alloc);
        p->dealloc = reinterpret_cast<MemFree>(r->dealloc);
        p->allocTexture = reinterpret_cast<MemAlloc>(r->allocTexture);
        p->deallocTexture = reinterpret_cast<MemFree>(r->deallocTexture);
        p->fileObject = r->fileObject;
        p->fileOpen = reinterpret_cast<FileOpen>(r->fileOpen);
        p->fileClose = reinterpret_cast<FileClose>(r->fileClose);
        p->fileRead = reinterpret_cast<FileReadOffset>(r->fileRead);
        p->fileSize = reinterpret_cast<FileSize>(r->fileSize);
        p->eventObject = r->eventObject;
        p->callback = reinterpret_cast<EventCallback>(r->eventCallback);
    }
    p->numFramebuffers = numFramebuffers;
    p->autoStart = autoStart;
    return p;
}

bool DecodingEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_MOVIES");
        return !(env && env[0] == '0');
    }();
    return on;
}

// The launcher's Display option "Play the opening movie" (General.opening_movie in
// user\custom_configs\CUSA03220.json next to the game's exe; on unless set to false).
bool OpeningMovieEnabled() {
    static const bool on = [] {
        std::string dir = ".";
#ifdef _WIN32
        char exe[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            dir.assign(exe, n);
            const auto slash = dir.find_last_of("\\/");
            dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
        }
#endif
        std::ifstream file(dir + "/user/custom_configs/CUSA03220.json", std::ios::binary);
        if (!file) return true;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto at = text.find("\"opening_movie\"");
        if (at == std::string::npos) return true;
        at = text.find(':', at);
        if (at == std::string::npos) return true;
        at = text.find_first_not_of(" \t\r\n", at + 1);
        return !(at != std::string::npos && text.compare(at, 5, "false") == 0);
    }();
    return on;
}

// GT Sport's opening movie (the long film the boot script plays before the menus).
bool IsOpeningMovie(const char* path) {
    const std::string name(path);
    const auto slash = name.find_last_of("\\/");
    const std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
    return file == "m049.mp4";
}

// Starts playback: the decoder thread when decoding, the clock either way. Caller holds no lock.
void BeginPlayback(Player* p) {
    Decoder* d = nullptr;
    {
        std::lock_guard g(p->lock);
        d = p->decoder;
    }
    if (d == nullptr || d->thread != nullptr) return;
    if (!StartDecoder(p, d)) {
        std::fprintf(stderr, "[AVP] could not start decoding '%s'; playing a blank clip\n", p->path.c_str());
        DestroyDecoder(p, d);
        std::lock_guard g(p->lock);
        p->decoder = nullptr;
        return;
    }
    // PRT-AVAUDIO prefill: decoded audio waits for the title from its first pull, and the clock starts after it.
    if (const std::uint32_t prefill = AvpPrefillMs(); prefill != 0) {
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t audioMs = 0;
        std::size_t pictures = 0;
        bool ready;
        {
            std::unique_lock lk(d->mutex);
            ready = d->cv.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return d->stop || d->eof || d->failed ||
                       ((d->audioCodec == nullptr || QueuedAudioMs(*d) >= prefill) &&
                        (d->videoCodec == nullptr || !d->video.empty()));
            });
            audioMs = QueuedAudioMs(*d);
            pictures = d->video.size();
        }
        {
            std::lock_guard g(p->lock);
            p->bankedMs = 0;
            p->clockAnchor = {};
        }
        std::fprintf(stderr, "[AVP] PRT-AVAUDIO: prefill %s in %lld ms: %llu ms of audio, %zu pictures\n",
                     ready ? "done" : "timed out",
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - t0).count()),
                     static_cast<unsigned long long>(audioMs), pictures);
    }
    std::lock_guard lk(d->mutex);
    d->avaStart = std::chrono::steady_clock::now();
}

int AddSourceCommon(Player* p, const char* path) {
    if (p == nullptr || path == nullptr || *path == '\0') return kErrInvalidParams;
    LogLifecycleCall("AddSource(before)", p);
    Decoder* old = nullptr;
    {
        std::lock_guard g(p->lock);
        old = p->decoder;
        p->decoder = nullptr;
    }
    DestroyDecoder(p, old);
    Decoder* d = nullptr;
    const bool skip = IsOpeningMovie(path) && !OpeningMovieEnabled();
    if (DecodingEnabled() && !skip) {
        d = new Decoder();
        if (!OpenDecoder(p, d, path)) {
            FreeDecoderMedia(d);
            delete d;
            d = nullptr;
        }
    }
    bool play = false;
    {
        std::lock_guard g(p->lock);
        p->path = path;
        p->decoder = d;
        p->skipped = skip;
        p->haveSource = true;
        p->playing = p->autoStart;
        p->paused = false;
        p->stopFired = false;
        p->bankedMs = 0;
        p->clockAnchor = {};
        p->framesDelivered = 0;
        play = p->autoStart;
        if (d != nullptr && play)
            for (auto& s : d->streams) s.enabled = true;
    }
    std::fprintf(stderr, "[AVP] AddSource '%s': %s\n", path,
                 skip ? "opening movie turned off, skipped"
                      : d != nullptr ? "decoding with FFmpeg" : "not decodable, blank clip");
    std::fflush(stderr);
    Fire(p, kEventReady);
    if (play) {
        BeginPlayback(p);
        Fire(p, kEventPlay);
    }
    return 0;
}
}  // namespace

// RYGT_AVP_TRACE=1: every call (polls are counted and summed up once a second).
static void Trace(const char* name) {
    static const bool on = [] {
        const char* env = std::getenv("RYGT_AVP_TRACE");
        return env && env[0] == '1';
    }();
    if (!on) return;
    static std::mutex m;
    static std::vector<std::pair<std::string, unsigned>> counts;
    static auto last = std::chrono::steady_clock::now();
    std::lock_guard g(m);
    const std::string n{name};
    const bool poll = n.find("GetVideoData") != std::string::npos || n.find("GetAudioData") != std::string::npos ||
                      n == "sceAvPlayerIsActive" || n == "sceAvPlayerCurrentTime";
    if (!poll) {
        std::fprintf(stderr, "[AVPTRACE] %s\n", name);
    } else {
        bool found = false;
        for (auto& [k, c] : counts) if (k == n) { ++c; found = true; }
        if (!found) counts.emplace_back(n, 1);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1) && !counts.empty()) {
        last = now;
        std::string line;
        for (auto& [k, c] : counts) { line += " " + k + "=" + std::to_string(c); c = 0; }
        std::fprintf(stderr, "[AVPTRACE] polls in 1 s:%s\n", line.c_str());
    }
    std::fflush(stderr);
}

extern "C" {

// sceAvPlayerInitData: the replacement blocks, then debug level, priority, framebuffers, auto start.
void* APS5_VABI sceAvPlayerInit(const void* init) {
 Trace("sceAvPlayerInit");
 if (init == nullptr) return CreateFrom(nullptr, 0, false);
 const auto* base = static_cast<const std::uint8_t*>(init);
 std::int32_t framebuffers = 0;
 std::uint8_t autoStart = 0;
 std::memcpy(&framebuffers, base + 96 + 8, 4);
 std::memcpy(&autoStart, base + 96 + 12, 1);
 return CreateFrom(reinterpret_cast<const Replacements*>(base), framebuffers, autoStart != 0);
}
// sceAvPlayerInitDataEx: size, the replacement blocks, default language, debug level, twelve priorities and
// affinities, then framebuffers (164) and auto start (168).
int APS5_VABI sceAvPlayerInitEx(const void* init_ex, void** handle) {
 Trace("sceAvPlayerInitEx");
 if (handle == nullptr) return kErrInvalidParams;
 const auto* base = static_cast<const std::uint8_t*>(init_ex);
 if (base == nullptr) {
  *handle = CreateFrom(nullptr, 0, false);
  return 0;
 }
 std::int32_t framebuffers = 0;
 std::uint8_t autoStart = 0;
 std::memcpy(&framebuffers, base + 164, 4);
 std::memcpy(&autoStart, base + 168, 1);
 *handle = CreateFrom(reinterpret_cast<const Replacements*>(base + 8), framebuffers, autoStart != 0);
 return 0;
}
int APS5_VABI sceAvPlayerPostInit(void* h, const void* post_init) {
 Trace("sceAvPlayerPostInit");
 (void)post_init;
 return Check(h) != nullptr ? 0 : kErrInvalidParams;
}
int APS5_VABI sceAvPlayerSetLogCallback(void* callback, void* user_data) {
 Trace("sceAvPlayerSetLogCallback");
 (void)callback;
 (void)user_data;
 return 0;
}
int APS5_VABI sceAvPlayerAddSource(void* h, const char* filename) {
 Trace("sceAvPlayerAddSource");
 return AddSourceCommon(Check(h), filename);
}
int APS5_VABI sceAvPlayerAddSourceEx(void* h, std::uint32_t uri_type, const void* source_details) {
 Trace("sceAvPlayerAddSourceEx");
 (void)uri_type;
 // SceAvPlayerSourceDetails begins with { const char* name; uint32_t length; }.
 const char* name = source_details != nullptr ? *static_cast<const char* const*>(source_details) : nullptr;
 return AddSourceCommon(Check(h), name);
}
int APS5_VABI sceAvPlayerStart(void* h) {
 Trace("sceAvPlayerStart");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 LogLifecycleCall("Start(before)", p);
 bool play = false;
 {
  std::lock_guard g(p->lock);
  if (p->haveSource && !p->playing && !p->stopFired) { p->playing = true; p->paused = false; play = true; }
 }
 if (play) {
  BeginPlayback(p);
  Fire(p, kEventPlay);
 }
 return 0;
}
int APS5_VABI sceAvPlayerStartEx(void* h, const void* start_info_ex) {
 Trace("sceAvPlayerStartEx");
 (void)start_info_ex;
 return sceAvPlayerStart(h);
}
int APS5_VABI sceAvPlayerPause(void* h) {
 Trace("sceAvPlayerPause");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 LogLifecycleCall("Pause(before)", p);
 bool notify = false;
 {
  std::lock_guard g(p->lock);
  if (p->playing && !p->paused) { p->paused = true; HaltClock(*p); notify = true; }
 }
 if (notify) Fire(p, kEventPause);
 return 0;
}
int APS5_VABI sceAvPlayerResume(void* h) {
 Trace("sceAvPlayerResume");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 LogLifecycleCall("Resume(before)", p);
 bool notify = false;
 {
  std::lock_guard g(p->lock);
  if (p->playing && p->paused) { p->paused = false; notify = true; }
 }
 if (notify) Fire(p, kEventPlay);
 return 0;
}
int APS5_VABI sceAvPlayerStop(void* h) {
 Trace("sceAvPlayerStop");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 LogLifecycleCall("Stop(before)", p);
 bool notify = false;
 Decoder* d = nullptr;
 {
  std::lock_guard g(p->lock);
  if (p->playing && !p->stopFired) { p->playing = false; HaltClock(*p); p->stopFired = true; notify = true; }
  p->haveSource = false;
  d = p->decoder;
  p->decoder = nullptr;
 }
 DestroyDecoder(p, d);
 if (notify) Fire(p, kEventStop);
 return 0;
}
int APS5_VABI sceAvPlayerClose(void* h) {
 Trace("sceAvPlayerClose");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 std::vector<std::uint8_t*> frames;
 Decoder* d = nullptr;
 {
  std::lock_guard g(p->lock);
  p->magic = 0;
  frames.swap(p->frames);
  d = p->decoder;
  p->decoder = nullptr;
 }
 DestroyDecoder(p, d);
 if (p->deallocTexture != nullptr)
  for (auto* frame : frames) p->deallocTexture(p->memObject, frame);
 delete p;
 return 0;
}
Bool APS5_VABI sceAvPlayerIsActive(void* h) {
 Trace("sceAvPlayerIsActive");
 Player* p = Check(h);
 if (p == nullptr) return 0;
 if (EndStream(p)) FireAsync(p, kEventStop);
 std::lock_guard g(p->lock);
 return p->playing ? 1 : 0;
}
std::uint64_t APS5_VABI sceAvPlayerCurrentTime(void* h) {
 Trace("sceAvPlayerCurrentTime");
 Player* p = Check(h);
 if (p == nullptr) return 0;
 std::lock_guard g(p->lock);
 const auto time_ms = MediaTime(*p);
 const auto duration = Duration(*p);
 return duration != 0 && time_ms > duration ? duration : time_ms;
}
int APS5_VABI sceAvPlayerJumpToTime(void* h, std::uint64_t time_ms) {
 Trace("sceAvPlayerJumpToTime");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 LogLifecycleCall("JumpToTime(before)", p);
 {
  std::lock_guard g(p->lock);
  const auto duration = Duration(*p);
  p->bankedMs = duration != 0 && time_ms > duration ? duration : time_ms;
  p->clockAnchor = {};
 }
 std::uint32_t warning = kWarningJumpComplete;
 Fire(p, kEventWarning, &warning);
 return 0;
}
int APS5_VABI sceAvPlayerSetLooping(void* h, Bool loop) {
 Trace("sceAvPlayerSetLooping");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 std::lock_guard g(p->lock);
 p->looping = loop != 0;
 if (p->decoder != nullptr) {
  std::lock_guard lk(p->decoder->mutex);
  p->decoder->looping = p->looping;
 }
 return 0;
}
int APS5_VABI sceAvPlayerSetAvSyncMode(void* h, std::uint32_t sync_mode) {
 Trace("sceAvPlayerSetAvSyncMode");
 (void)sync_mode;
 return Check(h) != nullptr ? 0 : kErrInvalidParams;
}
int APS5_VABI sceAvPlayerSetTrickSpeed(void* h, std::int32_t trick_speed) {
 Trace("sceAvPlayerSetTrickSpeed");
 (void)trick_speed;
 return Check(h) != nullptr ? 0 : kErrInvalidParams;
}
int APS5_VABI sceAvPlayerSetAvailableBandwidth(void* h, std::uint32_t start_bandwidth, std::uint32_t minimum_bandwidth, std::uint32_t maximum_bandwidth) {
 Trace("sceAvPlayerSetAvailableBandwidth");
 (void)start_bandwidth;
 (void)minimum_bandwidth;
 (void)maximum_bandwidth;
 return Check(h) != nullptr ? 0 : kErrInvalidParams;
}
int APS5_VABI sceAvPlayerStreamCount(void* h) {
 Trace("sceAvPlayerStreamCount");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 std::lock_guard g(p->lock);
 if (!p->haveSource) return 0;
 return p->decoder != nullptr ? static_cast<int>(p->decoder->streams.size()) : 1;
}
int APS5_VABI sceAvPlayerEnableStream(void* h, std::uint32_t stream_id) {
 Trace("sceAvPlayerEnableStream");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 std::lock_guard g(p->lock);
 if (p->decoder != nullptr) {
  if (stream_id >= p->decoder->streams.size()) return kErrInvalidParams;
  p->decoder->streams[stream_id].enabled = true;
 }
 return 0;
}
int APS5_VABI sceAvPlayerDisableStream(void* h, std::uint32_t stream_id) {
 Trace("sceAvPlayerDisableStream");
 Player* p = Check(h);
 if (p == nullptr) return kErrInvalidParams;
 std::lock_guard g(p->lock);
 if (p->decoder != nullptr && stream_id < p->decoder->streams.size())
  p->decoder->streams[stream_id].enabled = false;
 return 0;
}
int APS5_VABI sceAvPlayerChangeStream(void* h, std::uint32_t old_stream_id, std::uint32_t new_stream_id) {
 Trace("sceAvPlayerChangeStream");
 (void)old_stream_id;
 (void)new_stream_id;
 return Check(h) != nullptr ? 0 : kErrInvalidParams;
}
// SceAvPlayerStreamInfo (40 bytes): u32 type (0 video, 1 audio), u8 reserved[4], details[16], u64 duration (ms),
// u64 start time (ms).
int APS5_VABI sceAvPlayerGetStreamInfo(void* h, std::uint32_t stream_id, void* info) {
 Trace("sceAvPlayerGetStreamInfo");
 Player* p = Check(h);
 if (p == nullptr || info == nullptr) return kErrInvalidParams;
 std::lock_guard g(p->lock);
 if (!p->haveSource) return kErrOperationFailed;
 std::uint8_t* out = static_cast<std::uint8_t*>(info);
 std::uint32_t type = kStreamVideo;
 if (p->decoder != nullptr) {
  if (stream_id >= p->decoder->streams.size()) return kErrOperationFailed;
  type = p->decoder->streams[stream_id].type;
 } else if (stream_id != 0) {
  return kErrOperationFailed;
 }
 std::memset(out, 0, 40);
 const std::uint64_t duration = Duration(*p);
 std::memcpy(out + 0, &type, 4);
 if (type == kStreamVideo) {
  const std::uint32_t width = p->decoder != nullptr ? p->decoder->alignedWidth : kFrameWidth;
  const std::uint32_t height = p->decoder != nullptr ? p->decoder->alignedHeight : kFrameHeight;
  const float aspect = p->decoder != nullptr ? p->decoder->aspect
                                             : static_cast<float>(kFrameWidth) / static_cast<float>(kFrameHeight);
  std::memcpy(out + 8, &width, 4);
  std::memcpy(out + 12, &height, 4);
  std::memcpy(out + 16, &aspect, 4);
 } else {
  AudioDetails audio{};
  audio.channelCount = static_cast<std::uint16_t>(p->decoder->channels);
  audio.sampleRate = p->decoder->sampleRate;
  audio.size = 0;
  std::memcpy(out + 8, &audio, sizeof(audio));
 }
 std::memcpy(out + 24, &duration, 8);
 return 0;
}
// SceAvPlayerStreamInfoEx (104 bytes): u64 size, u32 type, reserved[4], details[80], u64 duration.
int APS5_VABI sceAvPlayerGetStreamInfoEx(void* h, std::uint32_t stream_id, void* info) {
 Trace("sceAvPlayerGetStreamInfoEx");
 Player* p = Check(h);
 if (p == nullptr || info == nullptr) return kErrInvalidParams;
 std::lock_guard g(p->lock);
 if (!p->haveSource) return kErrOperationFailed;
 std::uint32_t type = kStreamVideo;
 if (p->decoder != nullptr) {
  if (stream_id >= p->decoder->streams.size()) return kErrOperationFailed;
  type = p->decoder->streams[stream_id].type;
 } else if (stream_id != 0) {
  return kErrOperationFailed;
 }
 std::uint8_t* out = static_cast<std::uint8_t*>(info);
 std::uint64_t size = 0;
 std::memcpy(&size, out, 8);
 std::memset(out, 0, 104);
 std::memcpy(out, &size, 8);
 const std::uint64_t duration = Duration(*p);
 std::memcpy(out + 8, &type, 4);
 if (type == kStreamVideo) {
  VideoDetailsEx details;
  if (p->decoder != nullptr) WriteDecoderDetails(*p->decoder, details);
  else WriteBlankDetails(details);
  std::memcpy(out + 16, &details, sizeof(details));
 } else {
  AudioDetails audio{};
  audio.channelCount = static_cast<std::uint16_t>(p->decoder->channels);
  audio.sampleRate = p->decoder->sampleRate;
  std::memcpy(out + 16, &audio, sizeof(audio));
 }
 std::memcpy(out + 96, &duration, 8);
 return 0;
}
Bool APS5_VABI sceAvPlayerGetVideoData(void* h, void* video_info) {
 Trace("sceAvPlayerGetVideoData");
 Player* p = Check(h);
 if (p == nullptr) return 0;
 return PollVideo(p, video_info, false);
}
Bool APS5_VABI sceAvPlayerGetVideoDataEx(void* h, void* video_info) {
 Trace("sceAvPlayerGetVideoDataEx");
 Player* p = Check(h);
 if (p == nullptr) return 0;
 return PollVideo(p, video_info, true);
}
// Audio blocks are handed out as soon as they are decoded (the title's audio output paces them); the previous
// block goes back to the pool.
Bool APS5_VABI sceAvPlayerGetAudioData(void* h, void* audio_info) {
 Trace("sceAvPlayerGetAudioData");
 Player* p = Check(h);
 if (p == nullptr) return 0;
 {
  std::lock_guard g(p->lock);
  Decoder* d = p->decoder;
  if (d != nullptr && p->playing && !p->paused && audio_info != nullptr) {
   std::lock_guard lk(d->mutex);
   if (d->audio.empty()) NoteAudioPull(*d, false); // PRT-AVAUDIO
   if (!d->audio.empty()) {
    if (d->haveCurrentAudio) d->freeAudio.push_back(d->currentAudio.data);
    d->currentAudio = d->audio.front();
    d->audio.pop_front();
    d->haveCurrentAudio = true;
    d->audioClock = true;
    d->audioTs = d->currentAudio.timestamp;
    d->audioAt = std::chrono::steady_clock::now();
    NoteAudioPull(*d, true); // PRT-AVAUDIO
    d->cv.notify_all();
    FrameInfo out = {};
    out.data = d->currentAudio.data;
    out.timestamp = d->currentAudio.timestamp;
    AudioDetails audio{};
    audio.channelCount = static_cast<std::uint16_t>(d->channels);
    audio.sampleRate = d->sampleRate;
    audio.size = d->currentAudio.bytes;
    std::memcpy(out.details, &audio, sizeof(audio));
    std::memcpy(audio_info, &out, sizeof(out));
    return 1;
   }
  }
 }
 if (EndStream(p)) FireAsync(p, kEventStop);
 return 0;
}
}
