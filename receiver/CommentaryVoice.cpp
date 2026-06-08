#define _CRT_SECURE_NO_WARNINGS
#include "CommentaryVoice.h"

#include <Windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <sapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kSampleRate = 22050;
constexpr int kBitsPerSample = 16;
constexpr int kChannels = 1;
constexpr size_t kSeenSequenceLimit = 256;

struct VoiceConfig {
    bool enabled = true;
    std::string preset = "radio";
    int volume = 85;
    std::string voice;
    int rate = 1;
    int pitchSemitones = -2;
    int interruptPriority = 90;
    int queueLimit = 3;
};

struct VoiceItem {
    std::string text;
    int priority = 0;
    unsigned long long sequence = 0;
};

VoiceConfig g_config;
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<VoiceItem> g_queue;
std::unordered_set<unsigned long long> g_seenSequences;
std::deque<unsigned long long> g_seenOrder;
std::thread g_worker;
std::once_flag g_startOnce;
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_enabled{ false };

std::mutex g_playbackMutex;
HWAVEOUT g_currentWaveOut = nullptr;
int g_currentPriority = 0;

void VoiceLog(const char* fmt, ...) {
    FILE* file = fopen("commentary_voice_debug.log", "a");
    if (!file) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(file, fmt, args);
    va_end(args);
    fclose(file);
}

std::string GetIniPath() {
    char dllPath[MAX_PATH] = {};
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&GetIniPath),
                           &module)) {
        GetModuleFileNameA(module, dllPath, MAX_PATH);
        std::string fullPath(dllPath);
        size_t lastSlash = fullPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return fullPath.substr(0, lastSlash + 1) + "vcstreamer.ini";
        }
    }
    return "vcstreamer.ini";
}

int ClampInt(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

int ReadIniInt(const std::string& path, const char* key, int defaultValue) {
    char buf[64] = {};
    GetPrivateProfileStringA("CommentaryVoice", key, "", buf, sizeof(buf), path.c_str());
    if (buf[0] == '\0') return defaultValue;
    return atoi(buf);
}

std::string ReadIniString(const std::string& path, const char* key, const char* defaultValue) {
    char buf[256] = {};
    GetPrivateProfileStringA("CommentaryVoice", key, defaultValue, buf, sizeof(buf), path.c_str());
    return std::string(buf);
}

VoiceConfig LoadConfig() {
    std::string path = GetIniPath();
    VoiceConfig cfg;
    cfg.enabled = ReadIniInt(path, "enabled", 1) != 0;
    cfg.preset = ReadIniString(path, "preset", "radio");
    cfg.volume = ClampInt(ReadIniInt(path, "volume", 85), 0, 100);
    cfg.voice = ReadIniString(path, "voice", "");
    cfg.rate = ClampInt(ReadIniInt(path, "rate", 1), -10, 10);
    cfg.pitchSemitones = ClampInt(ReadIniInt(path, "pitch_semitones", -2), -12, 12);
    cfg.interruptPriority = ClampInt(ReadIniInt(path, "interrupt_priority", 90), 0, 100);
    cfg.queueLimit = ClampInt(ReadIniInt(path, "queue_limit", 3), 1, 12);
    return cfg;
}

std::wstring ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();

    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (needed <= 0) {
        codePage = CP_ACP;
        flags = 0;
        needed = MultiByteToWideChar(codePage, flags, text.c_str(), -1, nullptr, 0);
    }
    if (needed <= 0) return std::wstring();

    std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(codePage, flags, text.c_str(), -1, &wide[0], needed);
    return wide;
}

