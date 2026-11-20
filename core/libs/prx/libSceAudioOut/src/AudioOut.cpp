#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <prx/libc/include/General.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <time.h>
#endif

#include "SDL.h"
#include "SceTypes.hpp"
#include "prx/libkernel/Time/include/Time.hpp"

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int PORT_TYPE_MAIN = 0;
constexpr int PORT_TYPE_BGM = 1;
constexpr int PORT_TYPE_VOICE = 2;
constexpr int PORT_TYPE_PERSONAL = 3;
constexpr int PORT_TYPE_PADSPK = 4;
constexpr int PORT_TYPE_VIBRATION = 10;
constexpr int PORT_TYPE_AUDIO3D = 126;
constexpr int PORT_TYPE_AUX = 127;

constexpr int PORTS_MAX = 32;
constexpr int DEFAULT_VOLUME = 32768;
constexpr std::uint32_t FORMAT_MASK = 0xFFu;

// Unified Master Mixer format (10.0 ms WASAPI shared-mode clock @ 48 kHz stereo float):
constexpr std::uint32_t MASTER_FREQ = 48000u;
constexpr std::uint32_t MASTER_CHANNELS = 2u;
constexpr std::uint32_t MASTER_PERIOD_FRAMES = 480u;
constexpr std::uint32_t RING_FRAMES = 8192u; // ~170.67 ms power-of-2 lock-free SPSC ring per port
constexpr std::uint32_t FADE_RAMP_FRAMES = 16u; // 0.33 ms click-free edge ramp

constexpr std::uint64_t DEFAULT_TARGET_LATENCY_US = 40000ULL;
constexpr std::uint64_t DEFAULT_SECONDARY_HEADROOM_MS = 80ULL;
constexpr std::uint64_t DRAIN_TIMEOUT_US = 200000ULL;

constexpr std::uint32_t STD_8CH_MAP[8] = {0, 1, 2, 3, 6, 7, 4, 5};

enum class Format {
    Unknown,
    S16Mono,
    S16Stereo,
    S16_8Ch,
    F32Mono,
    F32Stereo,
    F32_8Ch,
    S16_8ChStd,
    F32_8ChStd,
};

enum class PortSlotState : std::uint8_t {
    Free = 0,
    Opening = 1,
    Active = 2,
    Closing = 3,
};

struct RuntimeConfig {
    bool traceAudioOut = false;
    bool diagEnabled = false;
    bool padSpkEnabled = false;
    bool promoteThreadPrio = true;
    std::uint64_t targetLatencyUs = DEFAULT_TARGET_LATENCY_US;
    std::uint64_t secondaryHeadroomMs = DEFAULT_SECONDARY_HEADROOM_MS;
    std::string diagPath = "logs/audio_diag.log";
};

const RuntimeConfig& getConfig() {
    static const RuntimeConfig cfg = [] {
        RuntimeConfig c{};
        c.traceAudioOut = std::getenv("APS5_TRACE_AUDIOOUT") != nullptr;
        if (const char* diag = std::getenv("RYGT_AUDIO_DIAG")) {
            c.diagEnabled = (diag[0] != '\0' && diag[0] != '0');
        }
        if (const char* path = std::getenv("RYGT_AUDIO_DIAG_PATH")) {
            if (path[0] != '\0') c.diagPath = path;
        }
        if (const char* pad = std::getenv("RYGT_AUDIO_PADSPK")) {
            c.padSpkEnabled = (pad[0] != '\0' && pad[0] != '0');
        }
        if (const char* prio = std::getenv("RYGT_AUDIO_PROMOTE_PRIO")) {
            c.promoteThreadPrio = (prio[0] != '0');
        }
        if (const char* latMs = std::getenv("RYGT_AUDIO_TARGET_LATENCY_MS")) {
            const long long parsedMs = std::atoll(latMs);
            if (parsedMs >= 20 && parsedMs <= 120) {
                c.targetLatencyUs = static_cast<std::uint64_t>(parsedMs) * 1000ULL;
            }
        } else if (const char* latUs = std::getenv("RYGT_AUDIO_LATENCY_US")) {
            const long long parsed = std::atoll(latUs);
            if (parsed >= 20000 && parsed <= 120000) {
                c.targetLatencyUs = static_cast<std::uint64_t>(parsed);
            }
        }
        if (const char* secMs = std::getenv("RYGT_AUDIO_SECONDARY_HEADROOM_MS")) {
            const long long parsed = std::atoll(secMs);
            if (parsed >= 40 && parsed <= 140) {
                c.secondaryHeadroomMs = static_cast<std::uint64_t>(parsed);
            }
        }
        return c;
    }();
    return cfg;
}

std::uint64_t hostMicros() {
    static const auto t0 = SteadyClock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0).count());
}

void preciseSleepUs(std::uint64_t microseconds) {
    if (microseconds == 0) return;
#ifdef _WIN32
    struct ThreadTimer {
        HANDLE handle = nullptr;
        bool highRes = false;
        ThreadTimer() {
            handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
            if (handle) {
                highRes = true;
            } else {
                handle = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            }
        }
        ~ThreadTimer() {
            if (handle) CloseHandle(handle);
        }
    };
    thread_local ThreadTimer timer;
    if (timer.handle) {
        if (microseconds >= 250 || timer.highRes) {
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -static_cast<LONGLONG>(microseconds * 10ULL);
            if (SetWaitableTimer(timer.handle, &dueTime, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer.handle, INFINITE);
                return;
            }
        }
    }
    const DWORD ms = static_cast<DWORD>((microseconds + 999ULL) / 1000ULL);
    Sleep(ms > 0 ? ms : 1u);
#else
    struct timespec req{};
    req.tv_sec = static_cast<time_t>(microseconds / 1000000ULL);
    req.tv_nsec = static_cast<long>((microseconds % 1000000ULL) * 1000ULL);
    nanosleep(&req, nullptr);
#endif
}

void promoteAndNameCallerOnce(int portType, std::uint32_t handle);

bool formatIsFloat(Format f) {
    return f == Format::F32Mono || f == Format::F32Stereo ||
           f == Format::F32_8Ch || f == Format::F32_8ChStd;
}

bool formatIsStd(Format f) {
    return f == Format::S16_8ChStd || f == Format::F32_8ChStd;
}

int channelsForFormat(Format f) {
    switch (f) {
        case Format::S16Mono:
        case Format::F32Mono:
            return 1;
        case Format::S16Stereo:
        case Format::F32Stereo:
            return 2;
        case Format::S16_8Ch:
        case Format::F32_8Ch:
        case Format::S16_8ChStd:
        case Format::F32_8ChStd:
            return 8;
        default:
            throw std::runtime_error("channelsForFormat: unknown format");
    }
}

SDL_AudioFormat sdlFormat(Format f) {
    return formatIsFloat(f) ? AUDIO_F32SYS : AUDIO_S16SYS;
}

std::uint32_t bytesPerSample(Format f) {
    return formatIsFloat(f) ? sizeof(float) : sizeof(std::int16_t);
}

// --- Opt-in low-overhead instrumentation (RYGT_AUDIO_DIAG=1, disabled by default) ---

enum class DiagEventKind : std::uint8_t {
    PortOpen = 1,
    PortClose = 2,
    ActiveUnderrun = 3,
    SilentUnderrun = 4,
    ProducerStall = 5,
    DrainTimeoutClear = 6,
    ExplicitDrain = 7,
    SdlError = 8,
    SilenceStateChange = 9,
    CallerThreadSeen = 10,
};

struct DiagEvent {
    std::uint64_t timestampUs = 0;
    std::uint32_t portHandle = 0;
    DiagEventKind kind = DiagEventKind::PortOpen;
    std::uint32_t arg0 = 0;
    std::uint32_t arg1 = 0;
};

constexpr std::uint32_t DIAG_RING_SIZE = 256;

struct DiagRing {
    std::array<DiagEvent, DIAG_RING_SIZE> events{};
    std::atomic<std::uint32_t> writeIdx{0};
    std::atomic<std::uint32_t> readIdx{0};

    void push(DiagEventKind kind, std::uint32_t handle, std::uint32_t a0, std::uint32_t a1) {
        const std::uint32_t w = writeIdx.load(std::memory_order_relaxed);
        events[w & (DIAG_RING_SIZE - 1u)] = DiagEvent{hostMicros(), handle, kind, a0, a1};
        writeIdx.store(w + 1u, std::memory_order_release);
    }
};

