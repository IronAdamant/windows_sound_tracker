#include "SoundTracker.h"

#include <psapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "ComPtr.h"
#include "CoreAudioCompat.h"
#include "DebugLog.h"
#include "DeviceWatcher.h"
#include "Logger.h"
#include "ProcessInfo.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// Defined locally to avoid initguid.h ordering issues across toolchains.
const PROPERTYKEY kPkeyDeviceFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

constexpr size_t kMaxEvents = 5000;
constexpr TimeMs kRescanIntervalMs = 2000;
constexpr TimeMs kLivePublishMs = 100;
constexpr TimeMs kWindowRefreshMs = 2000;

const wchar_t* StateName(AudioSessionState state) {
    switch (state) {
        case AudioSessionStateInactive: return L"inactive";
        case AudioSessionStateActive: return L"active";
        case AudioSessionStateExpired: return L"expired";
    }
    return L"?";
}

const wchar_t* DisconnectReasonName(AudioSessionDisconnectReason reason) {
    switch (reason) {
        case DisconnectReasonDeviceRemoval: return L"device removed";
        case DisconnectReasonServerShutdown: return L"audio service shut down";
        case DisconnectReasonFormatChanged: return L"format changed";
        case DisconnectReasonSessionLogoff: return L"user logged off";
        case DisconnectReasonSessionDisconnected: return L"remote desktop session disconnected";
        case DisconnectReasonExclusiveModeOverride: return L"exclusive mode override";
    }
    return L"unknown";
}

struct RateLimit {
    TimeMs last = -1;
    bool Allow(TimeMs now, TimeMs intervalMs) {
        if (last >= 0 && now - last < intervalMs) return false;
        last = now;
        return true;
    }
};

// State shared with COM callbacks. Callbacks can outlive the tracker (Core Audio
// may hold references briefly after unregistering), so they keep a shared_ptr
// to this and never touch the tracker itself.
struct WakeSignal {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> rescan{false};
    ~WakeSignal() {
        if (event) CloseHandle(event);
    }
    void RequestRescan() {
        rescan = true;
        SetEvent(event);
    }
};

template <class Interface>
class CallbackBase : public Interface {
public:
    explicit CallbackBase(std::shared_ptr<WakeSignal> signal) : m_signal(std::move(signal)) {}
    virtual ~CallbackBase() = default;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ref = InterlockedDecrement(&m_ref);
        if (ref == 0) delete this;
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }

protected:
    std::shared_ptr<WakeSignal> m_signal;

private:
    LONG m_ref = 1;
};

class SessionEvents : public CallbackBase<IAudioSessionEvents> {
public:
    SessionEvents(std::shared_ptr<WakeSignal> signal, std::wstring label)
        : CallbackBase(std::move(signal)), m_label(std::move(label)) {}

    std::atomic<bool> disconnected{false};
    std::atomic<bool> expired{false};

    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState state) override {
        LOG_DEBUG("session", m_label << L" state -> " << StateName(state));
        if (state == AudioSessionStateExpired) {
            expired = true;
            m_signal->RequestRescan();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override {
        LOG_INFO("session", m_label << L" disconnected: " << DisconnectReasonName(reason));
        disconnected = true;
        m_signal->RequestRescan();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float volume, BOOL mute, LPCGUID) override {
        LOG_DEBUG("session", m_label << L" volume -> " << dbg::Pct(volume) << (mute ? L" (muted)" : L""));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }

private:
    std::wstring m_label;
};

class SessionCreatedNotifier : public CallbackBase<IAudioSessionNotification> {
public:
    SessionCreatedNotifier(std::shared_ptr<WakeSignal> signal, std::wstring deviceName)
        : CallbackBase(std::move(signal)), m_deviceName(std::move(deviceName)) {}
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
        LOG_DEBUG("session", L"New session notification on '" << m_deviceName << L"'");
        m_signal->RequestRescan();
        return S_OK;
    }

private:
    std::wstring m_deviceName;
};