bool SelectConfiguredVoice(ISpVoice* voice, const std::string& configuredVoice) {
    if (!voice || configuredVoice.empty()) return true;

    std::wstring wideName = ToWide(configuredVoice);
    if (wideName.empty()) return true;

    ISpObjectTokenCategory* category = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory,
                                  nullptr,
                                  CLSCTX_ALL,
                                  IID_ISpObjectTokenCategory,
                                  reinterpret_cast<void**>(&category));
    if (FAILED(hr) || !category) {
        VoiceLog("voice category unavailable voice=%s hr=0x%08lx\n",
                 configuredVoice.c_str(),
                 static_cast<unsigned long>(hr));
        return false;
    }

    hr = category->SetId(SPCAT_VOICES, FALSE);
    IEnumSpObjectTokens* tokens = nullptr;
    if (SUCCEEDED(hr)) {
        hr = category->EnumTokens(nullptr, nullptr, &tokens);
    }
    category->Release();
    if (FAILED(hr) || !tokens) {
        VoiceLog("configured voice unavailable voice=%s hr=0x%08lx\n",
                 configuredVoice.c_str(),
                 static_cast<unsigned long>(hr));
        return false;
    }

    ISpObjectToken* selected = nullptr;
    ISpObjectToken* token = nullptr;
    while (tokens->Next(1, &token, nullptr) == S_OK && token) {
        ISpDataKey* attributes = nullptr;
        WCHAR* name = nullptr;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
            attributes->GetStringValue(L"Name", &name);
            attributes->Release();
        }
        if (name) {
            bool exact = _wcsicmp(name, wideName.c_str()) == 0;
            bool contains = wcsstr(name, wideName.c_str()) != nullptr;
            CoTaskMemFree(name);
            if (exact || contains) {
                selected = token;
                break;
            }
        }
        token->Release();
        token = nullptr;
    }
    tokens->Release();

    if (!selected) {
        VoiceLog("configured voice unavailable voice=%s\n", configuredVoice.c_str());
        return false;
    }

    hr = voice->SetVoice(selected);
    selected->Release();
    if (FAILED(hr)) {
        VoiceLog("SetVoice failed voice=%s hr=0x%08lx\n",
                 configuredVoice.c_str(),
                 static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool SynthesizePcm(const VoiceItem& item, const VoiceConfig& cfg, std::vector<short>& samples) {
    samples.clear();

    ISpVoice* voice = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice));
    if (FAILED(hr) || !voice) {
        VoiceLog("CoCreateInstance(CLSID_SpVoice) failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    if (!SelectConfiguredVoice(voice, cfg.voice)) {
        voice->Release();
        return false;
    }

    voice->SetRate(cfg.rate);
    voice->SetVolume(100);

    IStream* memoryStream = nullptr;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &memoryStream);
    if (FAILED(hr) || !memoryStream) {
        VoiceLog("CreateStreamOnHGlobal failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        voice->Release();
        return false;
    }

    ISpStream* sapiStream = nullptr;
    hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, reinterpret_cast<void**>(&sapiStream));
    if (FAILED(hr) || !sapiStream) {
        VoiceLog("CoCreateInstance(CLSID_SpStream) failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        memoryStream->Release();
        voice->Release();
        return false;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    hr = sapiStream->SetBaseStream(memoryStream, SPDFID_WaveFormatEx, &format);
    if (SUCCEEDED(hr)) {
        hr = voice->SetOutput(sapiStream, TRUE);
    }

    std::wstring text = ToWide(item.text);
    if (text.empty()) {
        sapiStream->Release();
        memoryStream->Release();
        voice->Release();
        return false;
    }

    if (SUCCEEDED(hr)) {
        hr = voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
    }
    if (SUCCEEDED(hr)) {
        voice->WaitUntilDone(INFINITE);
    }

    LARGE_INTEGER zero = {};
    memoryStream->Seek(zero, STREAM_SEEK_SET, nullptr);
    STATSTG stat = {};
    HRESULT statHr = memoryStream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || FAILED(statHr) || stat.cbSize.QuadPart <= 0) {
        VoiceLog("Speak failed seq=%llu hr=0x%08lx stat=0x%08lx bytes=%llu\n",
                 item.sequence,
                 static_cast<unsigned long>(hr),
                 static_cast<unsigned long>(statHr),
                 static_cast<unsigned long long>(stat.cbSize.QuadPart));
        sapiStream->Release();
        memoryStream->Release();
        voice->Release();
        return false;
    }

    ULONG bytesToRead = stat.cbSize.HighPart != 0 ? 0 : stat.cbSize.LowPart;
    if (bytesToRead < sizeof(short)) {
        sapiStream->Release();
        memoryStream->Release();
        voice->Release();
        return false;
    }

    std::vector<unsigned char> bytes(bytesToRead);
    ULONG bytesRead = 0;
    hr = memoryStream->Read(bytes.data(), bytesToRead, &bytesRead);
    if (FAILED(hr) || bytesRead < sizeof(short)) {
        VoiceLog("Read synthesized stream failed seq=%llu hr=0x%08lx bytes=%lu\n",
                 item.sequence,
                 static_cast<unsigned long>(hr),
                 static_cast<unsigned long>(bytesRead));
        sapiStream->Release();
        memoryStream->Release();
        voice->Release();
        return false;
    }

    size_t pcmOffset = 0;
    size_t pcmBytes = bytesRead;
    if (bytesRead >= 12
        && memcmp(bytes.data(), "RIFF", 4) == 0
        && memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
        size_t pos = 12;
        while (pos + 8 <= bytesRead) {
            unsigned int chunkSize = static_cast<unsigned int>(bytes[pos + 4])
                | (static_cast<unsigned int>(bytes[pos + 5]) << 8)
                | (static_cast<unsigned int>(bytes[pos + 6]) << 16)
                | (static_cast<unsigned int>(bytes[pos + 7]) << 24);
            size_t dataStart = pos + 8;
            if (memcmp(bytes.data() + pos, "data", 4) == 0) {
                pcmOffset = dataStart;
                pcmBytes = std::min<size_t>(chunkSize, bytesRead - dataStart);
                break;
            }
            pos = dataStart + chunkSize + (chunkSize & 1u);
        }
    }

    size_t sampleCount = pcmBytes / sizeof(short);
    samples.resize(sampleCount);
    memcpy(samples.data(), bytes.data() + pcmOffset, sampleCount * sizeof(short));

    sapiStream->Release();
    memoryStream->Release();
    voice->Release();
    return !samples.empty();
}

void ResampleForPitchSpeed(const std::vector<float>& in, int pitchSemitones, std::vector<float>& out) {
    if (in.empty()) {
        out.clear();
        return;
    }

    double factor = pow(2.0, static_cast<double>(pitchSemitones) / 12.0);
    if (factor <= 0.01 || fabs(factor - 1.0) < 0.001) {
        out = in;
        return;
    }

    size_t outCount = static_cast<size_t>(static_cast<double>(in.size()) / factor);
    if (outCount < 1) outCount = 1;
    out.resize(outCount);

    for (size_t i = 0; i < outCount; ++i) {
        double src = static_cast<double>(i) * factor;
        size_t idx = static_cast<size_t>(src);
        double frac = src - static_cast<double>(idx);
        if (idx + 1 < in.size()) {
            out[i] = static_cast<float>(in[idx] * (1.0 - frac) + in[idx + 1] * frac);
        } else {
            out[i] = in.back();
        }
    }
}

void HighPass(std::vector<float>& samples, float cutoffHz) {
    if (samples.empty()) return;
    float dt = 1.0f / static_cast<float>(kSampleRate);
    float rc = 1.0f / (6.2831853f * cutoffHz);
    float alpha = rc / (rc + dt);
    float prevY = 0.0f;
    float prevX = samples[0];
    for (float& x : samples) {
        float y = alpha * (prevY + x - prevX);
        prevX = x;
        prevY = y;
        x = y;
    }
}

void LowPass(std::vector<float>& samples, float cutoffHz) {
    if (samples.empty()) return;
    float dt = 1.0f / static_cast<float>(kSampleRate);
    float rc = 1.0f / (6.2831853f * cutoffHz);
    float alpha = dt / (rc + dt);
    float y = samples[0];
    for (float& x : samples) {
        y += alpha * (x - y);
        x = y;
    }
}

void ApplyRadioPreset(std::vector<short>& pcm, const VoiceConfig& cfg) {
    if (pcm.empty()) return;

    std::vector<float> signal(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        signal[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }

    std::vector<float> pitched;
    ResampleForPitchSpeed(signal, cfg.pitchSemitones, pitched);
    signal.swap(pitched);

    HighPass(signal, 180.0f);
    LowPass(signal, 3800.0f);

    float peak = 0.0f;
    for (float& sample : signal) {
        sample *= 1.35f;
        float absSample = fabsf(sample);
        if (absSample > 0.32f) {
            float compressed = 0.32f + (absSample - 0.32f) / 3.0f;
            sample = sample < 0.0f ? -compressed : compressed;
        }
        sample = tanhf(sample * 1.2f) / tanhf(1.2f);
        float absProcessed = fabsf(sample);
        if (absProcessed > peak) peak = absProcessed;
    }

    float volume = static_cast<float>(cfg.volume) / 100.0f;
    float gain = peak > 0.001f ? (0.84f / peak) * volume : volume;
    pcm.resize(signal.size());
    for (size_t i = 0; i < signal.size(); ++i) {
        float value = signal[i] * gain;
        if (value > 0.98f) value = 0.98f;
        if (value < -0.98f) value = -0.98f;
        pcm[i] = static_cast<short>(value * 32767.0f);
    }
}

void ClearCurrentWaveOut(HWAVEOUT handle) {
    std::lock_guard<std::mutex> lock(g_playbackMutex);
    if (g_currentWaveOut == handle) {
        g_currentWaveOut = nullptr;
        g_currentPriority = 0;
    }
}

bool PlayPcm(const std::vector<short>& pcm, int priority) {
    if (pcm.empty()) return false;

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign = static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HWAVEOUT waveOut = nullptr;
    MMRESULT mm = waveOutOpen(&waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR || !waveOut) {
        VoiceLog("waveOutOpen failed mm=%u\n", static_cast<unsigned>(mm));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_playbackMutex);
        g_currentWaveOut = waveOut;
        g_currentPriority = priority;
    }

    WAVEHDR header = {};
    header.lpData = reinterpret_cast<LPSTR>(const_cast<short*>(pcm.data()));
    header.dwBufferLength = static_cast<DWORD>(pcm.size() * sizeof(short));

    mm = waveOutPrepareHeader(waveOut, &header, sizeof(header));
    if (mm == MMSYSERR_NOERROR) {
        mm = waveOutWrite(waveOut, &header, sizeof(header));
    }
    if (mm != MMSYSERR_NOERROR) {
        VoiceLog("waveOutWrite failed mm=%u\n", static_cast<unsigned>(mm));
        waveOutUnprepareHeader(waveOut, &header, sizeof(header));
        ClearCurrentWaveOut(waveOut);
        waveOutClose(waveOut);
        return false;
    }

    while ((header.dwFlags & WHDR_DONE) == 0) {
        Sleep(10);
    }

    waveOutUnprepareHeader(waveOut, &header, sizeof(header));
    ClearCurrentWaveOut(waveOut);
    waveOutClose(waveOut);
    return true;
}

void WorkerMain() {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coHr)) {
        VoiceLog("CoInitializeEx failed hr=0x%08lx\n", static_cast<unsigned long>(coHr));
        return;
    }

    for (;;) {
        VoiceItem item;
        VoiceConfig cfg;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return !g_queue.empty(); });
            item = g_queue.front();
            g_queue.pop_front();
            cfg = g_config;
        }

        std::vector<short> pcm;
        if (!SynthesizePcm(item, cfg, pcm)) {
            continue;
        }
        ApplyRadioPreset(pcm, cfg);
        PlayPcm(pcm, item.priority);
    }
}

