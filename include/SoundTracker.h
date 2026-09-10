#pragma once
#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <unordered_map>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

// Include the separated AudioEvent structure
#include "AudioEvent.h"

// Custom implementation of IAudioSessionEvents interface
class CSoundTrackerAudioSessionEvents : public IAudioSessionEvents {
public:
    LONG _cRef;
    class SoundTracker* _pTracker;
    DWORD _processId;
    std::mutex _trackerMutex;
    std::atomic<bool> _disconnected{false};

    CSoundTrackerAudioSessionEvents(class SoundTracker* pTracker, DWORD processId);
    ~CSoundTrackerAudioSessionEvents();
    void Detach();

    // IUnknown methods
    ULONG STDMETHODCALLTYPE AddRef();
    ULONG STDMETHODCALLTYPE Release();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvInterface);

    // IAudioSessionEvents methods
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR NewDisplayName, LPCGUID EventContext);
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR NewIconPath, LPCGUID EventContext);
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float NewVolume, BOOL NewMute, LPCGUID EventContext);
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD ChannelCount, float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext);
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID NewGroupingParam, LPCGUID EventContext);
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState NewState);
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason DisconnectReason);
};

class SoundTracker {
private:
    std::atomic<bool> m_running;
    std::thread m_monitorThread;
    std::mutex m_logMutex;
    std::mutex m_cacheMutex;  // Separate mutex for process cache to avoid deadlock
    std::vector<AudioEvent> m_events;
    IMMDeviceEnumerator* m_pEnumerator;
    bool m_comInitialized = false;
    std::unordered_map<DWORD, std::wstring> m_processCache;
    std::unordered_map<DWORD, std::wstring> m_sessionNames;  // Store session display names
    std::chrono::system_clock::time_point m_startTime;
    std::wstring m_logFilePath;
    std::unique_ptr<class Logger> m_logger;  // Single logger instance for efficiency
    
    struct SessionRegistration {
        IAudioSessionControl2* control = nullptr;
        CSoundTrackerAudioSessionEvents* events = nullptr;
        bool seen = false;
        ~SessionRegistration();
    };
    // Session instance IDs remain stable across enumerations; COM pointers need not.
    // Only the monitor thread owns and removes registrations.
    std::unordered_map<std::wstring, std::unique_ptr<SessionRegistration>> m_activeEvents;

    void MonitorAudioSessions();
    bool ProcessAudioSession(IAudioSessionControl2* pSessionControl);
    void RemoveInactiveSessions(bool enumerationComplete);
    std::wstring GetProcessNameFromPID(DWORD processId);
    std::wstring GetProcessPathFromPID(DWORD processId);
    std::wstring GetSoundDescription(DWORD processId, const std::wstring& processName);
    std::wstring GetUSBDeviceInfo(DWORD processId, const std::wstring& processName);
    std::wstring GetBrowserTabInfo(DWORD processId, const std::wstring& processName);
    void LogEvent(const AudioEvent& event);
    float GetPeakMeterValue(IAudioSessionControl2* pSessionControl);

public:
    SoundTracker();
    ~SoundTracker();

    bool Initialize();
    void Start();
    void Stop();
    bool IsRunning() const { return m_running; }
    
    void AddAudioEvent(DWORD processId, float volume, float peak);
    bool ExportLogs(const std::wstring& outputPath, 
                   const std::chrono::system_clock::time_point& startTime,
                   const std::chrono::system_clock::time_point& endTime);
    
    std::vector<AudioEvent> GetEvents(const std::chrono::system_clock::time_point& startTime,
                                     const std::chrono::system_clock::time_point& endTime);
    
    size_t GetEventCount() const { return m_events.size(); }
    std::chrono::system_clock::time_point GetStartTime() const { return m_startTime; }
    std::wstring GetCurrentLogPath() const;
};