class DeviceNotifier : public CallbackBase<IMMNotificationClient> {
public:
    using CallbackBase::CallbackBase;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        LOG_INFO("device", L"Audio endpoint state changed to " << state << L": " << (id ? id : L"?"));
        m_signal->RequestRescan();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        LOG_INFO("device", L"Audio endpoint added: " << (id ? id : L"?"));
        m_signal->RequestRescan();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        LOG_INFO("device", L"Audio endpoint removed: " << (id ? id : L"?"));
        m_signal->RequestRescan();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        if (flow == eRender && role == eConsole) {
            LOG_INFO("device", L"Default output device changed: " << (id ? id : L"(none)"));
            m_signal->RequestRescan();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

std::wstring ResolveIndirectString(const std::wstring& value) {
    if (value.empty() || value[0] != L'@') return value;
    wchar_t expanded[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(value.c_str(), expanded, MAX_PATH * 2);
    wchar_t out[512] = {};
    if (SUCCEEDED(SHLoadIndirectString(expanded, out, 512, nullptr))) return out;
    return value;
}

std::wstring DescribeProcess(const std::wstring& name, bool isSystem) {
    if (isSystem) return L"Windows system sounds";
    struct Known { const wchar_t* exe; const wchar_t* description; };
    static const Known known[] = {
        {L"chrome.exe", L"Google Chrome"},
        {L"firefox.exe", L"Mozilla Firefox"},
        {L"msedge.exe", L"Microsoft Edge"},
        {L"brave.exe", L"Brave Browser"},
        {L"opera.exe", L"Opera"},
        {L"vivaldi.exe", L"Vivaldi"},
        {L"msedgewebview2.exe", L"Embedded web content (WebView2)"},
        {L"Discord.exe", L"Discord voice/notification"},
        {L"Teams.exe", L"Microsoft Teams"},
        {L"ms-teams.exe", L"Microsoft Teams"},
        {L"Spotify.exe", L"Spotify"},
        {L"slack.exe", L"Slack notification"},
        {L"outlook.exe", L"Outlook notification"},
        {L"olk.exe", L"Outlook notification"},
        {L"explorer.exe", L"Windows Explorer"},
        {L"svchost.exe", L"Windows service"},
        {L"ShellExperienceHost.exe", L"Windows shell"},
        {L"SystemSettings.exe", L"Windows Settings"},
        {L"TextInputHost.exe", L"Windows text input (keyboard)"},
        {L"Wow.exe", L"World of Warcraft"},
        {L"WowClassic.exe", L"World of Warcraft Classic"},
        {L"WowT.exe", L"World of Warcraft (PTR)"},
        {L"WowB.exe", L"World of Warcraft (beta)"},
        {L"WowVoiceProxy.exe", L"World of Warcraft voice chat"},
        {L"Battle.net.exe", L"Battle.net launcher"},
        {L"steam.exe", L"Steam"},
        {L"SoundBeacon.exe", L"Sound Tracker test beacon"},
    };
    for (const auto& k : known) {
        if (_wcsicmp(name.c_str(), k.exe) == 0) return k.description;
    }
    return name + L" audio";
}

}  // namespace

struct SoundTracker::Impl {
    struct RuleBinding {
        size_t ruleIndex = 0;
        AlertEvaluator evaluator;
        uint64_t fired = 0;
        RateLimit infoLimit;
    };

    struct TrackedSession {
        std::wstring key;
        std::wstring endpointId;
        std::wstring deviceName;
        ComPtr<IAudioSessionControl2> control;
        ComPtr<IAudioMeterInformation> meter;
        ComPtr<ISimpleAudioVolume> volume;
        SessionEvents* events = nullptr;
        bool registered = false;

        DWORD pid = 0;
        bool isSystem = false;
        std::wstring processName, processPath, description, displayName, windowTitle;
        DWORD windowOwnerPid = 0;
        TimeMs lastWindowRefresh = -1;

        SoundSegmenter segmenter;
        uint64_t currentEventId = 0;
        TimeMs lastOngoingUpdate = -1;
        std::vector<RuleBinding> bindings;

        bool seen = true;
        bool invalid = false;
        int meterFailures = 0;
        float lastPeak = 0.0f;
        float maxA = 0.0f, maxB = 0.0f;
        TimeMs maxWindowStart = 0;
        uint64_t sounds = 0;
        uint64_t alerts = 0;

        std::wstring Label() const { return processName + L" PID " + std::to_wstring(pid); }

        ~TrackedSession() {
            if (events) {
                if (registered && control) control->UnregisterAudioSessionNotification(events);
                events->Release();
            }
        }
    };

    struct Endpoint {
        std::wstring id;
        std::wstring name;
        bool isDefault = false;
        ComPtr<IAudioSessionManager2> manager;
        SessionCreatedNotifier* notifier = nullptr;
        bool seen = true;
        ~Endpoint() {
            if (notifier) {
                if (manager) manager->UnregisterSessionNotification(notifier);
                notifier->Release();
            }
        }
    };

    explicit Impl(std::wstring logsDir) : logsDirectory(std::move(logsDir)) {}

    // ---- Shared state -------------------------------------------------------
    std::wstring logsDirectory;
    std::atomic<bool> running{false};
    std::thread thread;
    std::shared_ptr<WakeSignal> signal = std::make_shared<WakeSignal>();
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point startTime = std::chrono::system_clock::now();
    AlertCallback onAlert;

    std::mutex configMutex;
    std::vector<AlertRule> pendingRules;
    bool rulesDirty = true;
    DetectionSettings pendingDetection;
    bool detectionDirty = true;
    std::atomic<bool> recordWindowTitles{true};

    mutable std::mutex eventsMutex;
    std::deque<AudioEvent> events;
    uint64_t nextEventId = 1;
    std::atomic<uint64_t> eventsVersion{0};
    std::atomic<uint64_t> soundCount{0};
    std::atomic<uint64_t> alertCount{0};

    mutable std::mutex liveMutex;
    std::vector<LiveSessionInfo> live;

    mutable std::mutex csvMutex;
    std::unique_ptr<Logger> csv;
    DeviceWatcher devices;

    // ---- Monitor-thread-only state -----------------------------------------
    std::vector<AlertRule> rules;
    DetectionSettings detection;
    ComPtr<IMMDeviceEnumerator> enumerator;
    DeviceNotifier* deviceNotifier = nullptr;
    std::unordered_map<std::wstring, std::unique_ptr<Endpoint>> endpoints;
    std::unordered_map<std::wstring, std::unique_ptr<TrackedSession>> sessions;
    std::wstring defaultEndpointId;

    DWORD lastInputTick = 0;
    bool haveInputTick = false;
    TimeMs lastAnyInput = -1;
    std::unordered_map<DWORD, TimeMs> lastInputByPid;
    DWORD lastForegroundPid = 0;

    TimeMs lastLivePublish = -1;
    TimeMs lastTick = -1;
    TimeMs statsStart = 0;
    uint64_t statTicks = 0;
    TimeMs statSumInterval = 0;
    TimeMs statMaxInterval = 0;
    uint64_t statLateTicks = 0;
    TimeMs lastInfoHeartbeat = 0;
    RateLimit stallLimit, enumLimit;

    TimeMs Now() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - epoch).count();
    }