void EnsureWorkerStarted() {
    std::call_once(g_startOnce, [] {
        g_worker = std::thread(WorkerMain);
        g_worker.detach();
    });
}

void InterruptLowPriorityPlaybackLocked(int priority) {
    if (priority < g_config.interruptPriority) return;

    g_queue.erase(std::remove_if(g_queue.begin(),
                                 g_queue.end(),
                                 [](const VoiceItem& queued) {
                                     return queued.priority < g_config.interruptPriority;
                                 }),
                  g_queue.end());

    std::lock_guard<std::mutex> playbackLock(g_playbackMutex);
    if (g_currentWaveOut && g_currentPriority < g_config.interruptPriority) {
        waveOutReset(g_currentWaveOut);
    }
}

bool MarkSequenceSeenLocked(unsigned long long sequence) {
    if (sequence == 0) return true;
    if (!g_seenSequences.insert(sequence).second) return false;

    g_seenOrder.push_back(sequence);
    while (g_seenOrder.size() > kSeenSequenceLimit) {
        g_seenSequences.erase(g_seenOrder.front());
        g_seenOrder.pop_front();
    }
    return true;
}

} // namespace

void CommentaryVoice_Init() {
    VoiceConfig cfg = LoadConfig();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = cfg;
    }
    g_enabled.store(cfg.enabled);
    g_initialized.store(true);

    if (cfg.enabled) {
        EnsureWorkerStarted();
        VoiceLog("initialized enabled=1 preset=%s volume=%d rate=%d pitch=%d interrupt=%d queue_limit=%d voice=%s\n",
                 cfg.preset.c_str(),
                 cfg.volume,
                 cfg.rate,
                 cfg.pitchSemitones,
                 cfg.interruptPriority,
                 cfg.queueLimit,
                 cfg.voice.c_str());
    } else {
        VoiceLog("initialized enabled=0\n");
    }
}

