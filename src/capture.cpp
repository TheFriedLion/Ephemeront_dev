#include "capture.h"

#include "ring.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "version.lib")

namespace {

template <class T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

WAVEFORMATEX CaptureFormat() {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = kChannels;
    f.nSamplesPerSec = SampleRate();
    f.wBitsPerSample = 32;
    f.nBlockAlign = kBytesPerFrame;
    f.nAvgBytesPerSec = SampleRate() * kBytesPerFrame;
    return f;
}

constexpr REFERENCE_TIME kBufferDuration = 2000000;  // 200 ms, in 100 ns units
constexpr DWORD kConvertFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

// ---- Process loopback activation ---------------------------------------------

class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivateHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (riid == __uuidof(IAgileObject)) {
            *out = static_cast<IAgileObject*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT activated = E_FAIL;
        IUnknown* unknown = nullptr;
        HRESULT hr = op->GetActivateResult(&activated, &unknown);
        if (SUCCEEDED(hr)) hr = activated;
        if (SUCCEEDED(hr) && unknown) hr = unknown->QueryInterface(IID_PPV_ARGS(&client_));
        SafeRelease(unknown);
        result_ = hr;
        SetEvent(done_);
        return S_OK;
    }

    HANDLE Done() const { return done_; }
    HRESULT Result() const { return result_; }
    IAudioClient* TakeClient() {
        IAudioClient* c = client_;
        client_ = nullptr;
        return c;
    }

private:
    ~ActivateHandler() {
        SafeRelease(client_);
        CloseHandle(done_);
    }
    LONG refs_ = 1;
    HANDLE done_;
    HRESULT result_ = E_FAIL;
    IAudioClient* client_ = nullptr;
};

HRESULT ActivateProcessLoopback(DWORD pid, bool include, IAudioClient** out) {
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv = {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                             handler, &op);
    if (SUCCEEDED(hr)) {
        if (WaitForSingleObject(handler->Done(), 5000) != WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            hr = handler->Result();
            if (SUCCEEDED(hr)) *out = handler->TakeClient();
        }
    }
    SafeRelease(op);
    handler->Release();
    return hr;
}

// Activates a process-loopback client and initializes it in our format.
HRESULT OpenProcessLoopback(DWORD pid, bool include, IAudioClient** out) {
    WAVEFORMATEX fmt = CaptureFormat();
    HRESULT hr = E_FAIL;
    for (DWORD extra : {kConvertFlags, 0ul}) {
        IAudioClient* client = nullptr;
        hr = ActivateProcessLoopback(pid, include, &client);
        if (FAILED(hr)) return hr;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extra,
                                kBufferDuration, 0, &fmt, nullptr);
        if (SUCCEEDED(hr)) {
            *out = client;
            return hr;
        }
        SafeRelease(client);
    }
    return hr;
}

HRESULT OpenDevice(EDataFlow flow, bool loopback, IAudioClient** out) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) {
        WAVEFORMATEX fmt = CaptureFormat();
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | kConvertFlags | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &fmt, nullptr);
    }
    if (SUCCEEDED(hr)) {
        *out = client;
        client = nullptr;
    }
    SafeRelease(client);
    SafeRelease(device);
    SafeRelease(enumerator);
    return hr;
}

std::wstring DefaultDeviceId(EDataFlow flow) {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    std::wstring id;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device))) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(device->GetId(&raw))) id = raw;
        CoTaskMemFree(raw);
    }
    SafeRelease(device);
    SafeRelease(enumerator);
    return id;
}

// ---- Source -----------------------------------------------------------------

enum class Kind { System, Mic, App };

std::atomic<uint64_t> g_nextSourceId{1};

class Source {
public:
    Source(Kind kind, DWORD pid, std::wstring name, int seconds)
        : id(g_nextSourceId++), kind(kind), pid(pid), name(std::move(name)), ring(seconds),
          stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        thread_ = std::thread([this] { Run(); });
    }
    ~Source() {
        SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        CloseHandle(stop_);
        CloseHandle(wake_);
    }

    const uint64_t id;
    const Kind kind;
    const DWORD pid;
    const std::wstring name;
    RingBuffer ring;

    bool Finished() const { return finished_; }
    void SetMonoInput(int input) { monoInput_ = input; }
    bool EverOpened() const { return everOpened_; }
    void Reopen() {
        reopen_ = true;
        SetEvent(wake_);
    }