    void Run();
    void ApplyConfig();
    void Rescan(TimeMs now);
    Endpoint* AddEndpoint(IMMDevice* device, const std::wstring& id);
    bool EnumerateSessions(Endpoint& endpoint);
    std::unique_ptr<TrackedSession> CreateSession(ComPtr<IAudioSessionControl2> control, const std::wstring& key, const Endpoint& endpoint);
    void BindRules(TrackedSession& s);
    void RefreshWindow(TrackedSession& s, TimeMs now, bool force);
    void PollInput(TimeMs now);
    void PollMeters(TimeMs now);
    void StartSound(TrackedSession& s, TimeMs now);
    void UpdateOngoing(TrackedSession& s, TimeMs now);
    void EndSound(TrackedSession& s);
    void EvaluateRule(TrackedSession& s, RuleBinding& b, TimeMs now, float peak);
    void PublishLive(TimeMs now);
    void UpdateStats(TimeMs now);
    TimeMs LastInputFor(const TrackedSession& s, const AlertRule& rule) const;
    AudioEvent* FindEventLocked(uint64_t id);
};

static AlertParams ToParams(const AlertRule& r) {
    AlertParams p;
    p.threshold = r.thresholdPercent / 100.0f;
    p.quietBeforeMs = r.quietBeforeMs;
    p.minLoudMs = r.minLoudMs;
    p.cooldownMs = r.cooldownMs;
    p.suppressAfterInputMs = r.suppressAfterInputMs;
    return p;
}

// ---- Public API ---------------------------------------------------------------

SoundTracker::SoundTracker(std::wstring logsDirectory) : m(std::make_unique<Impl>(std::move(logsDirectory))) {}

SoundTracker::~SoundTracker() { Stop(); }

void SoundTracker::Start() {
    if (m->running) return;
    if (m->thread.joinable()) m->thread.join();
    {
        std::lock_guard<std::mutex> lock(m->eventsMutex);
        m->events.clear();
    }
    m->soundCount = 0;
    m->alertCount = 0;
    ++m->eventsVersion;
    {
        std::lock_guard<std::mutex> lock(m->csvMutex);
        m->csv = std::make_unique<Logger>(m->logsDirectory);
        m->csv->Initialize();
    }
    m->devices.Start();
    m->startTime = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> lock(m->configMutex);
        m->rulesDirty = true;
        m->detectionDirty = true;
    }
    m->running = true;
    LOG_INFO("tracker", L"Tracking started");
    m->thread = std::thread([this] { m->Run(); });
}

void SoundTracker::Stop() {
    bool wasRunning = m->running.exchange(false);
    SetEvent(m->signal->event);
    if (m->thread.joinable()) m->thread.join();
    m->devices.Stop();
    {
        std::lock_guard<std::mutex> lock(m->csvMutex);
        if (m->csv) m->csv->Close();
    }
    {
        std::lock_guard<std::mutex> lock(m->liveMutex);
        m->live.clear();
    }
    if (wasRunning) {
        LOG_INFO("tracker", L"Tracking stopped: " << m->soundCount.load() << L" sounds, " << m->alertCount.load() << L" alerts");
    }
}

bool SoundTracker::IsRunning() const { return m->running; }

void SoundTracker::SetAlertCallback(AlertCallback callback) { m->onAlert = std::move(callback); }

void SoundTracker::SetRules(const std::vector<AlertRule>& rules) {
    std::lock_guard<std::mutex> lock(m->configMutex);
    m->pendingRules = rules;
    m->rulesDirty = true;
}

void SoundTracker::SetDetection(const DetectionSettings& detection) {
    std::lock_guard<std::mutex> lock(m->configMutex);
    m->pendingDetection = detection;
    m->detectionDirty = true;
}

void SoundTracker::SetRecordWindowTitles(bool record) { m->recordWindowTitles = record; }

std::vector<AudioEvent> SoundTracker::GetEventsSnapshot(uint64_t afterId, size_t maxCount) const {
    std::lock_guard<std::mutex> lock(m->eventsMutex);
    std::vector<AudioEvent> out;
    size_t begin = m->events.size() > maxCount ? m->events.size() - maxCount : 0;
    for (size_t i = begin; i < m->events.size(); ++i) {
        if (m->events[i].id > afterId) out.push_back(m->events[i]);
    }
    return out;
}

uint64_t SoundTracker::GetEventsVersion() const { return m->eventsVersion; }

uint64_t SoundTracker::GetLastEventId() const {
    std::lock_guard<std::mutex> lock(m->eventsMutex);
    return m->events.empty() ? 0 : m->events.back().id;
}

uint64_t SoundTracker::GetSoundCount() const { return m->soundCount; }
uint64_t SoundTracker::GetAlertCount() const { return m->alertCount; }