struct PortDiagCounters {
    std::atomic<std::uint64_t> blocks{0};
    std::atomic<std::uint64_t> silentBlocks{0};
    std::atomic<std::uint64_t> ringFramesSum{0};
    std::atomic<std::uint32_t> ringFramesMin{std::numeric_limits<std::uint32_t>::max()};
    std::atomic<std::uint32_t> ringFramesMax{0};
    std::atomic<std::uint64_t> intervalUsSum{0};
    std::atomic<std::uint64_t> intervalUsMax{0};
    std::atomic<std::uint64_t> producerStalls{0};
    std::atomic<std::uint64_t> lockWaitUsSum{0};
    std::atomic<std::uint64_t> lockWaitUsMax{0};
    std::atomic<std::uint64_t> paceWaitUsSum{0};
    std::atomic<std::uint64_t> paceWaitUsMax{0};
    std::atomic<std::uint64_t> convertUsMax{0};
    std::atomic<std::uint64_t> activeUnderruns{0};
    std::atomic<std::uint64_t> silentUnderruns{0};
    std::atomic<std::uint64_t> underrunFrames{0};
    std::atomic<std::uint64_t> queueClears{0};
    std::atomic<std::uint64_t> silentHealFrames{0};
    std::atomic<std::uint64_t> sdlErrors{0};

    void reset() {
        blocks.store(0, std::memory_order_relaxed);
        silentBlocks.store(0, std::memory_order_relaxed);
        ringFramesSum.store(0, std::memory_order_relaxed);
        ringFramesMin.store(std::numeric_limits<std::uint32_t>::max(), std::memory_order_relaxed);
        ringFramesMax.store(0, std::memory_order_relaxed);
        intervalUsSum.store(0, std::memory_order_relaxed);
        intervalUsMax.store(0, std::memory_order_relaxed);
        producerStalls.store(0, std::memory_order_relaxed);
        lockWaitUsSum.store(0, std::memory_order_relaxed);
        lockWaitUsMax.store(0, std::memory_order_relaxed);
        paceWaitUsSum.store(0, std::memory_order_relaxed);
        paceWaitUsMax.store(0, std::memory_order_relaxed);
        convertUsMax.store(0, std::memory_order_relaxed);
        activeUnderruns.store(0, std::memory_order_relaxed);
        silentUnderruns.store(0, std::memory_order_relaxed);
        underrunFrames.store(0, std::memory_order_relaxed);
        queueClears.store(0, std::memory_order_relaxed);
        silentHealFrames.store(0, std::memory_order_relaxed);
        sdlErrors.store(0, std::memory_order_relaxed);
    }
};

struct MixerDiagCounters {
    std::atomic<std::uint64_t> cbCount{0};
    std::atomic<std::uint64_t> cbPeriodUsMax{0};
    std::atomic<std::uint64_t> cbSlowOverruns{0};
    std::atomic<std::uint64_t> main3dSkewFramesMax{0};
    std::atomic<std::uint64_t> clipGuardHits{0};
};

template <typename T>
void atomicUpdateMax(std::atomic<T>& target, T val) {
    T cur = target.load(std::memory_order_relaxed);
    while (val > cur && !target.compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
}

template <typename T>
void atomicUpdateMin(std::atomic<T>& target, T val) {
    T cur = target.load(std::memory_order_relaxed);
    while (val < cur && !target.compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
}

struct Port {
    std::mutex ioMutex;
    std::atomic<PortSlotState> state{PortSlotState::Free};
    int type = 0;
    bool hardwareRouted = false;
    std::uint32_t samplesNum = 0;
    std::uint32_t freq = 0;
    Format format = Format::Unknown;
    int channels = 0;
    std::array<std::atomic<int>, 8> volume{};
    std::uint64_t lastOutputTime = 0;
    std::uint64_t lastHostProdUs = 0;

    // Optional resampler ONLY when port.freq != MASTER_FREQ (protected by ioMutex):
    SDL_AudioStream* resampleStream = nullptr;

    // Wait-free SPSC stereo float ring @ MASTER_FREQ (written under ioMutex, read ONLY in MasterMixer::sdlCallback):
    std::vector<float> ring;
    std::atomic<std::uint64_t> writeFrames{0};
    std::atomic<std::uint64_t> readFrames{0};
    std::atomic<bool> mixActive{false};
    std::atomic<bool> draining{false};
    std::atomic<bool> inSilenceRunAtomic{false};

    // Producer-only scratch buffers (under ioMutex, zero per-grain heap alloc):
    bool needsInitialPrime = true;
    bool inSilenceRun = false;
    std::uint32_t consecutiveSilentGrains = 0;
    std::vector<std::uint8_t> prepareBuf;
    std::vector<float> stereoFloatBuf;

    // Callback-only smoothing state (accessed ONLY inside MasterMixer::sdlCallback / while mixActive == false):
    bool wasInUnderrun = false;
    std::uint32_t fadeInRemaining = 0;
    float lastOutL = 0.0f;
    float lastOutR = 0.0f;

    float tracePeak = 0.0f;
    std::uint32_t traceBlocks = 0;
    PortDiagCounters diag{};

    std::uint64_t queuedFrames() const {
        const std::uint64_t w = writeFrames.load(std::memory_order_acquire);
        const std::uint64_t r = readFrames.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : 0ULL;
    }
};

std::mutex g_tableMutex;
Port g_ports[PORTS_MAX];
DiagRing g_diagRing;
MixerDiagCounters g_mixerDiag;

class MasterMixer {
public:
    MasterMixer() = default;

    ~MasterMixer() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            SDL_CloseAudioDevice(m_device);
            m_device = 0;
        }
    }

    bool acquireHardwarePort() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_activeHwPorts == 0) {
            if (!openDeviceLocked()) {
                return false;
            }
        }
        ++m_activeHwPorts;
        return true;
    }

    void releaseHardwarePort() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_activeHwPorts > 0) {
            --m_activeHwPorts;
        }
        if (m_activeHwPorts == 0 && m_device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            SDL_PauseAudioDevice(m_device, 1);
            SDL_CloseAudioDevice(m_device);
            m_device = 0;
            m_spec = {};
            m_lastCbUs.store(0, std::memory_order_relaxed);
        }
    }

    void syncCallbackBarrier() {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_device != 0 && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            SDL_LockAudioDevice(m_device);
            SDL_UnlockAudioDevice(m_device);
        }
    }

    std::uint32_t deviceId() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return static_cast<std::uint32_t>(m_device);
    }

    SDL_AudioSpec specSnapshot() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_spec;
    }

    void waitForTick(std::uint64_t observedEpoch, std::uint32_t timeoutMs) {
        std::unique_lock<std::mutex> lk(m_tickMutex);
        m_tickCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]() {
            return m_tickEpoch.load(std::memory_order_acquire) != observedEpoch;
        });
    }

    std::uint64_t currentTickEpoch() const {
        return m_tickEpoch.load(std::memory_order_acquire);
    }