private:
    HRESULT Open(IAudioClient** client) {
        switch (kind) {
            case Kind::App:
                return OpenProcessLoopback(pid, true, client);
            case Kind::Mic:
                return OpenDevice(eCapture, false, client);
            case Kind::System:
                // Everything except Ephemeront itself, so picker previews never get recorded.
                if (!deviceLoopback_) {
                    HRESULT hr = OpenProcessLoopback(GetCurrentProcessId(), false, client);
                    if (SUCCEEDED(hr)) return hr;
                    Log(L"System: process loopback unavailable (0x%08X), using device loopback", hr);
                    deviceLoopback_ = true;
                }
                return OpenDevice(eRender, true, client);
        }
        return E_UNEXPECTED;
    }

    HRESULT Drain(IAudioCaptureClient* capture) {
        UINT32 packet = 0;
        HRESULT hr;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 qpc = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
            if (FAILED(hr)) return hr;
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            bool timed = qpc != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR);
            int64_t at = timed ? QpcToFrame(qpc) : NowFrame() - frames;
            // The first packet of a stream is routinely flagged; only later ones mean loss.
            bool lost = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && packets_ > 0;
            if (lost && ++losses_ <= 5) Log(L"%s: audio was lost before it could be collected", name.c_str());
            ++packets_;
            const float* pcm = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<float*>(data);
            if (pcm && kind == Kind::Mic && monoInput_ > 0) {
                // A mono mic on an interface arrives on one side only; copy it to both.
                const int side = monoInput_ - 1;
                mono_.resize(static_cast<size_t>(frames) * kChannels);
                for (UINT32 i = 0; i < frames; ++i) mono_[i * 2] = mono_[i * 2 + 1] = pcm[i * 2 + side];
                pcm = mono_.data();
            }
            int64_t jump = ring.Write(at, pcm, frames, lost);
            if (jump && packets_ > 1 && ++realigns_ <= 20) {
                Log(L"%s: realigned by %.1f ms", name.c_str(), jump * 1000.0 / SampleRate());
            }
            capture->ReleaseBuffer(frames);
        }
        return hr;
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HANDLE process = nullptr;
        if (kind == Kind::App) {
            process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (!process) Log(L"%s: can't watch process %lu (error %lu)", name.c_str(), pid, GetLastError());
        }
        HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        int failures = 0;
        bool quit = kind == Kind::App && !process;

        while (!quit) {
            IAudioClient* client = nullptr;
            IAudioCaptureClient* capture = nullptr;
            reopen_ = false;
            HRESULT hr = Open(&client);
            if (SUCCEEDED(hr)) hr = client->SetEventHandle(audioEvent);
            if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
            if (SUCCEEDED(hr)) hr = client->Start();

            if (FAILED(hr)) {
                Log(L"%s: open failed (0x%08X)", name.c_str(), hr);
                SafeRelease(capture);
                SafeRelease(client);
                if (kind == Kind::App && ++failures >= 3) break;
                HANDLE waits[] = {stop_, wake_};
                if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) break;
                continue;
            }

            Log(L"%s: capturing", name.c_str());
            packets_ = 0;
            failures = 0;
            everOpened_ = true;
            // Collect every packet as soon as it's ready. Process loopback ignores the
            // requested buffer size and only holds about 30 ms, so batching loses audio.
            HANDLE waits[] = {stop_, wake_, audioEvent, process};
            DWORD count = process ? 4 : 3;
            bool error = false;
            for (;;) {
                DWORD w = WaitForMultipleObjects(count, waits, FALSE, 100);
                if (w == WAIT_OBJECT_0) {
                    quit = true;
                    break;
                }
                if (FAILED(Drain(capture))) {
                    Log(L"%s: stream lost, reopening", name.c_str());
                    error = true;
                    break;
                }
                if (w == WAIT_OBJECT_0 + 3) {
                    Log(L"%s: process exited", name.c_str());
                    quit = true;
                    break;
                }
                if (reopen_) break;
            }

            client->Stop();
            SafeRelease(capture);
            SafeRelease(client);
            if (error && !quit && WaitForSingleObject(stop_, 1000) == WAIT_OBJECT_0) quit = true;
        }

        CloseHandle(audioEvent);
        if (process) CloseHandle(process);
        CoUninitialize();
        finished_ = true;
    }

    HANDLE stop_;
    HANDLE wake_;
    std::thread thread_;
    std::atomic<bool> finished_{false};
    std::atomic<bool> everOpened_{false};
    std::atomic<bool> reopen_{false};
    bool deviceLoopback_ = false;
    int64_t packets_ = 0;
    std::atomic<int> monoInput_{0};
    std::vector<float> mono_;
    int losses_ = 0;
    int realigns_ = 0;
};