std::vector<LiveSessionInfo> SoundTracker::GetLiveSessions() const {
    std::lock_guard<std::mutex> lock(m->liveMutex);
    return m->live;
}

std::chrono::system_clock::time_point SoundTracker::GetStartTime() const { return m->startTime; }

std::wstring SoundTracker::GetCurrentLogPath() const {
    std::lock_guard<std::mutex> lock(m->csvMutex);
    return m->csv ? m->csv->GetCurrentLogPath() : std::wstring();
}

// ---- Monitor thread -------------------------------------------------------------

void SoundTracker::Impl::Run() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("tracker", L"CoInitializeEx on monitor thread failed: " << dbg::HrToString(hr));
        running = false;
        return;
    }
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(enumerator.Put()));
    if (FAILED(hr)) {
        LOG_ERROR("tracker", L"Cannot create MMDeviceEnumerator: " << dbg::HrToString(hr) << L" - is the Windows Audio service running?");
        running = false;
        CoUninitialize();
        return;
    }
    deviceNotifier = new DeviceNotifier(signal);
    hr = enumerator->RegisterEndpointNotificationCallback(deviceNotifier);
    if (FAILED(hr)) LOG_WARN("tracker", L"RegisterEndpointNotificationCallback failed: " << dbg::HrToString(hr));

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool highResolution = timer != nullptr;
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    ApplyConfig();
    LOG_INFO("tracker", L"Monitor thread running: meter interval " << detection.meterIntervalMs << L" ms, "
                            << (highResolution ? L"high-resolution timer" : L"standard timer (coarser timing)"));

    TimeMs now = Now();
    statsStart = now;
    lastInfoHeartbeat = now;
    Rescan(now);
    TimeMs nextRescan = now + kRescanIntervalMs;

    while (running) {
        now = Now();
        ApplyConfig();
        if (signal->rescan.exchange(false) || now >= nextRescan) {
            Rescan(now);
            nextRescan = now + kRescanIntervalMs;
        }
        PollInput(now);
        PollMeters(now);
        PublishLive(now);
        UpdateStats(now);

        HANDLE handles[2] = {signal->event, timer};
        DWORD count = 1;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(detection.meterIntervalMs) * 10000;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) count = 2;
        }
        WaitForMultipleObjects(count, handles, FALSE, static_cast<DWORD>(detection.meterIntervalMs) * 4);
    }

    for (auto& entry : sessions) EndSound(*entry.second);
    sessions.clear();
    endpoints.clear();
    if (deviceNotifier) {
        enumerator->UnregisterEndpointNotificationCallback(deviceNotifier);
        deviceNotifier->Release();
        deviceNotifier = nullptr;
    }
    enumerator.Reset();
    if (timer) CloseHandle(timer);
    CoUninitialize();
    LOG_DEBUG("tracker", L"Monitor thread exited");
}

void SoundTracker::Impl::ApplyConfig() {
    bool rebind = false;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        if (detectionDirty) {
            detection = pendingDetection;
            detectionDirty = false;
            LOG_DEBUG("tracker", L"Detection: sound threshold " << dbg::Pct(detection.soundThreshold) << L", release "
                                     << detection.soundReleaseMs << L" ms, meter interval " << detection.meterIntervalMs << L" ms");
        }
        if (rulesDirty) {
            rules = pendingRules;
            rulesDirty = false;
            rebind = true;
        }
    }
    if (rebind) {
        LOG_INFO("alerts", L"Alert rules applied (" << rules.size() << L"):");
        for (size_t i = 0; i < rules.size(); ++i) LOG_INFO("alerts", L"  rule[" << i << L"] " << rules[i].Describe());
        for (auto& entry : sessions) BindRules(*entry.second);
    }
}

void SoundTracker::Impl::Rescan(TimeMs now) {
    for (auto& e : endpoints) e.second->seen = false;
    for (auto& s : sessions) s.second->seen = false;
    bool complete = true;

    ComPtr<IMMDevice> defaultDevice;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, defaultDevice.Put()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.Put());
    if (FAILED(hr)) {
        if (enumLimit.Allow(now, 60000)) LOG_WARN("tracker", L"EnumAudioEndpoints failed: " << dbg::HrToString(hr));
        complete = false;
    } else {
        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) complete = false;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.Put()))) {
                complete = false;
                continue;
            }
            LPWSTR rawId = nullptr;
            if (FAILED(device->GetId(&rawId)) || !rawId) {
                complete = false;
                continue;
            }
            std::wstring id = rawId;
            CoTaskMemFree(rawId);

            Endpoint* endpoint = nullptr;
            auto it = endpoints.find(id);
            if (it != endpoints.end()) {
                endpoint = it->second.get();
            } else {
                endpoint = AddEndpoint(device.Get(), id);
            }
            if (!endpoint) {
                complete = false;
                continue;
            }
            endpoint->seen = true;
            endpoint->isDefault = (id == defaultId);
            if (!EnumerateSessions(*endpoint)) complete = false;
        }
    }
    if (defaultId != defaultEndpointId) {
        auto it = endpoints.find(defaultId);
        LOG_INFO("tracker", L"Default output device: " << (it != endpoints.end() ? it->second->name : defaultId));
        defaultEndpointId = defaultId;
    }

    for (auto it = sessions.begin(); it != sessions.end();) {
        TrackedSession& s = *it->second;
        const wchar_t* reason = nullptr;
        AudioSessionState state = AudioSessionStateInactive;
        HRESULT stateHr = s.control->GetState(&state);
        if (complete && !s.seen) reason = L"no longer listed";
        else if (s.events && s.events->disconnected) reason = L"disconnected";
        else if ((s.events && s.events->expired) || (SUCCEEDED(stateHr) && state == AudioSessionStateExpired)) reason = L"expired";
        else if (s.invalid || stateHr == AUDCLNT_E_DEVICE_INVALIDATED) reason = L"device invalidated";
        else if (complete && endpoints.find(s.endpointId) == endpoints.end()) reason = L"device gone";
        if (reason) {
            EndSound(s);
            LOG_INFO("session", L"Session removed: " << s.Label() << L" on '" << s.deviceName << L"' (" << reason << L"), "
                                    << s.sounds << L" sounds, " << s.alerts << L" alerts");
            it = sessions.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = endpoints.begin(); it != endpoints.end();) {
        if (complete && !it->second->seen) {
            LOG_INFO("tracker", L"Output device gone: '" << it->second->name << L"'");
            it = endpoints.erase(it);
        } else {
            ++it;
        }
    }
}