void CommentaryVoice_Queue(const char* text, int priority, unsigned long long sequence) {
    if (!text || !text[0]) return;
    if (!g_initialized.load()) CommentaryVoice_Init();
    if (!g_enabled.load()) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_config.enabled) return;
    if (!MarkSequenceSeenLocked(sequence)) {
        VoiceLog("dedupe seq=%llu priority=%d text=%s\n", sequence, priority, text);
        return;
    }

    InterruptLowPriorityPlaybackLocked(priority);

    if (priority < g_config.interruptPriority
        && static_cast<int>(g_queue.size()) >= g_config.queueLimit) {
        VoiceLog("drop-backlog seq=%llu priority=%d queued=%u text=%s\n",
                 sequence,
                 priority,
                 static_cast<unsigned>(g_queue.size()),
                 text);
        return;
    }

    VoiceItem item;
    item.text = text;
    item.priority = priority;
    item.sequence = sequence;
    g_queue.push_back(item);
    VoiceLog("queued seq=%llu priority=%d queued=%u text=%s\n",
             sequence,
             priority,
             static_cast<unsigned>(g_queue.size()),
             text);
    g_cv.notify_one();
}

void CommentaryVoice_Reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
    g_seenSequences.clear();
    g_seenOrder.clear();

    std::lock_guard<std::mutex> playbackLock(g_playbackMutex);
    if (g_currentWaveOut) {
        waveOutReset(g_currentWaveOut);
    }
}