private:
    bool openDeviceLocked() {
        if (!ensureSdlAudioLocked()) {
            return false;
        }
        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(MASTER_FREQ);
        desired.format = AUDIO_F32SYS;
        desired.channels = static_cast<Uint8>(MASTER_CHANNELS);
        desired.samples = static_cast<Uint16>(MASTER_PERIOD_FRAMES);
        desired.callback = &MasterMixer::sdlCallback;
        desired.userdata = this;

        SDL_AudioSpec obtained{};
        // Request exact 48 kHz F32SYS Stereo so SDL uses its built-in SDL_AudioStream if Windows differs.
        m_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (m_device == 0) {
            // Fallback allowing sample count change if driver rejects 480 samples.
            m_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
        }
        if (m_device == 0) {
            return false;
        }
        m_spec = obtained;
        m_periodUs = obtained.freq > 0
            ? (1000000ULL * static_cast<std::uint64_t>(obtained.samples)) / static_cast<std::uint64_t>(obtained.freq)
            : 10000ULL;
        m_lastCbUs.store(0, std::memory_order_relaxed);
        SDL_PauseAudioDevice(m_device, 0);
        return true;
    }

    bool ensureSdlAudioLocked() {
        if (m_sdlInitialized && SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            return true;
        }
        SDL_SetMainReady();
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
                return false;
            }
        }
        m_sdlInitialized = true;
        return true;
    }

    static void SDLCALL sdlCallback(void* userdata, Uint8* stream, int len) {
        static_cast<MasterMixer*>(userdata)->mixAudio(stream, len);
    }

    void mixAudio(Uint8* stream, int len) {
        if (!stream || len <= 0) return;
        const std::uint32_t wantFrames = static_cast<std::uint32_t>(len) / (MASTER_CHANNELS * sizeof(float));
        auto* out = reinterpret_cast<float*>(stream);
        std::memset(out, 0, static_cast<std::size_t>(wantFrames) * MASTER_CHANNELS * sizeof(float));

        const bool diag = getConfig().diagEnabled;
        if (diag) [[unlikely]] {
            const std::uint64_t nowUs = hostMicros();
            const std::uint64_t prevUs = m_lastCbUs.exchange(nowUs, std::memory_order_relaxed);
            g_mixerDiag.cbCount.fetch_add(1, std::memory_order_relaxed);
            if (prevUs != 0) {
                const std::uint64_t dt = nowUs - prevUs;
                atomicUpdateMax(g_mixerDiag.cbPeriodUsMax, dt);
                if (dt > m_periodUs * 2ULL + 5000ULL) {
                    g_mixerDiag.cbSlowOverruns.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        std::uint64_t mainDepthFrames = 0;
        std::uint64_t audio3dDepthFrames = 0;
        bool hasMain = false;
        bool hasAudio3d = false;

        for (int i = 0; i < PORTS_MAX; ++i) {
            Port& p = g_ports[i];
            if (!p.mixActive.load(std::memory_order_acquire)) {
                continue;
            }
            const std::uint64_t w = p.writeFrames.load(std::memory_order_acquire);
            const std::uint64_t r = p.readFrames.load(std::memory_order_relaxed);
            const std::uint64_t avail = (w >= r) ? (w - r) : 0ULL;

            if (p.type == PORT_TYPE_MAIN && !hasMain) {
                hasMain = true;
                mainDepthFrames = avail;
            } else if (p.type == PORT_TYPE_AUDIO3D && !hasAudio3d) {
                hasAudio3d = true;
                audio3dDepthFrames = avail;
            }

            if (diag) [[unlikely]] {
                const std::uint32_t av32 = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(avail, std::numeric_limits<std::uint32_t>::max()));
                p.diag.ringFramesSum.fetch_add(av32, std::memory_order_relaxed);
                atomicUpdateMin(p.diag.ringFramesMin, av32);
                atomicUpdateMax(p.diag.ringFramesMax, av32);
            }

            const std::uint32_t take = static_cast<std::uint32_t>(std::min<std::uint64_t>(avail, wantFrames));
            const float* ringData = p.ring.data();
            float lastL = p.lastOutL;
            float lastR = p.lastOutR;

            for (std::uint32_t f = 0; f < take; ++f) {
                const std::size_t idx = static_cast<std::size_t>((r + f) & (RING_FRAMES - 1u)) * 2u;
                float sL = ringData[idx];
                float sR = ringData[idx + 1u];
                if (p.fadeInRemaining > 0u) {
                    const std::uint32_t step = FADE_RAMP_FRAMES - p.fadeInRemaining;
                    const float gain = static_cast<float>(step + 1u) / static_cast<float>(FADE_RAMP_FRAMES);
                    sL *= gain;
                    sR *= gain;
                    --p.fadeInRemaining;
                }
                out[f * 2u] += sL;
                out[f * 2u + 1u] += sR;
                lastL = sL;
                lastR = sR;
            }
            p.readFrames.store(r + take, std::memory_order_release);

            if (take < wantFrames) {
                const std::uint32_t miss = wantFrames - take;
                const bool silentRun = p.inSilenceRunAtomic.load(std::memory_order_relaxed) ||
                                       p.draining.load(std::memory_order_relaxed);
                if (diag) [[unlikely]] {
                    if (silentRun) {
                        p.diag.silentUnderruns.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        p.diag.activeUnderruns.fetch_add(1, std::memory_order_relaxed);
                    }
                    p.diag.underrunFrames.fetch_add(miss, std::memory_order_relaxed);
                    if (!p.wasInUnderrun && !p.draining.load(std::memory_order_relaxed)) {
                        g_diagRing.push(
                            silentRun ? DiagEventKind::SilentUnderrun : DiagEventKind::ActiveUnderrun,
                            static_cast<std::uint32_t>(i + 1),
                            static_cast<std::uint32_t>(avail),
                            miss);
                    }
                }
                // Clickless exponential decay of the last real sample (no 20 ms silence bomb!)
                const std::uint32_t tailFrames = std::min<std::uint32_t>(miss, 32u);
                for (std::uint32_t f = 0; f < tailFrames; ++f) {
                    lastL *= 0.78f;
                    lastR *= 0.78f;
                    const std::size_t outIdx = static_cast<std::size_t>(take + f) * 2u;
                    out[outIdx] += lastL;
                    out[outIdx + 1u] += lastR;
                }
                lastL = 0.0f;
                lastR = 0.0f;
                p.wasInUnderrun = true;
                if (!silentRun && miss >= 2u) {
                    p.fadeInRemaining = FADE_RAMP_FRAMES;
                }
            } else {
                p.wasInUnderrun = false;
            }
            p.lastOutL = lastL;
            p.lastOutR = lastR;
        }

        if (diag && hasMain && hasAudio3d) [[unlikely]] {
            const std::uint64_t skew = mainDepthFrames > audio3dDepthFrames
                ? (mainDepthFrames - audio3dDepthFrames)
                : (audio3dDepthFrames - mainDepthFrames);
            atomicUpdateMax(g_mixerDiag.main3dSkewFramesMax, skew);
        }

        // Bit-exact within [-0.98, +0.98], smooth soft knee only if concurrent streams peak past 0.98f:
        const std::uint32_t totalSamples = wantFrames * MASTER_CHANNELS;
        for (std::uint32_t k = 0; k < totalSamples; ++k) {
            const float x = out[k];
            const float ax = x < 0.0f ? -x : x;
            if (ax > 0.98f) {
                const float sign = x < 0.0f ? -1.0f : 1.0f;
                out[k] = sign * (0.98f + 0.02f * std::tanh((ax - 0.98f) / 0.02f));
                if (diag) [[unlikely]] {
                    g_mixerDiag.clipGuardHits.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        {
            std::lock_guard<std::mutex> tlk(m_tickMutex);
            m_tickEpoch.fetch_add(1, std::memory_order_release);
        }
        m_tickCv.notify_all();
    }

    mutable std::mutex m_mutex;
    bool m_sdlInitialized = false;
    SDL_AudioDeviceID m_device = 0;
    SDL_AudioSpec m_spec{};
    std::uint64_t m_periodUs = 10000ULL;
    std::uint32_t m_activeHwPorts = 0;
    std::atomic<std::uint64_t> m_lastCbUs{0};

    std::mutex m_tickMutex;
    std::condition_variable m_tickCv;
    std::atomic<std::uint64_t> m_tickEpoch{0};
};

MasterMixer g_masterMixer;

void promoteAndNameCallerOnce(int portType, std::uint32_t handle) {
#ifdef _WIN32
    thread_local bool checked = false;
    if (checked) return;
    checked = true;

    int newPrio = THREAD_PRIORITY_NORMAL;
    if (getConfig().promoteThreadPrio) {
        const int cur = GetThreadPriority(GetCurrentThread());
        if (cur != THREAD_PRIORITY_ERROR_RETURN && cur < THREAD_PRIORITY_HIGHEST) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
        newPrio = GetThreadPriority(GetCurrentThread());
    }

    // Name only currently-unnamed threads (e.g. libSceAudio3d RunPortDrain) so
    // shadgt7's ConfineDriverCompileThreads() will not SuspendThread + GetThreadContext on them.
    using PFN_GetThreadDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    using PFN_SetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    bool assignedName = false;
    if (k32) {
        auto pGetDesc = reinterpret_cast<PFN_GetThreadDescription>(GetProcAddress(k32, "GetThreadDescription"));
        auto pSetDesc = reinterpret_cast<PFN_SetThreadDescription>(GetProcAddress(k32, "SetThreadDescription"));
        if (pGetDesc && pSetDesc) {
            PWSTR existing = nullptr;
            if (SUCCEEDED(pGetDesc(GetCurrentThread(), &existing))) {
                const bool empty = (existing == nullptr || existing[0] == L'\0');
                if (existing) LocalFree(existing);
                if (empty) {
                    const wchar_t* label = (portType == PORT_TYPE_AUDIO3D) ? L"Audio3dPortDrain" : L"AudioOutCaller";
                    if (SUCCEEDED(pSetDesc(GetCurrentThread(), label))) {
                        assignedName = true;
                    }
                }
            }
        }
    }

    if (getConfig().diagEnabled) [[unlikely]] {
        g_diagRing.push(DiagEventKind::CallerThreadSeen, handle,
                        static_cast<std::uint32_t>(GetCurrentThreadId()),
                        static_cast<std::uint32_t>((newPrio & 0xFFFF) | (assignedName ? 0x10000u : 0u)));
    }
#else
    (void)portType;
    (void)handle;
#endif
}

const char* eventKindName(DiagEventKind k) {
    switch (k) {
        case DiagEventKind::PortOpen: return "PORT_OPEN";
        case DiagEventKind::PortClose: return "PORT_CLOSE";
        case DiagEventKind::ActiveUnderrun: return "ACTIVE_UNDERRUN";
        case DiagEventKind::SilentUnderrun: return "SILENT_UNDERRUN";
        case DiagEventKind::ProducerStall: return "PRODUCER_STALL";
        case DiagEventKind::DrainTimeoutClear: return "DRAIN_TIMEOUT_CLEAR";
        case DiagEventKind::ExplicitDrain: return "EXPLICIT_DRAIN";
        case DiagEventKind::SdlError: return "SDL_ERROR";
        case DiagEventKind::SilenceStateChange: return "SILENCE_STATE";
        case DiagEventKind::CallerThreadSeen: return "CALLER_THREAD";
    }
    return "UNKNOWN";
}

class DiagBackgroundWriter {
public:
    DiagBackgroundWriter() {
        if (!getConfig().diagEnabled) return;
        m_running = true;
        m_worker = std::thread([this]() { run(); });
    }

    ~DiagBackgroundWriter() {
        if (!m_running) return;
        {
            std::lock_guard<std::mutex> lk(m_cvMutex);
            m_running = false;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void ensureStarted() {}

private:
    void flushOnce(std::FILE* fp) {
        if (!fp) return;
        const std::uint32_t w = g_diagRing.writeIdx.load(std::memory_order_acquire);
        std::uint32_t r = g_diagRing.readIdx.load(std::memory_order_relaxed);
        if (w - r > DIAG_RING_SIZE) {
            r = w - DIAG_RING_SIZE;
        }
        while (r != w) {
            const DiagEvent ev = g_diagRing.events[r & (DIAG_RING_SIZE - 1u)];
            std::fprintf(fp, "[event t=%.3fs] port=%u %s arg0=%u arg1=%u\n",
                         static_cast<double>(ev.timestampUs) / 1e6,
                         ev.portHandle, eventKindName(ev.kind), ev.arg0, ev.arg1);
            ++r;
        }
        g_diagRing.readIdx.store(w, std::memory_order_relaxed);

        const double nowSec = static_cast<double>(hostMicros()) / 1e6;
        const std::uint64_t cbCount = g_mixerDiag.cbCount.load(std::memory_order_relaxed);
        if (cbCount > 0) {
            const SDL_AudioSpec sp = g_masterMixer.specSnapshot();
            const double maxSkewMs = (static_cast<double>(g_mixerDiag.main3dSkewFramesMax.load(std::memory_order_relaxed)) * 1000.0) /
                                     static_cast<double>(MASTER_FREQ);
            std::fprintf(
                fp,
                "[mixer t=%.3fs] dev=%u %dHz/%uch/%usmp cbCount=%llu cbMaxUs=%llu slowCb=%llu main3dMaxSkewMs=%.2f clipHits=%llu\n",
                nowSec, g_masterMixer.deviceId(), sp.freq, static_cast<unsigned>(sp.channels),
                static_cast<unsigned>(sp.samples),
                static_cast<unsigned long long>(cbCount),
                static_cast<unsigned long long>(g_mixerDiag.cbPeriodUsMax.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_mixerDiag.cbSlowOverruns.load(std::memory_order_relaxed)),
                maxSkewMs,
                static_cast<unsigned long long>(g_mixerDiag.clipGuardHits.load(std::memory_order_relaxed)));
        }

        for (int i = 0; i < PORTS_MAX; ++i) {
            Port& p = g_ports[i];
            if (p.state.load(std::memory_order_acquire) != PortSlotState::Active) continue;
            const std::uint64_t blks = p.diag.blocks.load(std::memory_order_relaxed);
            if (blks == 0) continue;
            const std::uint64_t rSum = p.diag.ringFramesSum.load(std::memory_order_relaxed);
            const std::uint32_t rMin = p.diag.ringFramesMin.load(std::memory_order_relaxed);
            const std::uint32_t rMax = p.diag.ringFramesMax.load(std::memory_order_relaxed);
            const double denom = cbCount > 0 ? static_cast<double>(cbCount) : 1.0;
            const double rAvgMs = ((static_cast<double>(rSum) / denom) * 1000.0) / static_cast<double>(MASTER_FREQ);
            const double rMinMs = rMin == std::numeric_limits<std::uint32_t>::max()
                ? 0.0 : (static_cast<double>(rMin) * 1000.0) / static_cast<double>(MASTER_FREQ);
            const double rMaxMs = (static_cast<double>(rMax) * 1000.0) / static_cast<double>(MASTER_FREQ);
            std::fprintf(
                fp,
                "[summary t=%.3fs] port=%d(type=%d,hw=%d,%uHz,%uch,%usmp) blocks=%llu(silent=%llu) "
                "ringMs=[min=%.2f avg=%.2f max=%.2f] prodMaxUs=%llu stalls=%llu lockMaxUs=%llu paceMaxUs=%llu "
                "cvtMaxUs=%llu actUnder=%llu silUnder=%llu underF=%llu healF=%llu clears=%llu sdlErr=%llu\n",
                nowSec, i + 1, p.type, p.hardwareRouted ? 1 : 0, p.freq, p.channels, p.samplesNum,
                static_cast<unsigned long long>(blks),
                static_cast<unsigned long long>(p.diag.silentBlocks.load(std::memory_order_relaxed)),
                rMinMs, rAvgMs, rMaxMs,
                static_cast<unsigned long long>(p.diag.intervalUsMax.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.producerStalls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.lockWaitUsMax.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.paceWaitUsMax.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.convertUsMax.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.activeUnderruns.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.silentUnderruns.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.underrunFrames.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.silentHealFrames.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.queueClears.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(p.diag.sdlErrors.load(std::memory_order_relaxed)));
        }
        std::fflush(fp);
    }

    void run() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
        std::FILE* fp = std::fopen(getConfig().diagPath.c_str(), "a");
        if (fp) {
            std::fprintf(fp, "=== RyGT7 AudioOut v2 Unified-Clock Diagnostic Session Started ===\n");
            std::fflush(fp);
        }
        std::unique_lock<std::mutex> lk(m_cvMutex);
        while (m_running) {
            m_cv.wait_for(lk, std::chrono::milliseconds(2000), [this]() { return !m_running; });
            lk.unlock();
            flushOnce(fp);
            lk.lock();
        }
        lk.unlock();
        flushOnce(fp);
        if (fp) {
            std::fprintf(fp, "=== RyGT7 AudioOut v2 Diagnostic Session Ended ===\n");
            std::fclose(fp);
        }
    }

    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    bool m_running = false;
    std::thread m_worker;
};

void touchDiagWriter() {
    static DiagBackgroundWriter writer;
    writer.ensureStarted();
}

bool shouldOpenHardwareDevice(int type) {
    if (type == PORT_TYPE_VIBRATION) {
        return false;
    }
    if (type == PORT_TYPE_PADSPK && !getConfig().padSpkEnabled) {
        return false;
    }
    return true;
}

bool openPortAudio(Port& port, std::uint32_t handle) {
    if (!g_masterMixer.acquireHardwarePort()) {
        if (getConfig().diagEnabled) [[unlikely]] {
            port.diag.sdlErrors.fetch_add(1, std::memory_order_relaxed);
            g_diagRing.push(DiagEventKind::SdlError, handle, 1, 0);
        }
        return false;
    }

    port.ring.assign(RING_FRAMES * MASTER_CHANNELS, 0.0f);
    port.writeFrames.store(0, std::memory_order_relaxed);
    port.readFrames.store(0, std::memory_order_relaxed);
    port.mixActive.store(false, std::memory_order_relaxed);
    port.draining.store(false, std::memory_order_relaxed);
    port.inSilenceRunAtomic.store(false, std::memory_order_relaxed);

    const std::size_t preparedSize = static_cast<std::size_t>(port.samplesNum) *
        static_cast<std::size_t>(port.channels) * bytesPerSample(port.format);
    port.prepareBuf.assign(preparedSize, 0u);
    const std::size_t maxOutFrames = (port.freq == MASTER_FREQ)
        ? port.samplesNum
        : static_cast<std::size_t>(
              ((static_cast<std::uint64_t>(port.samplesNum) * MASTER_FREQ) / std::max(1u, port.freq)) + 64u);
    port.stereoFloatBuf.assign(maxOutFrames * MASTER_CHANNELS, 0.0f);

    if (port.freq != MASTER_FREQ) {
        port.resampleStream = SDL_NewAudioStream(
            sdlFormat(port.format), static_cast<Uint8>(port.channels), static_cast<int>(port.freq),
            AUDIO_F32SYS, static_cast<Uint8>(MASTER_CHANNELS), static_cast<int>(MASTER_FREQ));
        if (!port.resampleStream) {
            g_masterMixer.releaseHardwarePort();
            throw std::runtime_error(std::string("SDL_NewAudioStream: ") + SDL_GetError());
        }
    } else {
        port.resampleStream = nullptr;
    }

    port.hardwareRouted = true;
    port.needsInitialPrime = true;
    port.wasInUnderrun = false;
    port.fadeInRemaining = 0;
    port.lastOutL = 0.0f;
    port.lastOutR = 0.0f;
    return true;
}

void closePortAudio(Port& port) {
    port.mixActive.store(false, std::memory_order_release);
    if (port.hardwareRouted) {
        g_masterMixer.syncCallbackBarrier();
    }
    if (port.resampleStream) {
        SDL_FreeAudioStream(port.resampleStream);
        port.resampleStream = nullptr;
    }
    if (port.hardwareRouted) {
        g_masterMixer.releaseHardwarePort();
        port.hardwareRouted = false;
    }
    port.writeFrames.store(0, std::memory_order_relaxed);
    port.readFrames.store(0, std::memory_order_relaxed);
    port.needsInitialPrime = true;
    port.wasInUnderrun = false;
    port.fadeInRemaining = 0;
    port.lastOutL = 0.0f;
    port.lastOutR = 0.0f;
}

bool hasAnotherActiveMainPort(const Port& self) {
    for (int i = 0; i < PORTS_MAX; ++i) {
        const Port& p = g_ports[i];
        if (&p == &self) continue;
        if (p.hardwareRouted && p.type == PORT_TYPE_MAIN &&
            p.state.load(std::memory_order_acquire) == PortSlotState::Active) {
            return true;
        }
    }
    return false;
}

const void* prepareBuffer(Port& port, const void* data) {
    const auto frames = port.samplesNum;
    const auto ch = static_cast<std::uint32_t>(port.channels);
    const auto bps = bytesPerSample(port.format);
    const auto size = static_cast<std::size_t>(frames) * ch * bps;

    int vols[8];
    bool volumeChanged = false;
    for (std::uint32_t i = 0; i < ch; i++) {
        vols[i] = port.volume[i].load(std::memory_order_relaxed);
        if (vols[i] != DEFAULT_VOLUME) {
            volumeChanged = true;
        }
    }

    if (!volumeChanged && !formatIsStd(port.format)) {
        return data;
    }

    if (port.prepareBuf.size() < size) {
        port.prepareBuf.resize(size);
    }
    const bool isStd = formatIsStd(port.format) && ch == 8;

    if (formatIsFloat(port.format)) {
        auto* dst = reinterpret_cast<float*>(port.prepareBuf.data());
        const auto* src = static_cast<const float*>(data);
        for (std::uint32_t fr = 0; fr < frames; fr++) {
            for (std::uint32_t c = 0; c < ch; c++) {
                const auto srcCh = isStd ? STD_8CH_MAP[c] : c;
                dst[fr * ch + c] = src[fr * ch + srcCh] *
                    (static_cast<float>(vols[c]) / static_cast<float>(DEFAULT_VOLUME));
            }
        }
    } else {
        auto* dst = reinterpret_cast<std::int16_t*>(port.prepareBuf.data());
        const auto* src = static_cast<const std::int16_t*>(data);
        for (std::uint32_t fr = 0; fr < frames; fr++) {
            for (std::uint32_t c = 0; c < ch; c++) {
                const auto srcCh = isStd ? STD_8CH_MAP[c] : c;
                std::int64_t s = static_cast<std::int64_t>(src[fr * ch + srcCh]) *
                    vols[c] / DEFAULT_VOLUME;
                s = std::clamp(s,
                    static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max()));
                dst[fr * ch + c] = static_cast<std::int16_t>(s);
            }
        }
    }
    return port.prepareBuf.data();
}

// Fast direct conversion into 48 kHz stereo float (exact SDL2 7.1->stereo weights when ch==8):
std::uint32_t convertToMixerStereo(Port& port, const void* prepared) {
    const std::uint32_t frames = port.samplesNum;
    const std::uint32_t ch = static_cast<std::uint32_t>(port.channels);
    const bool isFloat = formatIsFloat(port.format);

    if (port.freq != MASTER_FREQ && port.resampleStream != nullptr) {
        const int srcBytes = static_cast<int>(frames * ch * bytesPerSample(port.format));
        if (SDL_AudioStreamPut(port.resampleStream, prepared, srcBytes) != 0) {
            return 0u;
        }
        const int availBytes = SDL_AudioStreamAvailable(port.resampleStream);
        if (availBytes <= 0) return 0u;
        const std::uint32_t outFrames = static_cast<std::uint32_t>(availBytes) / (MASTER_CHANNELS * sizeof(float));
        if (port.stereoFloatBuf.size() < outFrames * MASTER_CHANNELS) {
            port.stereoFloatBuf.resize(outFrames * MASTER_CHANNELS);
        }
        const int got = SDL_AudioStreamGet(port.resampleStream, port.stereoFloatBuf.data(),
                                           static_cast<int>(outFrames * MASTER_CHANNELS * sizeof(float)));
        return got > 0 ? static_cast<std::uint32_t>(got) / (MASTER_CHANNELS * sizeof(float)) : 0u;
    }

    if (port.stereoFloatBuf.size() < static_cast<std::size_t>(frames) * MASTER_CHANNELS) {
        port.stereoFloatBuf.resize(static_cast<std::size_t>(frames) * MASTER_CHANNELS);
    }
    float* dst = port.stereoFloatBuf.data();

    if (isFloat) {
        const float* src = static_cast<const float*>(prepared);
        if (ch == 2) {
            std::memcpy(dst, src, static_cast<std::size_t>(frames) * 2u * sizeof(float));
        } else if (ch == 1) {
            for (std::uint32_t f = 0; f < frames; ++f) {
                const float s = src[f];
                dst[f * 2u] = s;
                dst[f * 2u + 1u] = s;
            }
        } else if (ch == 8) {
            for (std::uint32_t f = 0; f < frames; ++f, src += 8, dst += 2) {
                const float fc = src[2], lfe = src[3], bl = src[4], br = src[5], sl = src[6], sr = src[7];
                dst[0] = (src[0] * 0.211866662f) + (fc * 0.150266662f) + (lfe * 0.066666670f) +
                         (bl * 0.181066677f) + (br * 0.111066669f) + (sl * 0.194133341f) + (sr * 0.085866667f);
                dst[1] = (src[1] * 0.211866662f) + (fc * 0.150266662f) + (lfe * 0.066666670f) +
                         (bl * 0.111066669f) + (br * 0.181066677f) + (sl * 0.085866667f) + (sr * 0.194133341f);
            }
        }
    } else {
        const auto* src = static_cast<const std::int16_t*>(prepared);
        constexpr float invScale = 1.0f / 32768.0f;
        if (ch == 2) {
            for (std::uint32_t f = 0; f < frames; ++f) {
                dst[f * 2u] = static_cast<float>(src[f * 2u]) * invScale;
                dst[f * 2u + 1u] = static_cast<float>(src[f * 2u + 1u]) * invScale;
            }
        } else if (ch == 1) {
            for (std::uint32_t f = 0; f < frames; ++f) {
                const float s = static_cast<float>(src[f]) * invScale;
                dst[f * 2u] = s;
                dst[f * 2u + 1u] = s;
            }
        } else if (ch == 8) {
            for (std::uint32_t f = 0; f < frames; ++f, src += 8, dst += 2) {
                const float fl  = static_cast<float>(src[0]) * invScale;
                const float fr  = static_cast<float>(src[1]) * invScale;
                const float fc  = static_cast<float>(src[2]) * invScale;
                const float lfe = static_cast<float>(src[3]) * invScale;
                const float bl  = static_cast<float>(src[4]) * invScale;
                const float br  = static_cast<float>(src[5]) * invScale;
                const float sl  = static_cast<float>(src[6]) * invScale;
                const float sr  = static_cast<float>(src[7]) * invScale;
                dst[0] = (fl * 0.211866662f) + (fc * 0.150266662f) + (lfe * 0.066666670f) +
                         (bl * 0.181066677f) + (br * 0.111066669f) + (sl * 0.194133341f) + (sr * 0.085866667f);
                dst[1] = (fr * 0.211866662f) + (fc * 0.150266662f) + (lfe * 0.066666670f) +
                         (bl * 0.111066669f) + (br * 0.181066677f) + (sl * 0.085866667f) + (sr * 0.194133341f);
            }
        }
    }
    return frames;
}

void writeRingZeroFrames(Port& port, std::uint32_t zeroFrames) {
    if (zeroFrames == 0) return;
    std::uint64_t w = port.writeFrames.load(std::memory_order_relaxed);
    const std::uint64_t r = port.readFrames.load(std::memory_order_acquire);
    const std::uint64_t capFree = RING_FRAMES - std::min<std::uint64_t>(RING_FRAMES, (w >= r ? w - r : 0ULL));
    const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::uint64_t>(zeroFrames, capFree));
    float* dst = port.ring.data();
    for (std::uint32_t f = 0; f < count; ++f) {
        const std::size_t idx = static_cast<std::size_t>((w + f) & (RING_FRAMES - 1u)) * 2u;
        dst[idx] = 0.0f;
        dst[idx + 1u] = 0.0f;
    }
    port.writeFrames.store(w + count, std::memory_order_release);
}

void writeRingStereoFrames(Port& port, const float* src, std::uint32_t frames) {
    if (frames == 0) return;
    std::uint64_t w = port.writeFrames.load(std::memory_order_relaxed);
    const std::uint64_t r = port.readFrames.load(std::memory_order_acquire);
    const std::uint64_t used = (w >= r) ? (w - r) : 0ULL;
    const std::uint64_t capFree = RING_FRAMES - std::min<std::uint64_t>(RING_FRAMES, used);
    const std::uint32_t pushCount = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, capFree));
    float* dst = port.ring.data();
    for (std::uint32_t f = 0; f < pushCount; ++f) {
        const std::size_t idx = static_cast<std::size_t>((w + f) & (RING_FRAMES - 1u)) * 2u;
        dst[idx] = src[f * 2u];
        dst[idx + 1u] = src[f * 2u + 1u];
    }
    port.writeFrames.store(w + pushCount, std::memory_order_release);
}

void traceLevel(Port& port, std::uint32_t handle, const void* data) {
    const auto& cfg = getConfig();
    if (data == nullptr) return;
    // Note: silence detection runs very cheaply at low overhead when diagEnabled, or always lightweight?
    // Keeping exact zero-cost fast path when both traceAudioOut & diagEnabled are false, OR checking cheap sparse samples:
    // Actually let's check peak ONLY when traceAudioOut or diagEnabled, EXCEPT wait: silent-run tracking helps self-healing too!
    // Wait: checking 256 stereo samples is 256 comparisons (~80 ns), negligible compared to format conversion!
    const std::size_t samples = static_cast<std::size_t>(port.samplesNum) * port.channels;
    const bool isFloat = formatIsFloat(port.format);
    float blockPeak = 0.0f;
    for (std::size_t i = 0; i < samples; ++i) {
        const float value = isFloat ? static_cast<const float*>(data)[i]
                                    : static_cast<const std::int16_t*>(data)[i] / 32768.0f;
        const float absVal = value < 0.0f ? -value : value;
        if (absVal > blockPeak) blockPeak = absVal;
    }

    const bool silent = (blockPeak < 1.0e-5f);
    if (silent) {
        if (cfg.diagEnabled) [[unlikely]] {
            port.diag.silentBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        if (++port.consecutiveSilentGrains == 16u && !port.inSilenceRun) {
            port.inSilenceRun = true;
            port.inSilenceRunAtomic.store(true, std::memory_order_relaxed);
            if (cfg.diagEnabled) [[unlikely]] {
                g_diagRing.push(DiagEventKind::SilenceStateChange, handle, 1u, 0u);
            }
        }
    } else {
        port.consecutiveSilentGrains = 0u;
        if (port.inSilenceRun) {
            port.inSilenceRun = false;
            port.inSilenceRunAtomic.store(false, std::memory_order_relaxed);
            if (cfg.diagEnabled) [[unlikely]] {
                g_diagRing.push(DiagEventKind::SilenceStateChange, handle, 0u,
                                static_cast<std::uint32_t>(blockPeak * 10000.0f));
            }
        }
    }

    if (cfg.traceAudioOut) [[unlikely]] {
        port.tracePeak = std::max(port.tracePeak, blockPeak);
        if (++port.traceBlocks % 200 == 0) {
            std::fprintf(stderr, "[audioout] port %u (type %d): peak %.4f over the last 200 blocks\n",
                         handle, port.type, port.tracePeak);
            port.tracePeak = 0.0f;
        }
    }
}

std::uint32_t targetFramesForConfig(const RuntimeConfig& cfg) {
    return static_cast<std::uint32_t>((cfg.targetLatencyUs * MASTER_FREQ) / 1000000ULL);
}

std::uint32_t highWatermarkFramesForPort(Port& port, const RuntimeConfig& cfg) {
    const std::uint32_t primaryFrames = std::clamp(targetFramesForConfig(cfg), 960u, 5760u);
    if (port.type == PORT_TYPE_MAIN || !hasAnotherActiveMainPort(port)) {
        return primaryFrames;
    }
    const std::uint32_t secFrames = static_cast<std::uint32_t>((cfg.secondaryHeadroomMs * MASTER_FREQ) / 1000ULL);
    return std::max(primaryFrames + 1440u, std::min<std::uint32_t>(secFrames, RING_FRAMES - 1024u));
}

void queueAudio(std::unique_lock<std::mutex>& ioLock, Port& port, std::uint32_t handle, const void* data) {
    if (!port.hardwareRouted) return;
    const auto& cfg = getConfig();

    if (data == nullptr) {
        port.draining.store(true, std::memory_order_release);
        if (cfg.diagEnabled) [[unlikely]] {
            g_diagRing.push(DiagEventKind::ExplicitDrain, handle,
                            static_cast<std::uint32_t>(port.queuedFrames()), 0u);
        }
        const std::uint64_t waitStartUs = hostMicros();
        while (port.queuedFrames() > 0) {
            if (hostMicros() - waitStartUs > DRAIN_TIMEOUT_US) {
                if (cfg.diagEnabled) [[unlikely]] {
                    port.diag.queueClears.fetch_add(1, std::memory_order_relaxed);
                    g_diagRing.push(DiagEventKind::DrainTimeoutClear, handle, 0u, 1u);
                }
                break;
            }
            const std::uint64_t epoch = g_masterMixer.currentTickEpoch();
            ioLock.unlock();
            g_masterMixer.waitForTick(epoch, 20u);
            ioLock.lock();
            if (port.state.load(std::memory_order_acquire) != PortSlotState::Active) {
                return;
            }
        }
        port.mixActive.store(false, std::memory_order_release);
        g_masterMixer.syncCallbackBarrier();
        port.draining.store(false, std::memory_order_relaxed);
        port.writeFrames.store(0, std::memory_order_relaxed);
        port.readFrames.store(0, std::memory_order_relaxed);
        if (port.resampleStream) SDL_AudioStreamClear(port.resampleStream);
        port.needsInitialPrime = true;
        port.wasInUnderrun = false;
        port.fadeInRemaining = 0;
        port.lastOutL = 0.0f;
        port.lastOutR = 0.0f;
        return;
    }

    const std::uint64_t prodNowUs = hostMicros();
    const std::uint64_t bufferUs = port.freq != 0
        ? (1000000ULL * port.samplesNum) / port.freq
        : 0;

    if (cfg.diagEnabled) [[unlikely]] {
        if (port.lastHostProdUs != 0) {
            const std::uint64_t deltaUs = prodNowUs - port.lastHostProdUs;
            port.diag.intervalUsSum.fetch_add(deltaUs, std::memory_order_relaxed);
            atomicUpdateMax(port.diag.intervalUsMax, deltaUs);
            if (bufferUs > 0 && deltaUs > std::max<std::uint64_t>(bufferUs * 2ULL + 2000ULL, 12000ULL)) {
                port.diag.producerStalls.fetch_add(1, std::memory_order_relaxed);
                g_diagRing.push(DiagEventKind::ProducerStall, handle,
                                static_cast<std::uint32_t>(deltaUs),
                                static_cast<std::uint32_t>(port.queuedFrames()));
            }
        }
        port.lastHostProdUs = prodNowUs;
    }

    const std::uint64_t cvtStartUs = cfg.diagEnabled ? prodNowUs : 0;
    const void* prepared = prepareBuffer(port, data);
    const std::uint32_t outFrames = convertToMixerStereo(port, prepared);
    if (cfg.diagEnabled) [[unlikely]] {
        atomicUpdateMax(port.diag.convertUsMax, hostMicros() - cvtStartUs);
    }

    if (outFrames == 0) {
        if (cfg.diagEnabled) [[unlikely]] {
            port.diag.blocks.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    const std::uint32_t highWater = highWatermarkFramesForPort(port, cfg);
    const std::uint64_t paceStartUs = cfg.diagEnabled ? hostMicros() : 0;

    if (!port.needsInitialPrime) {
        const std::uint64_t timeoutStart = paceStartUs != 0 ? paceStartUs : hostMicros();
        while (port.queuedFrames() + outFrames > highWater) {
            if (hostMicros() - timeoutStart > DRAIN_TIMEOUT_US) {
                break;
            }
            const std::uint64_t epoch = g_masterMixer.currentTickEpoch();
            ioLock.unlock();
            g_masterMixer.waitForTick(epoch, 20u);
            ioLock.lock();
            if (port.state.load(std::memory_order_acquire) != PortSlotState::Active) {
                return;
            }
        }
        if (cfg.diagEnabled) [[unlikely]] {
            const std::uint64_t paceUs = hostMicros() - paceStartUs;
            port.diag.paceWaitUsSum.fetch_add(paceUs, std::memory_order_relaxed);
            atomicUpdateMax(port.diag.paceWaitUsMax, paceUs);
        }
    }

    const std::uint32_t seedFrames = std::clamp((targetFramesForConfig(cfg) * 3u) / 8u, 480u, 960u);
    if (port.needsInitialPrime) {
        port.mixActive.store(false, std::memory_order_release);
        g_masterMixer.syncCallbackBarrier();
        port.readFrames.store(0, std::memory_order_relaxed);
        port.writeFrames.store(0, std::memory_order_relaxed);
        port.wasInUnderrun = false;
        port.fadeInRemaining = 0;
        port.lastOutL = 0.0f;
        port.lastOutR = 0.0f;
        writeRingZeroFrames(port, seedFrames);
        writeRingStereoFrames(port, port.stereoFloatBuf.data(), outFrames);
        port.needsInitialPrime = false;
        port.mixActive.store(true, std::memory_order_release);
    } else {
        // Silent-run self-heal ONLY during extended digital silence (completely inaudible):
        if (port.inSilenceRun && port.consecutiveSilentGrains >= 24u) {
            const std::uint64_t curQ = port.queuedFrames();
            if (curQ < seedFrames) {
                const std::uint32_t heal = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(outFrames, seedFrames - curQ));
                writeRingZeroFrames(port, heal);
                if (cfg.diagEnabled) [[unlikely]] {
                    port.diag.silentHealFrames.fetch_add(heal, std::memory_order_relaxed);
                }
            }
        }
        writeRingStereoFrames(port, port.stereoFloatBuf.data(), outFrames);
    }

    if (cfg.diagEnabled) [[unlikely]] {
        port.diag.blocks.fetch_add(1, std::memory_order_relaxed);
    }
}

bool portTypeValid(int type) {
    return (type >= PORT_TYPE_MAIN && type <= PORT_TYPE_PADSPK) ||
           type == PORT_TYPE_VIBRATION || type == PORT_TYPE_AUDIO3D || type == PORT_TYPE_AUX;
}

Port* getActivePort(int handle) {
    const int idx = handle - 1;
    if (idx < 0 || idx >= PORTS_MAX) {
        return nullptr;
    }
    Port& port = g_ports[idx];
    if (port.state.load(std::memory_order_acquire) != PortSlotState::Active) {
        return nullptr;
    }
    return &port;
}

} // namespace

extern "C" {

int APS5_VABI sceAudioOutDeviceIdOpen() {
    return 0;
}

int APS5_VABI sceAudioOutInit() {
    touchDiagWriter();
    return 0;
}

int APS5_VABI sceAudioOutOpen(int userId, int type, int index, std::uint32_t len,
    std::uint32_t freq, std::uint32_t param) {
    (void)userId;
    touchDiagWriter();

    if (!portTypeValid(type)) {
        return -2144993270;
    }
    if (len == 0) {
        return -2144993274;
    }
    if (freq == 0) {
        return -2144993272;
    }
    if (index != 0) {
        throw std::runtime_error("sceAudioOutOpen: index != 0 not supported");
    }

    Format format = Format::Unknown;
    switch (param & FORMAT_MASK) {
        case 0: format = Format::S16Mono; break;
        case 1: format = Format::S16Stereo; break;
        case 2: format = Format::S16_8Ch; break;
        case 3: format = Format::F32Mono; break;
        case 4: format = Format::F32Stereo; break;
        case 5: format = Format::F32_8Ch; break;
        case 6: format = Format::S16_8ChStd; break;
        case 7: format = Format::F32_8ChStd; break;
        default:
            throw std::runtime_error("sceAudioOutOpen: unknown format param");
    }

    int slotIdx = -1;
    {
        std::lock_guard<std::mutex> tableLock(g_tableMutex);
        for (int i = 0; i < PORTS_MAX; i++) {
            if (g_ports[i].state.load(std::memory_order_relaxed) == PortSlotState::Free) {
                g_ports[i].state.store(PortSlotState::Opening, std::memory_order_relaxed);
                slotIdx = i;
                break;
            }
        }
    }
    if (slotIdx < 0) {
        return -2144993275;
    }

    const std::uint32_t handle = static_cast<std::uint32_t>(slotIdx + 1);
    Port& port = g_ports[slotIdx];
    std::unique_lock<std::mutex> ioLock(port.ioMutex);
    try {
        port.type = type;
        port.hardwareRouted = false;
        port.samplesNum = len;
        port.freq = freq;
        port.format = format;
        port.channels = channelsForFormat(format);
        port.lastOutputTime = 0;
        port.lastHostProdUs = 0;
        port.resampleStream = nullptr;
        port.needsInitialPrime = true;
        port.inSilenceRun = false;
        port.inSilenceRunAtomic.store(false, std::memory_order_relaxed);
        port.consecutiveSilentGrains = 0;
        port.wasInUnderrun = false;
        port.fadeInRemaining = 0;
        port.lastOutL = 0.0f;
        port.lastOutR = 0.0f;
        port.tracePeak = 0.0f;
        port.traceBlocks = 0;
        port.diag.reset();
        for (int c = 0; c < 8; c++) {
            port.volume[c].store(c < port.channels ? DEFAULT_VOLUME : 0, std::memory_order_relaxed);
        }

        if (shouldOpenHardwareDevice(type)) {
            openPortAudio(port, handle);
        }

        port.state.store(PortSlotState::Active, std::memory_order_release);
    } catch (...) {
        closePortAudio(port);
        {
            std::lock_guard<std::mutex> tableLock(g_tableMutex);
            port.state.store(PortSlotState::Free, std::memory_order_release);
        }
        throw;
    }

    const auto& cfg = getConfig();
    if (cfg.diagEnabled) [[unlikely]] {
        g_diagRing.push(DiagEventKind::PortOpen, handle,
                        static_cast<std::uint32_t>(type), port.hardwareRouted ? g_masterMixer.deviceId() : 0u);
    }
    if (cfg.traceAudioOut) [[unlikely]] {
        std::fprintf(stderr, "[audioout] port %u: type %d, %u samples at %u Hz, format %d, hw=%d dev=%u\n",
                     handle, type, len, freq, static_cast<int>(format), port.hardwareRouted ? 1 : 0,
                     port.hardwareRouted ? g_masterMixer.deviceId() : 0u);
    }
    return static_cast<int>(handle);
}

int APS5_VABI sceAudioOutClose(int handle) {
    const int idx = handle - 1;
    if (idx < 0 || idx >= PORTS_MAX) {
        return -2144993277;
    }

    Port& port = g_ports[idx];
    {
        std::lock_guard<std::mutex> tableLock(g_tableMutex);
        if (port.state.load(std::memory_order_acquire) != PortSlotState::Active) {
            return -2144993277;
        }
        port.state.store(PortSlotState::Closing, std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> ioLock(port.ioMutex);
        closePortAudio(port);
        port.lastOutputTime = 0;
        port.lastHostProdUs = 0;
    }

    if (getConfig().diagEnabled) [[unlikely]] {
        g_diagRing.push(DiagEventKind::PortClose, static_cast<std::uint32_t>(handle), 0u, 0u);
    }

    {
        std::lock_guard<std::mutex> tableLock(g_tableMutex);
        port.state.store(PortSlotState::Free, std::memory_order_release);
    }
    return 0;
}

int APS5_VABI sceAudioOutOutput(int handle, const void* ptr) {
    Port* port = getActivePort(handle);
    if (port == nullptr) {
        return -2144993277;
    }

    promoteAndNameCallerOnce(port->type, static_cast<std::uint32_t>(handle));

    const bool diag = getConfig().diagEnabled;
    const std::uint64_t lockStartUs = diag ? hostMicros() : 0;
    std::unique_lock<std::mutex> ioLock(port->ioMutex);
    if (diag) [[unlikely]] {
        const std::uint64_t lockUs = hostMicros() - lockStartUs;
        port->diag.lockWaitUsSum.fetch_add(lockUs, std::memory_order_relaxed);
        atomicUpdateMax(port->diag.lockWaitUsMax, lockUs);
    }

    if (port->state.load(std::memory_order_acquire) != PortSlotState::Active) {
        return -2144993277;
    }

    const std::uint32_t samplesNum = port->samplesNum;
    const std::uint64_t blockUs = (1000000ULL * samplesNum) / port->freq;
    const std::uint64_t now = sceKernelGetProcessTime();
    const std::uint64_t next = port->lastOutputTime + blockUs;
    if (next > now && !port->hardwareRouted) {
        const std::uint64_t waitUs = next - now;
        ioLock.unlock();
        preciseSleepUs(waitUs);
        ioLock.lock();
        if (port->state.load(std::memory_order_acquire) != PortSlotState::Active) {
            return -2144993277;
        }
    }

    traceLevel(*port, static_cast<std::uint32_t>(handle), ptr);
    queueAudio(ioLock, *port, static_cast<std::uint32_t>(handle), ptr);
    port->lastOutputTime = sceKernelGetProcessTime();
    return static_cast<int>(samplesNum);
}

int APS5_VABI sceAudioOutOutputs(AudioOutOutputParam* param, std::uint32_t num) {
    if (param == nullptr || num == 0) {
        return -2144993276;
    }

    for (std::uint32_t i = 0; i < num; i++) {
        if (getActivePort(param[i].handle) == nullptr) {
            return -2144993277;
        }
    }

    Port* first = getActivePort(param[0].handle);
    if (first == nullptr) {
        return -2144993277;
    }
    promoteAndNameCallerOnce(first->type, static_cast<std::uint32_t>(param[0].handle));

    const std::uint32_t firstSamples = first->samplesNum;
    const std::uint64_t blockUs = (1000000ULL * firstSamples) / first->freq;
    const std::uint64_t now = sceKernelGetProcessTime();

    std::uint64_t maxWait = 0;
    bool anyHw = false;
    for (std::uint32_t i = 0; i < num; i++) {
        Port* p = getActivePort(param[i].handle);
        if (p == nullptr) {
            return -2144993277;
        }
        std::lock_guard<std::mutex> ioLock(p->ioMutex);
        if (p->hardwareRouted) {
            anyHw = true;
        }
        const std::uint64_t next = p->lastOutputTime + blockUs;
        const std::uint64_t wait = next > now ? next - now : 0;
        if (wait > maxWait) {
            maxWait = wait;
        }
    }

    if (maxWait != 0 && !anyHw) {
        preciseSleepUs(maxWait);
    }

    const bool diag = getConfig().diagEnabled;
    for (std::uint32_t i = 0; i < num; i++) {
        const int h = param[i].handle;
        Port* port = getActivePort(h);
        if (port == nullptr) continue;

        const std::uint64_t lockStartUs = diag ? hostMicros() : 0;
        std::unique_lock<std::mutex> ioLock(port->ioMutex);
        if (diag) [[unlikely]] {
            const std::uint64_t lockUs = hostMicros() - lockStartUs;
            port->diag.lockWaitUsSum.fetch_add(lockUs, std::memory_order_relaxed);
            atomicUpdateMax(port->diag.lockWaitUsMax, lockUs);
        }
        if (port->state.load(std::memory_order_acquire) != PortSlotState::Active) {
            continue;
        }
        traceLevel(*port, static_cast<std::uint32_t>(h), param[i].ptr);
        queueAudio(ioLock, *port, static_cast<std::uint32_t>(h), param[i].ptr);
        port->lastOutputTime = sceKernelGetProcessTime();
    }

    return static_cast<int>(firstSamples);
}

int APS5_VABI sceAudioOutSetVolume(int handle, std::uint32_t flag, int* vol) {
    if (vol == nullptr) {
        return -2144993276;
    }
    Port* port = getActivePort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    const bool isStd = formatIsStd(port->format);
    for (int i = 0; i < port->channels; i++, flag >>= 1u) {
        if ((flag & 1u) == 0) {
            continue;
        }
        int srcIdx = i;
        if (isStd) {
            if (i == 4) srcIdx = 6;
            else if (i == 5) srcIdx = 7;
            else if (i == 6) srcIdx = 4;
            else if (i == 7) srcIdx = 5;
        }
        port->volume[i].store(vol[srcIdx], std::memory_order_relaxed);
    }
    return 0;
}

int APS5_VABI sceAudioOutGetPortState(int handle, AudioOutPortState* state) {
    if (state == nullptr) {
        return -2144993276;
    }
    Port* port = getActivePort(handle);
    if (port == nullptr) {
        return -2144993277;
    }
    state->rerouteCounter = 0;
    state->volume = 127;
    state->flag = 0;
    state->activeState = 0;
    state->reserved[0] = 0;
    switch (port->type) {
        case PORT_TYPE_MAIN:
        case PORT_TYPE_BGM:
        case PORT_TYPE_AUDIO3D:
            state->output = 1;
            state->channel = static_cast<std::uint8_t>(port->channels > 2 ? 2 : port->channels);
            break;
        case PORT_TYPE_VOICE:
        case PORT_TYPE_PERSONAL:
            state->output = 0x40;
            state->channel = 1;
            break;
        case PORT_TYPE_PADSPK:
        case PORT_TYPE_VIBRATION:
            state->output = 4;
            state->channel = 1;
            break;
        case PORT_TYPE_AUX:
            state->output = 0x80;
            state->channel = 0;
            break;
        default:
            throw std::runtime_error("sceAudioOutGetPortState: unknown port type");
    }
    return 0;
}

int APS5_VABI sceAudioOutSetMixLevelPadSpk(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

} // extern "C"