SoundTracker::Impl::Endpoint* SoundTracker::Impl::AddEndpoint(IMMDevice* device, const std::wstring& id) {
    auto endpoint = std::make_unique<Endpoint>();
    endpoint->id = id;
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.Put()))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            endpoint->name = pv.pwszVal;
        }
        PropVariantClear(&pv);
    }
    if (endpoint->name.empty()) endpoint->name = L"(unnamed device)";

    HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(endpoint->manager.Put()));
    if (FAILED(hr)) {
        LOG_WARN("tracker", L"Cannot open session manager for '" << endpoint->name << L"': " << dbg::HrToString(hr));
        return nullptr;
    }
    // Session notifications only start after the enumerator has been requested once.
    ComPtr<IAudioSessionEnumerator> warmup;
    endpoint->manager->GetSessionEnumerator(warmup.Put());
    endpoint->notifier = new SessionCreatedNotifier(signal, endpoint->name);
    hr = endpoint->manager->RegisterSessionNotification(endpoint->notifier);
    if (FAILED(hr)) {
        LOG_WARN("tracker", L"RegisterSessionNotification failed for '" << endpoint->name << L"': " << dbg::HrToString(hr)
                                << L" (new sessions will be found by the periodic re-scan)");
        endpoint->notifier->Release();
        endpoint->notifier = nullptr;
    }

    std::wstring format = L"format unknown";
    ComPtr<IAudioClient> client;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.Put())))) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            format = std::to_wstring(mix->nSamplesPerSec) + L" Hz, " + std::to_wstring(mix->nChannels) + L" ch";
            CoTaskMemFree(mix);
        }
    }
    LOG_INFO("tracker", L"Output device: '" << endpoint->name << L"' (" << format << L") id=" << id);
    Endpoint* raw = endpoint.get();
    endpoints.emplace(id, std::move(endpoint));
    return raw;
}

bool SoundTracker::Impl::EnumerateSessions(Endpoint& endpoint) {
    ComPtr<IAudioSessionEnumerator> sessionEnum;
    HRESULT hr = endpoint.manager->GetSessionEnumerator(sessionEnum.Put());
    if (FAILED(hr)) {
        LOG_DEBUG("tracker", L"GetSessionEnumerator failed on '" << endpoint.name << L"': " << dbg::HrToString(hr));
        return false;
    }
    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) return false;
    bool complete = true;
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessionEnum->GetSession(i, control.Put()))) {
            complete = false;
            continue;
        }
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(control2))) {
            complete = false;
            continue;
        }
        LPWSTR rawKey = nullptr;
        std::wstring key;
        if (SUCCEEDED(control2->GetSessionInstanceIdentifier(&rawKey)) && rawKey) key = rawKey;
        CoTaskMemFree(rawKey);
        if (key.empty()) {
            complete = false;
            continue;
        }
        auto it = sessions.find(key);
        if (it != sessions.end()) {
            it->second->seen = true;
            continue;
        }
        AudioSessionState state = AudioSessionStateInactive;
        if (SUCCEEDED(control2->GetState(&state)) && state == AudioSessionStateExpired) continue;
        auto session = CreateSession(std::move(control2), key, endpoint);
        if (session) sessions.emplace(key, std::move(session));
    }
    return complete;
}