// ---- Session discovery ------------------------------------------------------

// Wakes the manager the moment any app opens a new audio session.
class SessionWatcher final : public IAudioSessionNotification {
public:
    explicit SessionWatcher(HANDLE wake) : wake_(wake) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
            *out = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }
    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override {
        SetEvent(wake_);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    HANDLE wake_;
};

struct Watch {
    std::wstring deviceId;
    IAudioSessionManager2* manager = nullptr;
    SessionWatcher* watcher = nullptr;
};

struct ProcInfo {
    DWORD parent;
    std::wstring exe;
};

std::map<DWORD, ProcInfo> SnapshotProcesses() {
    std::map<DWORD, ProcInfo> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        procs[pe.th32ProcessID] = {pe.th32ParentProcessID, pe.szExeFile};
    }
    CloseHandle(snap);
    return procs;
}

// Browsers and chat apps play audio from helper processes. Walk up to the
// topmost process with the same executable so the whole app is one source.
DWORD RootOf(DWORD pid, const std::map<DWORD, ProcInfo>& procs) {
    for (int steps = 0; steps < 32; ++steps) {
        auto self = procs.find(pid);
        if (self == procs.end()) break;
        auto parent = procs.find(self->second.parent);
        if (parent == procs.end() || parent->first == pid ||
            _wcsicmp(parent->second.exe.c_str(), self->second.exe.c_str()) != 0)
            break;
        pid = parent->first;
    }
    return pid;
}

std::wstring FriendlyName(DWORD pid, const std::map<DWORD, ProcInfo>& procs) {
    std::wstring stem;
    auto it = procs.find(pid);
    if (it != procs.end()) {
        stem = it->second.exe;
        size_t dot = stem.rfind(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);
    }

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return stem.empty() ? L"App " + std::to_wstring(pid) : stem;
    wchar_t path[MAX_PATH * 2];
    DWORD len = _countof(path);
    BOOL gotPath = QueryFullProcessImageNameW(proc, 0, path, &len);
    CloseHandle(proc);
    if (!gotPath) return stem;

    DWORD size = GetFileVersionInfoSizeW(path, nullptr);
    if (!size) return stem;
    std::vector<BYTE> info(size);
    if (!GetFileVersionInfoW(path, 0, size, info.data())) return stem;
    struct LangCodePage {
        WORD lang, codepage;
    }* langs = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &bytes) ||
        bytes < sizeof(LangCodePage))
        return stem;
    wchar_t query[64];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].codepage);
    wchar_t* desc = nullptr;
    if (VerQueryValueW(info.data(), query, reinterpret_cast<void**>(&desc), &bytes) && desc && bytes > 1) {
        std::wstring d = desc;
        while (!d.empty() && d.back() == L' ') d.pop_back();
        if (!d.empty() && d.size() <= 40) return d;
    }
    return stem;
}

// ---- Manager ----------------------------------------------------------------

std::mutex g_configMutex;
Capture::Config g_config;

std::mutex g_sourcesMutex;
std::vector<std::shared_ptr<Source>> g_sources;

std::thread g_manager;
HANDLE g_stop = nullptr;
HANDLE g_wake = nullptr;

struct ManagerState {
    IMMDeviceEnumerator* enumerator = nullptr;
    std::vector<Watch> watches;
    std::set<DWORD> failedApps;
    int bufferSeconds = -1;
    std::wstring renderDevice, captureDevice;
};