std::unique_ptr<SoundTracker::Impl::TrackedSession> SoundTracker::Impl::CreateSession(
    ComPtr<IAudioSessionControl2> control, const std::wstring& key, const Endpoint& endpoint) {
    auto s = std::make_unique<TrackedSession>();
    s->control = std::move(control);
    s->key = key;
    s->endpointId = endpoint.id;
    s->deviceName = endpoint.name;

    HRESULT pidHr = s->control->GetProcessId(&s->pid);
    s->isSystem = s->control->IsSystemSoundsSession() == S_OK;

    HRESULT hr = s->control.As(s->meter);
    if (FAILED(hr)) {
        LOG_WARN("session", L"Session without peak meter skipped (PID " << s->pid << L"): " << dbg::HrToString(hr));
        return nullptr;
    }
    s->control.As(s->volume);

    LPWSTR display = nullptr;
    if (SUCCEEDED(s->control->GetDisplayName(&display)) && display) {
        s->displayName = ResolveIndirectString(display);
        CoTaskMemFree(display);
    }

    if (s->isSystem) {
        s->processName = L"System Sounds";
    } else if (pidHr == AUDCLNT_S_NO_SINGLE_PROCESS) {
        s->processName = L"Multiple processes";
    } else if (s->pid == 0) {
        s->processName = L"Unknown";
    } else {
        DWORD error = 0;
        s->processPath = procinfo::QueryImagePath(s->pid, &error);
        s->processName = procinfo::FileName(s->processPath);
        if (s->processName.empty()) {
            s->processName = L"PID " + std::to_wstring(s->pid);
            LOG_DEBUG("session", L"Cannot read image path of PID " << s->pid << L": " << dbg::Win32ErrorToString(error));
        }
    }
    s->description = DescribeProcess(s->processName, s->isSystem);
    if (!s->displayName.empty() && !s->isSystem) s->description += L" [" + s->displayName + L"]";

    RefreshWindow(*s, Now(), true);

    s->events = new SessionEvents(signal, s->Label());
    hr = s->control->RegisterAudioSessionNotification(s->events);
    s->registered = SUCCEEDED(hr);
    if (!s->registered) LOG_WARN("session", L"RegisterAudioSessionNotification failed for " << s->Label() << L": " << dbg::HrToString(hr));

    BindRules(*s);
    std::wstring ruleNames;
    for (const auto& b : s->bindings) ruleNames += (ruleNames.empty() ? L"" : L", ") + rules[b.ruleIndex].name;
    LOG_INFO("session", L"Session added: " << s->Label() << L" on '" << s->deviceName << L"'"
                            << (s->isSystem ? L" [system sounds]" : L"")
                            << L" window='" << s->windowTitle << L"'"
                            << (s->windowOwnerPid && s->windowOwnerPid != s->pid ? L" (window owner PID " + std::to_wstring(s->windowOwnerPid) + L")" : L"")
                            << (ruleNames.empty() ? L"" : L" alert rules: " + ruleNames));
    LOG_DEBUG("session", L"  path=" << s->processPath << L" display='" << s->displayName << L"' key=" << key);
    return s;
}

void SoundTracker::Impl::BindRules(TrackedSession& s) {
    s.bindings.clear();
    for (size_t i = 0; i < rules.size(); ++i) {
        if (rules[i].enabled && WildcardMatch(rules[i].processPattern, s.processName)) {
            RuleBinding b;
            b.ruleIndex = i;
            s.bindings.push_back(b);
        }
    }
}

void SoundTracker::Impl::RefreshWindow(TrackedSession& s, TimeMs now, bool force) {
    if (s.isSystem || s.pid == 0) return;
    if (!force && s.lastWindowRefresh >= 0 && now - s.lastWindowRefresh < kWindowRefreshMs) return;
    s.lastWindowRefresh = now;
    std::vector<HWND> windows;
    if (s.windowOwnerPid) windows = procinfo::FindMainWindows(s.windowOwnerPid);
    if (windows.empty()) {
        auto owner = procinfo::ResolveWindowOwner(s.pid, s.processName);
        s.windowOwnerPid = owner.pid;
        windows = std::move(owner.windows);
    }
    s.windowTitle = recordWindowTitles ? procinfo::DescribeWindows(windows) : (windows.empty() ? L"" : L"(hidden)");
}

void SoundTracker::Impl::PollInput(TimeMs now) {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) return;
    if (haveInputTick && info.dwTime == lastInputTick) return;
    bool first = !haveInputTick;
    haveInputTick = true;
    lastInputTick = info.dwTime;
    if (first) return;  // Input before tracking started doesn't count

    DWORD age = GetTickCount() - info.dwTime;
    TimeMs when = now - static_cast<TimeMs>(age);
    lastAnyInput = when;
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    if (pid) {
        if (lastInputByPid.size() > 512) lastInputByPid.clear();
        lastInputByPid[pid] = when;
    }
    if (pid != lastForegroundPid) {
        lastForegroundPid = pid;
        LOG_DEBUG("input", L"Input now going to PID " << pid << L" '" << (foreground ? procinfo::WindowTitle(foreground) : L"") << L"'");
    }
}

TimeMs SoundTracker::Impl::LastInputFor(const TrackedSession& s, const AlertRule& rule) const {
    if (rule.suppressScope == 1) return lastAnyInput;
    TimeMs latest = -1;
    for (DWORD pid : {s.pid, s.windowOwnerPid}) {
        auto it = lastInputByPid.find(pid);
        if (pid && it != lastInputByPid.end()) latest = (std::max)(latest, it->second);
    }
    return latest;
}

void SoundTracker::Impl::PollMeters(TimeMs now) {
    for (auto& entry : sessions) {
        TrackedSession& s = *entry.second;
        if (s.invalid) continue;
        float peak = 0.0f;
        HRESULT hr = s.meter->GetPeakValue(&peak);
        if (FAILED(hr)) {
            ++s.meterFailures;
            if (s.meterFailures == 1 || s.meterFailures % 1000 == 0) {
                LOG_WARN("meter", L"GetPeakValue failed for " << s.Label() << L" (" << s.meterFailures << L"x): " << dbg::HrToString(hr));
            }
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                s.invalid = true;
                signal->RequestRescan();
            }
            continue;
        }
        if (s.meterFailures) {
            LOG_INFO("meter", L"Peak meter recovered for " << s.Label() << L" after " << s.meterFailures << L" failures");
            s.meterFailures = 0;
        }
        s.lastPeak = peak;
        if (now - s.maxWindowStart >= 1500) {
            s.maxB = s.maxA;
            s.maxA = 0.0f;
            s.maxWindowStart = now;
        }
        s.maxA = (std::max)(s.maxA, peak);

        switch (s.segmenter.Update(now, peak, detection.soundThreshold, detection.soundReleaseMs)) {
            case SoundSegmenter::Change::Started: StartSound(s, now); break;
            case SoundSegmenter::Change::Ended: EndSound(s); break;
            case SoundSegmenter::Change::None: break;
        }
        if (s.segmenter.Active()) UpdateOngoing(s, now);
        for (auto& binding : s.bindings) EvaluateRule(s, binding, now, peak);
    }
}

AudioEvent* SoundTracker::Impl::FindEventLocked(uint64_t id) {
    if (events.empty() || id < events.front().id || id > events.back().id) return nullptr;
    AudioEvent& e = events[static_cast<size_t>(id - events.front().id)];
    return e.id == id ? &e : nullptr;
}

void SoundTracker::Impl::StartSound(TrackedSession& s, TimeMs now) {
    RefreshWindow(s, now, false);
    AudioEvent e;
    e.timestamp = std::chrono::system_clock::now();
    e.processId = s.pid;
    e.processName = s.processName;
    e.processPath = s.processPath;
    e.soundDescription = s.description;
    e.sessionDisplayName = s.displayName;
    e.windowTitle = s.windowTitle;
    e.deviceName = s.deviceName;
    e.isSystemSound = s.isSystem;
    e.peakLevel = s.segmenter.MaxPeak();
    if (s.volume) {
        float volume = 0.0f;
        BOOL mute = FALSE;
        if (SUCCEEDED(s.volume->GetMasterVolume(&volume)) && SUCCEEDED(s.volume->GetMute(&mute))) {
            e.volumeLevel = mute ? 0.0f : volume;
        }
    }
    if (s.isSystem) {
        e.deviceChangeInfo = devices.DescribeRecentChange(3000);
        if (!e.deviceChangeInfo.empty()) e.soundDescription += L" - " + e.deviceChangeInfo;
    }
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        e.id = nextEventId++;
        events.push_back(e);
        while (events.size() > kMaxEvents) events.pop_front();
        ++eventsVersion;
    }
    s.currentEventId = e.id;
    s.lastOngoingUpdate = now;
    ++s.sounds;
    ++soundCount;
    LOG_DEBUG("sound", L"Start #" << e.id << L": " << s.Label() << L" peak " << dbg::Pct(e.peakLevel) << L" on '" << s.deviceName << L"'"
                           << (e.deviceChangeInfo.empty() ? L"" : L" (" + e.deviceChangeInfo + L")"));
}

void SoundTracker::Impl::UpdateOngoing(TrackedSession& s, TimeMs now) {
    if (!s.currentEventId || now - s.lastOngoingUpdate < 100) return;
    s.lastOngoingUpdate = now;
    // Long sounds (music, videos) outlive window changes; keep the title current.
    RefreshWindow(s, now, false);
    std::lock_guard<std::mutex> lock(eventsMutex);
    if (AudioEvent* e = FindEventLocked(s.currentEventId)) {
        e->durationMs = static_cast<DWORD>(s.segmenter.DurationMs());
        e->peakLevel = s.segmenter.MaxPeak();
        e->windowTitle = s.windowTitle;
        ++eventsVersion;
    }
}

void SoundTracker::Impl::EndSound(TrackedSession& s) {
    if (!s.currentEventId) return;
    AudioEvent copy;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(eventsMutex);
        if (AudioEvent* e = FindEventLocked(s.currentEventId)) {
            e->ongoing = false;
            e->durationMs = static_cast<DWORD>(s.segmenter.DurationMs());
            e->peakLevel = s.segmenter.MaxPeak();
            copy = *e;
            found = true;
            ++eventsVersion;
        }
    }
    if (found) {
        std::lock_guard<std::mutex> lock(csvMutex);
        if (csv) csv->LogEvent(copy);
    }
    bool detailed = !s.bindings.empty() || dbg::Enabled(dbg::Level::Trace);
    LOG_DEBUG("sound", L"End #" << s.currentEventId << L": " << s.Label() << L" " << s.segmenter.DurationMs() << L" ms, max "
                         << dbg::Pct(s.segmenter.MaxPeak()) << (found ? L"" : L" (evicted)")
                         << (detailed ? L", envelope % per 100ms: [" + s.segmenter.EnvelopeText() + L"]" : L""));
    s.currentEventId = 0;
    s.segmenter.Reset();
}