std::set<DWORD> AppsWithSessions(ManagerState& st) {
    std::set<DWORD> pids;
    IMMDeviceCollection* devices = nullptr;
    if (FAILED(st.enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return pids;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(i, &device))) continue;
        LPWSTR rawId = nullptr;
        device->GetId(&rawId);
        std::wstring id = rawId ? rawId : L"";
        CoTaskMemFree(rawId);

        auto watch = std::find_if(st.watches.begin(), st.watches.end(), [&](const Watch& w) { return w.deviceId == id; });
        if (watch == st.watches.end()) {
            Watch w;
            w.deviceId = id;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&w.manager)))) {
                w.watcher = new SessionWatcher(g_wake);
                w.manager->RegisterSessionNotification(w.watcher);
            }
            st.watches.push_back(w);
            watch = st.watches.end() - 1;
        }

        IAudioSessionEnumerator* sessions = nullptr;
        if (watch->manager && SUCCEEDED(watch->manager->GetSessionEnumerator(&sessions))) {
            int n = 0;
            sessions->GetCount(&n);
            for (int s = 0; s < n; ++s) {
                IAudioSessionControl* control = nullptr;
                IAudioSessionControl2* control2 = nullptr;
                if (SUCCEEDED(sessions->GetSession(s, &control)) &&
                    SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2)))) {
                    AudioSessionState state = AudioSessionStateExpired;
                    DWORD pid = 0;
                    control2->GetState(&state);
                    control2->GetProcessId(&pid);
                    if (control2->IsSystemSoundsSession() != S_OK && state != AudioSessionStateExpired && pid != 0)
                        pids.insert(pid);
                }
                SafeRelease(control2);
                SafeRelease(control);
            }
            SafeRelease(sessions);
        }
        SafeRelease(device);
    }
    SafeRelease(devices);
    return pids;
}

void Scan(ManagerState& st) {
    Capture::Config cfg;
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        cfg = g_config;
    }
    std::vector<std::shared_ptr<Source>> current;
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        current = g_sources;
    }
    if (cfg.bufferSeconds != st.bufferSeconds) {
        current.clear();
        st.bufferSeconds = cfg.bufferSeconds;
    }

    auto find = [&](Kind kind, DWORD pid) -> std::shared_ptr<Source> {
        for (auto& s : current) {
            if (s->kind == kind && s->pid == pid) return s;
        }
        return nullptr;
    };

    std::vector<std::shared_ptr<Source>> next;
    std::wstring render = DefaultDeviceId(eRender);
    std::wstring capture = DefaultDeviceId(eCapture);

    if (cfg.system) {
        auto s = find(Kind::System, 0);
        if (!s) s = std::make_shared<Source>(Kind::System, 0, L"System", cfg.bufferSeconds);
        else if (render != st.renderDevice) s->Reopen();
        next.push_back(s);
    }
    if (cfg.mic) {
        auto s = find(Kind::Mic, 0);
        if (!s) s = std::make_shared<Source>(Kind::Mic, 0, L"Microphone", cfg.bufferSeconds);
        else if (capture != st.captureDevice) s->Reopen();
        s->SetMonoInput(cfg.micMono);
        next.push_back(s);
    }
    st.renderDevice = render;
    st.captureDevice = capture;

    if (cfg.apps) {
        const int64_t horizon = NowFrame() - static_cast<int64_t>(cfg.bufferSeconds) * SampleRate();
        std::set<DWORD> live;
        for (auto& s : current) {
            if (s->kind != Kind::App) continue;
            if (s->Finished() && !s->EverOpened()) st.failedApps.insert(s->pid);
            // Keep a closed app's audio until it ages out of the buffer.
            if (!s->Finished() || s->ring.LastSoundFrame() > horizon) {
                next.push_back(s);
                if (!s->Finished()) live.insert(s->pid);
            }
        }

        auto procs = SnapshotProcesses();
        const DWORD self = GetCurrentProcessId();
        for (DWORD pid : AppsWithSessions(st)) {
            DWORD root = RootOf(pid, procs);
            if (root == self || live.count(root) || st.failedApps.count(root)) continue;
            live.insert(root);
            std::wstring name = FriendlyName(root, procs);
            Log(L"New app source: %s (pid %lu)", name.c_str(), root);
            next.push_back(std::make_shared<Source>(Kind::App, root, name, cfg.bufferSeconds));
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        g_sources.swap(next);
    }
    // Dropped sources are destroyed here, outside the lock, as `next` and `current` go away.
}

void ManagerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ManagerState st;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&st.enumerator));

    HANDLE waits[] = {g_stop, g_wake};
    do {
        if (st.enumerator) Scan(st);
    } while (WaitForMultipleObjects(2, waits, FALSE, 2000) != WAIT_OBJECT_0);

    for (Watch& w : st.watches) {
        if (w.manager && w.watcher) w.manager->UnregisterSessionNotification(w.watcher);
        SafeRelease(w.watcher);
        SafeRelease(w.manager);
    }
    std::vector<std::shared_ptr<Source>> dying;
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        dying.swap(g_sources);
    }
    dying.clear();
    SafeRelease(st.enumerator);
    CoUninitialize();
}

int KindOrder(Kind k) { return k == Kind::System ? 0 : k == Kind::Mic ? 1 : 2; }

std::vector<std::shared_ptr<Source>> SortedSources() {
    std::vector<std::shared_ptr<Source>> sources;
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        sources = g_sources;
    }
    std::stable_sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
        if (KindOrder(a->kind) != KindOrder(b->kind)) return KindOrder(a->kind) < KindOrder(b->kind);
        return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    return sources;
}

}  // namespace

namespace Capture {

int DeviceMixRate() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    WAVEFORMATEX* mix = nullptr;
    int rate = 48000;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) &&
        SUCCEEDED(client->GetMixFormat(&mix)) && mix->nSamplesPerSec >= 8000) {
        rate = static_cast<int>(mix->nSamplesPerSec);
    }
    CoTaskMemFree(mix);
    SafeRelease(client);
    SafeRelease(device);
    SafeRelease(enumerator);
    return rate;
}

void Start(const Config& config) {
    Configure(config);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_manager = std::thread(ManagerMain);
}

void Stop() {
    if (!g_manager.joinable()) return;
    SetEvent(g_stop);
    g_manager.join();
    CloseHandle(g_stop);
    CloseHandle(g_wake);
}

void Configure(const Config& config) {
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = config;
    }
    if (g_wake) SetEvent(g_wake);
}

std::vector<SourceInfo> Sources() {
    std::vector<SourceInfo> out;
    for (auto& s : SortedSources()) out.push_back({s->id, s->name, s->ring.LastSoundFrame()});
    return out;
}

bool Read(uint64_t id, int64_t start, int64_t frames, std::vector<float>& out) {
    std::shared_ptr<Source> source;
    {
        std::lock_guard<std::mutex> lock(g_sourcesMutex);
        for (auto& s : g_sources) {
            if (s->id == id) source = s;
        }
    }
    if (!source) return false;
    out.resize(static_cast<size_t>(frames) * kChannels);
    source->ring.Read(start, frames, out.data());
    return true;
}

std::vector<Clip> Grab(int64_t start, int64_t frames, bool dropSilent) {
    std::vector<Clip> clips;
    for (auto& s : SortedSources()) {
        if (dropSilent && s->ring.LastSoundFrame() <= start) continue;
        Clip clip;
        clip.id = s->id;
        clip.name = s->name;
        clip.pcm.resize(static_cast<size_t>(frames) * kChannels);
        s->ring.Read(start, frames, clip.pcm.data());
        if (dropSilent && PeakOf(clip.pcm.data(), clip.pcm.size()) <= kSilentPeak) continue;
        clips.push_back(std::move(clip));
    }
    return clips;
}

void Stats(int* sourceCount, size_t* bytes) {
    std::lock_guard<std::mutex> lock(g_sourcesMutex);
    *sourceCount = static_cast<int>(g_sources.size());
    *bytes = 0;
    for (auto& s : g_sources) *bytes += s->ring.BytesInUse();
}

}  // namespace Capture