void SoundTracker::Impl::EvaluateRule(TrackedSession& s, RuleBinding& b, TimeMs now, float peak) {
    const AlertRule& rule = rules[b.ruleIndex];
    AlertParams params = ToParams(rule);
    TimeMs lastInput = LastInputFor(s, rule);
    AlertEvaluator::Result r = b.evaluator.Update(now, peak, lastInput, params);

    if (r.runStarted) {
        LOG_DEBUG("alerts", L"'" << rule.name << L"' " << s.Label() << L": loud " << dbg::Pct(peak) << L" (threshold "
                                 << rule.thresholdPercent << L"%), quiet before: "
                                 << (r.quietForMs < 0 ? std::wstring(L"since start") : std::to_wstring(r.quietForMs) + L" ms"));
    }
    if (r.decision == AlertDecision::None) return;

    std::wostringstream detail;
    detail << L"'" << rule.name << L"' " << s.Label() << L": " << AlertDecisionName(r.decision) << L" | peak " << dbg::Pct(peak)
           << L", threshold " << rule.thresholdPercent << L"%, quiet before "
           << (r.quietForMs < 0 ? std::wstring(L"(no earlier sound)") : std::to_wstring(r.quietForMs) + L" ms") << L" (need "
           << rule.quietBeforeMs << L"), loud for " << r.loudForMs << L" ms";
    if (r.decision == AlertDecision::BlockedCooldown) detail << L", cooldown left " << r.cooldownLeftMs << L" ms";
    if (r.inputAgeMs >= 0) detail << L", last input " << r.inputAgeMs << L" ms ago";

    if (r.decision == AlertDecision::Fired) {
        ++b.fired;
        ++s.alerts;
        ++alertCount;
        if (s.currentEventId) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            if (AudioEvent* e = FindEventLocked(s.currentEventId)) {
                ++e->alertsFired;
                ++eventsVersion;
            }
        }
        LOG_INFO("alerts", L"ALERT " << detail.str());
        if (onAlert) {
            AlertFire fire;
            fire.processId = s.pid;
            fire.windowOwnerPid = s.windowOwnerPid ? s.windowOwnerPid : s.pid;
            fire.processName = s.processName;
            fire.rule = rule;
            fire.peak = peak;
            fire.quietForMs = r.quietForMs;
            fire.detectedAt = std::chrono::steady_clock::now();
            onAlert(fire);
        }
    } else if ((r.decision == AlertDecision::BlockedCooldown || r.decision == AlertDecision::BlockedInput) &&
               b.infoLimit.Allow(now, 1000)) {
        // The interesting near-misses are logged at Info so normal logs explain missed alerts.
        LOG_INFO("alerts", L"No alert: " << detail.str());
    } else {
        LOG_DEBUG("alerts", L"No alert: " << detail.str());
    }
}

void SoundTracker::Impl::PublishLive(TimeMs now) {
    if (lastLivePublish >= 0 && now - lastLivePublish < kLivePublishMs) return;
    lastLivePublish = now;
    std::vector<LiveSessionInfo> snapshot;
    snapshot.reserve(sessions.size());
    for (const auto& entry : sessions) {
        const TrackedSession& s = *entry.second;
        LiveSessionInfo info;
        info.processId = s.pid;
        info.windowOwnerPid = s.windowOwnerPid;
        info.processName = s.processName;
        info.windowTitle = s.windowTitle;
        info.deviceName = s.deviceName;
        info.isSystem = s.isSystem;
        info.peakNow = s.lastPeak;
        info.peakRecentMax = (std::max)(s.maxA, s.maxB);
        for (const auto& b : s.bindings) {
            const AlertRule& rule = rules[b.ruleIndex];
            if (!info.ruleNames.empty()) info.ruleNames += L", ";
            info.ruleNames += rule.name;
            if (info.alertState.empty()) info.alertState = b.evaluator.StateText(now, LastInputFor(s, rule), ToParams(rule));
            info.alertsFired += b.fired;
        }
        snapshot.push_back(std::move(info));
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const LiveSessionInfo& a, const LiveSessionInfo& b) {
        int c = _wcsicmp(a.processName.c_str(), b.processName.c_str());
        return c != 0 ? c < 0 : a.processId < b.processId;
    });
    std::lock_guard<std::mutex> lock(liveMutex);
    live.swap(snapshot);
}

void SoundTracker::Impl::UpdateStats(TimeMs now) {
    if (lastTick >= 0) {
        TimeMs interval = now - lastTick;
        ++statTicks;
        statSumInterval += interval;
        statMaxInterval = (std::max)(statMaxInterval, interval);
        if (interval > detection.meterIntervalMs * 3 + 20) ++statLateTicks;
        if (interval > 500 && stallLimit.Allow(now, 60000)) {
            LOG_WARN("meter", L"Meter loop stalled for " << interval << L" ms (system busy or suspended?) - sounds in that gap may be missed");
        }
    }
    lastTick = now;

    if (now - statsStart >= 60000) {
        LOG_DEBUG("meter", L"Meter loop last 60s: " << statTicks << L" ticks, avg "
                               << (statTicks ? static_cast<double>(statSumInterval) / statTicks : 0.0) << L" ms, max "
                               << statMaxInterval << L" ms, late " << statLateTicks << L", sessions " << sessions.size());
        statsStart = now;
        statTicks = 0;
        statSumInterval = 0;
        statMaxInterval = 0;
        statLateTicks = 0;
    }
    if (now - lastInfoHeartbeat >= 300000) {
        lastInfoHeartbeat = now;
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        LOG_INFO("health", L"Heartbeat: " << endpoints.size() << L" devices, " << sessions.size() << L" sessions, "
                               << soundCount.load() << L" sounds, " << alertCount.load() << L" alerts, memory "
                               << (pmc.WorkingSetSize / (1024 * 1024)) << L" MB, handles " << handles);
    }
}
