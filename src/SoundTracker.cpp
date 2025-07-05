#define NOMINMAX  // Prevent Windows.h from defining min/max macros
#include "../include/SoundTracker.h"
#include "../include/Logger.h"
#include <psapi.h>
#include <audioclient.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <initguid.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

// Define USB device class GUID if not already defined
#ifndef GUID_DEVCLASS_USB
DEFINE_GUID(GUID_DEVCLASS_USB, 0x36fc9e60, 0xc465, 0x11cf, 0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
#endif

CSoundTrackerAudioSessionEvents::CSoundTrackerAudioSessionEvents(SoundTracker* pTracker, DWORD processId) 
    : _cRef(1), _pTracker(pTracker), _processId(processId) {
}

CSoundTrackerAudioSessionEvents::~CSoundTrackerAudioSessionEvents() {
}

ULONG STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::AddRef() {
    return InterlockedIncrement(&_cRef);
}

ULONG STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::Release() {
    ULONG ulRef = InterlockedDecrement(&_cRef);
    if (0 == ulRef) {
        delete this;
    }
    return ulRef;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::QueryInterface(REFIID riid, void** ppvInterface) {
    if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown*)this;
    }
    else if (__uuidof(IAudioSessionEvents) == riid) {
        AddRef();
        *ppvInterface = (IAudioSessionEvents*)this;
    }
    else {
        *ppvInterface = NULL;
        return E_NOINTERFACE;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnSimpleVolumeChanged(float NewVolume, BOOL NewMute, LPCGUID EventContext) {
    if (!NewMute && NewVolume > 0.0f) {
        _pTracker->AddAudioEvent(_processId, NewVolume, 0.0f);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnStateChanged(AudioSessionState NewState) {
    if (NewState == AudioSessionStateActive) {
        _pTracker->AddAudioEvent(_processId, 0.0f, 0.0f);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnDisplayNameChanged(LPCWSTR NewDisplayName, LPCGUID EventContext) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnIconPathChanged(LPCWSTR NewIconPath, LPCGUID EventContext) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnChannelVolumeChanged(DWORD ChannelCount, float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnGroupingParamChanged(LPCGUID NewGroupingParam, LPCGUID EventContext) {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CSoundTrackerAudioSessionEvents::OnSessionDisconnected(AudioSessionDisconnectReason DisconnectReason) {
    return S_OK;
}

SoundTracker::SoundTracker() 
    : m_running(false), m_pEnumerator(nullptr), m_logger(nullptr) {
    m_startTime = std::chrono::system_clock::now();
    m_logFilePath = L"logs\\sound_tracker.log";
}

SoundTracker::~SoundTracker() {
    Stop();
    
    // Clean up all registered audio session events
    {
        std::lock_guard<std::mutex> lock(m_eventsMutex);
        for (auto& pair : m_activeEvents) {
            // Unregister the event notification
            pair.first->UnregisterAudioSessionNotification(pair.second);
            // Release the event object
            pair.second->Release();
            // Release the session control
            pair.first->Release();
        }
        m_activeEvents.clear();
    }
    
    if (m_pEnumerator) {
        m_pEnumerator->Release();
    }
    CoUninitialize();
}

bool SoundTracker::Initialize() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), (void**)&m_pEnumerator);
    
    // Logger will be created when tracking starts
    
    return SUCCEEDED(hr);
}

void SoundTracker::Start() {
    if (m_running) return;
    
    // Clear previous events for new session
    {
        std::lock_guard<std::mutex> lock(m_logMutex);
        m_events.clear();
    }
    
    // Create a new logger for this tracking session
    m_logger = std::make_unique<Logger>(L"logs");
    if (!m_logger->Initialize()) {
        // Handle error but continue tracking
    }
    
    // Record start time
    m_startTime = std::chrono::system_clock::now();
    
    m_running = true;
    m_monitorThread = std::thread(&SoundTracker::MonitorAudioSessions, this);
}

void SoundTracker::Stop() {
    m_running = false;
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
    
    // Clean up all registered audio session events when stopping
    {
        std::lock_guard<std::mutex> lock(m_eventsMutex);
        for (auto& pair : m_activeEvents) {
            // Unregister the event notification
            pair.first->UnregisterAudioSessionNotification(pair.second);
            // Release the event object
            pair.second->Release();
            // Release the session control
            pair.first->Release();
        }
        m_activeEvents.clear();
    }
    
    // Close the logger
    if (m_logger) {
        m_logger->Close();
    }
}

void SoundTracker::MonitorAudioSessions() {
    while (m_running) {
        // Get ALL audio endpoints, not just the default
        IMMDeviceCollection* pCollection = nullptr;
        HRESULT hr = m_pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
        
        if (SUCCEEDED(hr)) {
            UINT deviceCount = 0;
            pCollection->GetCount(&deviceCount);
            
            // Monitor all active audio devices
            for (UINT deviceIdx = 0; deviceIdx < deviceCount; deviceIdx++) {
                IMMDevice* pDevice = nullptr;
                if (SUCCEEDED(pCollection->Item(deviceIdx, &pDevice))) {
                    IAudioSessionManager2* pSessionManager = nullptr;
                    hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager);
                    
                    if (SUCCEEDED(hr)) {
                        IAudioSessionEnumerator* pSessionEnumerator = nullptr;
                        hr = pSessionManager->GetSessionEnumerator(&pSessionEnumerator);
                        
                        if (SUCCEEDED(hr)) {
                            int sessionCount = 0;
                            pSessionEnumerator->GetCount(&sessionCount);
                            
                            for (int i = 0; i < sessionCount; i++) {
                                IAudioSessionControl* pSessionControl = nullptr;
                                if (SUCCEEDED(pSessionEnumerator->GetSession(i, &pSessionControl))) {
                                    IAudioSessionControl2* pSessionControl2 = nullptr;
                                    pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2);
                                    
                                    if (pSessionControl2) {
                                        ProcessAudioSession(pSessionControl2);
                                        pSessionControl2->Release();
                                    }
                                    pSessionControl->Release();
                                }
                            }
                            pSessionEnumerator->Release();
                        }
                        pSessionManager->Release();
                    }
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
        
        // Reduced polling interval for better performance with multiple devices
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void SoundTracker::ProcessAudioSession(IAudioSessionControl2* pSessionControl) {
    DWORD processId = 0;
    HRESULT hr = pSessionControl->GetProcessId(&processId);
    
    // Process even if processId is 0 (system sounds)
    if (SUCCEEDED(hr)) {
        AudioSessionState state;
        pSessionControl->GetState(&state);
        
        if (state == AudioSessionStateActive) {
            ISimpleAudioVolume* pVolume = nullptr;
            hr = pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pVolume);
            
            if (SUCCEEDED(hr)) {
                float volume = 0.0f;
                BOOL mute = FALSE;
                pVolume->GetMasterVolume(&volume);
                pVolume->GetMute(&mute);
                
                if (!mute && volume > 0.0f) {
                    float peak = GetPeakMeterValue(pSessionControl);
                    if (peak > 0.001f) {  // Lower threshold to catch quiet system sounds
                        AddAudioEvent(processId, volume, peak);
                    }
                }
                
                pVolume->Release();
            }
        }
        
        // Get session display name for more info
        LPWSTR pDisplayName = nullptr;
        pSessionControl->GetDisplayName(&pDisplayName);
        std::wstring sessionName;
        if (pDisplayName) {
            sessionName = pDisplayName;
            CoTaskMemFree(pDisplayName);
        }
        
        // Store session name for event creation
        if (!sessionName.empty() && processId == 0) {
            // For system sounds, use session name as hint
            m_sessionNames[0] = sessionName;
        }
        
        // Register for events and store for cleanup
        CSoundTrackerAudioSessionEvents* pEvents = new CSoundTrackerAudioSessionEvents(this, processId);
        if (SUCCEEDED(pSessionControl->RegisterAudioSessionNotification(pEvents))) {
            // Store for cleanup - AddRef the session control
            pSessionControl->AddRef();
            std::lock_guard<std::mutex> lock(m_eventsMutex);
            m_activeEvents.push_back(std::make_pair(pSessionControl, pEvents));
        } else {
            // Registration failed, clean up
            pEvents->Release();
        }
    }
}

float SoundTracker::GetPeakMeterValue(IAudioSessionControl2* pSessionControl) {
    IAudioMeterInformation* pMeter = nullptr;
    float peak = 0.0f;
    
    HRESULT hr = pSessionControl->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&pMeter);
    if (SUCCEEDED(hr)) {
        pMeter->GetPeakValue(&peak);
        pMeter->Release();
    }
    
    return peak;
}

std::wstring SoundTracker::GetProcessNameFromPID(DWORD processId) {
    // Check cache first with dedicated mutex to avoid deadlock
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_processCache.find(processId) != m_processCache.end()) {
            return m_processCache[processId];
        }
    }
    
    std::wstring processName = L"Unknown";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    
    if (hProcess) {
        WCHAR szProcessName[MAX_PATH] = L"";
        if (GetModuleBaseNameW(hProcess, NULL, szProcessName, sizeof(szProcessName) / sizeof(WCHAR))) {
            processName = szProcessName;
            // Cache the result with dedicated mutex
            {
                std::lock_guard<std::mutex> lock(m_cacheMutex);
                m_processCache[processId] = processName;
            }
        }
        CloseHandle(hProcess);
    }
    
    return processName;
}

std::wstring SoundTracker::GetProcessPathFromPID(DWORD processId) {
    std::wstring processPath = L"";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    
    if (hProcess) {
        WCHAR szProcessPath[MAX_PATH] = L"";
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, szProcessPath, &size)) {
            processPath = szProcessPath;
        }
        CloseHandle(hProcess);
    }
    
    return processPath;
}

std::wstring SoundTracker::GetSoundDescription(DWORD processId, const std::wstring& processName) {
    // System sounds - including USB device sounds
    if (processId == 0) {
        return L"Windows System Sound (USB/Device Connect)";
    }
    if (processId == 4) {
        return L"Windows Kernel System Sound";
    }
    
    // Common applications and system processes
    std::unordered_map<std::wstring, std::wstring> knownApps = {
        {L"chrome.exe", L"Google Chrome Browser"},
        {L"firefox.exe", L"Mozilla Firefox Browser"},
        {L"msedge.exe", L"Microsoft Edge Browser"},
        {L"Discord.exe", L"Discord Voice/Message"},
        {L"Teams.exe", L"Microsoft Teams"},
        {L"Spotify.exe", L"Spotify Music"},
        {L"slack.exe", L"Slack Notification"},
        {L"outlook.exe", L"Outlook Email Notification"},
        {L"explorer.exe", L"Windows Explorer/System Sound"},
        {L"svchost.exe", L"Windows Service/System Sound"},
        {L"audiodg.exe", L"Windows Audio Device Graph"},
        {L"RuntimeBroker.exe", L"Windows Runtime Broker"},
        {L"SearchApp.exe", L"Windows Search"},
        {L"ShellExperienceHost.exe", L"Windows Shell Experience"},
        {L"SystemSettings.exe", L"Windows Settings"},
        {L"UserNotificationBroker.exe", L"Windows Notifications"},
        {L"csrss.exe", L"Windows Client/Server Runtime"},
        {L"dwm.exe", L"Desktop Window Manager"},
        {L"winlogon.exe", L"Windows Logon Process"},
        {L"services.exe", L"Windows Services Controller"},
        {L"lsass.exe", L"Local Security Authority"},
        {L"System", L"Windows System Process"},
        {L"TextInputHost.exe", L"Windows Text Input (Keyboard)"},
        {L"ctfmon.exe", L"CTF Loader (Keyboard/Language)"},
        {L"TabTip.exe", L"Touch Keyboard and Handwriting"},
        {L"osk.exe", L"On-Screen Keyboard"},
        {L"SynTPEnh.exe", L"Synaptics TouchPad"},
        {L"ETDCtrl.exe", L"ELAN TouchPad"},
        {L"", L"Unknown System Process (Possible USB/Keyboard Event)"},
    };
    
    // Special handling for empty process name (system sounds)
    if (processName.empty() || processName == L"Unknown") {
        return L"System Sound (Check USB/Keyboard/Input Devices)";
    }
    
    // Check for keyboard-related processes
    if (processName.find(L"TabTip") != std::wstring::npos ||
        processName.find(L"TextInput") != std::wstring::npos ||
        processName.find(L"ctfmon") != std::wstring::npos ||
        processName.find(L"osk") != std::wstring::npos) {
        return processName + L" (Keyboard/Input Related)";
    }
    
    auto it = knownApps.find(processName);
    if (it != knownApps.end()) {
        return it->second;
    }
    
    return processName + L" Audio";
}

std::wstring SoundTracker::GetUSBDeviceInfo(DWORD processId, const std::wstring& processName) {
    // Check if this might be USB-related
    if (processId == 0 || processId == 4 || processName == L"svchost.exe" || 
        processName == L"System" || processName.empty()) {
        
        // Enumerate USB devices to find recently connected ones
        HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_USB, NULL, NULL, DIGCF_PRESENT);
        if (hDevInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA deviceData;
            deviceData.cbSize = sizeof(SP_DEVINFO_DATA);
            
            for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &deviceData); i++) {
                WCHAR deviceName[256] = {0};
                WCHAR deviceDesc[256] = {0};
                
                // Get device description
                if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &deviceData, 
                    SPDRP_DEVICEDESC, NULL, (PBYTE)deviceDesc, sizeof(deviceDesc), NULL)) {
                    
                    // Get device friendly name
                    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &deviceData, 
                        SPDRP_FRIENDLYNAME, NULL, (PBYTE)deviceName, sizeof(deviceName), NULL);
                    
                    // Get device location (port info)
                    WCHAR locationInfo[256] = {0};
                    if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &deviceData,
                        SPDRP_LOCATION_INFORMATION, NULL, (PBYTE)locationInfo, sizeof(locationInfo), NULL)) {
                        
                        // Check if device was recently connected (simplified check)
                        // In production, you'd check actual connection timestamps
                        std::wstring info = L"USB: ";
                        
                        // Use friendly name if available, otherwise description
                        if (deviceName[0]) {
                            info += deviceName;
                        } else {
                            info += deviceDesc;
                        }
                        
                        // Parse location info to make it clearer
                        std::wstring location(locationInfo);
                        if (location.find(L"Port_#") != std::wstring::npos) {
                            // Extract port number
                            size_t portPos = location.find(L"Port_#");
                            size_t portNum = portPos + 6; // Skip "Port_#"
                            if (portNum < location.length() && isdigit(location[portNum])) {
                                info += L" (Port ";
                                info += location[portNum];
                                
                                // Check for hub info
                                if (location.find(L"Hub_#") != std::wstring::npos) {
                                    size_t hubPos = location.find(L"Hub_#");
                                    size_t hubNum = hubPos + 5;
                                    if (hubNum < location.length() && isdigit(location[hubNum])) {
                                        info += L", Hub ";
                                        info += location[hubNum];
                                    }
                                }
                                info += L")";
                            }
                        } else if (!location.empty()) {
                            info += L" (";
                            info += location;
                            info += L")";
                        }
                        
                        SetupDiDestroyDeviceInfoList(hDevInfo);
                        return info;
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }
    }
    
    return L"";
}

std::wstring SoundTracker::GetBrowserTabInfo(DWORD processId, const std::wstring& processName) {
    // Check if this is a browser process
    if (processName == L"chrome.exe" || processName == L"msedge.exe" || 
        processName == L"firefox.exe" || processName == L"opera.exe" ||
        processName == L"brave.exe" || processName == L"vivaldi.exe") {
        
        // Find all windows for this process
        struct EnumData {
            DWORD processId;
            std::wstring title;
        } enumData = { processId, L"" };
        
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            EnumData* pData = reinterpret_cast<EnumData*>(lParam);
            
            DWORD windowProcessId;
            GetWindowThreadProcessId(hwnd, &windowProcessId);
            
            if (windowProcessId == pData->processId) {
                // Check if this is a main browser window
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                    WCHAR windowTitle[512] = {0};
                    int length = GetWindowTextW(hwnd, windowTitle, 512);
                    
                    if (length > 0) {
                        std::wstring title(windowTitle);
                        
                        // Skip browser UI windows
                        if (title.find(L"DevTools") == std::wstring::npos &&
                            title.find(L"Developer Tools") == std::wstring::npos &&
                            title.find(L"Extensions") == std::wstring::npos) {
                            
                            // Extract meaningful part (remove browser name suffix)
                            size_t dashPos = title.rfind(L" - ");
                            if (dashPos != std::wstring::npos) {
                                title = title.substr(0, dashPos);
                            }
                            
                            // Limit length for display
                            if (title.length() > 50) {
                                title = title.substr(0, 47) + L"...";
                            }
                            
                            pData->title = title;
                            return FALSE; // Stop enumeration
                        }
                    }
                }
            }
            return TRUE; // Continue enumeration
        }, reinterpret_cast<LPARAM>(&enumData));
        
        if (!enumData.title.empty()) {
            return L"Tab: \"" + enumData.title + L"\"";
        }
    }
    
    return L"";
}

void SoundTracker::AddAudioEvent(DWORD processId, float volume, float peak) {
    try {
        AudioEvent event;
        event.timestamp = std::chrono::system_clock::now();
        event.processId = processId;
        event.processName = GetProcessNameFromPID(processId);
        event.processPath = GetProcessPathFromPID(processId);
        event.soundDescription = GetSoundDescription(processId, event.processName);
        
        // Get USB device info if applicable
        event.usbDeviceInfo = GetUSBDeviceInfo(processId, event.processName);
        
        // Get browser tab info if applicable
        event.browserTabInfo = GetBrowserTabInfo(processId, event.processName);
        
        // Add session name to description if available
        auto sessionIt = m_sessionNames.find(processId);
        if (sessionIt != m_sessionNames.end() && !sessionIt->second.empty()) {
            event.sessionDisplayName = sessionIt->second;
            event.soundDescription += L" [" + sessionIt->second + L"]";
        }
        
        // Add USB info to description if available
        if (!event.usbDeviceInfo.empty()) {
            event.soundDescription += L" - " + event.usbDeviceInfo;
        }
        
        // Add browser tab info to description if available
        if (!event.browserTabInfo.empty()) {
            event.soundDescription += L" - " + event.browserTabInfo;
        }
        
        event.volumeLevel = volume;
        event.peakLevel = peak;
        event.isSystemSound = (processId == 0 || processId == 4 || event.processName == L"svchost.exe");
        event.duration_ms = 0;  // Will be calculated based on continuous events
        event.eventCount = 1;  // Default count
        
        // Thread-safe addition to events vector
        {
            std::lock_guard<std::mutex> lock(m_logMutex);
            
            // Check if we should batch with the last event
            if (!m_events.empty()) {
                auto& lastEvent = m_events.back();
                
                // Get time_t for both timestamps to compare minutes
                auto lastTime = std::chrono::system_clock::to_time_t(lastEvent.timestamp);
                auto currentTime = std::chrono::system_clock::to_time_t(event.timestamp);
                
                std::tm lastTm = {}, currentTm = {};
                localtime_s(&lastTm, &lastTime);
                localtime_s(&currentTm, &currentTime);
                
                // If same minute (hour and minute match) AND same process, increment count
                if (lastTm.tm_hour == currentTm.tm_hour && 
                    lastTm.tm_min == currentTm.tm_min && 
                    lastEvent.processId == event.processId) {
                    lastEvent.eventCount++;
                    // Update peak/volume to max values
                    lastEvent.peakLevel = (std::max)(lastEvent.peakLevel, event.peakLevel);
                    lastEvent.volumeLevel = (std::max)(lastEvent.volumeLevel, event.volumeLevel);
                    return;  // Don't add new event, just increment count
                }
            }
            
            // Limit memory usage - keep only last 10000 events
            if (m_events.size() > 10000) {
                m_events.erase(m_events.begin(), m_events.begin() + 1000);
            }
            m_events.push_back(event);
        }
        
        // Log to file (non-blocking)
        LogEvent(event);
    }
    catch (const std::exception&) {
        // Silently ignore exceptions to keep monitoring running
        // Could add debug logging here if needed
    }
}

void SoundTracker::LogEvent(const AudioEvent& event) {
    // Use the single logger instance for efficiency
    if (m_logger) {
        m_logger->LogEvent(event);
    }
}

bool SoundTracker::ExportLogs(const std::wstring& outputPath, 
                             const std::chrono::system_clock::time_point& startTime,
                             const std::chrono::system_clock::time_point& endTime) {
    std::vector<AudioEvent> filteredEvents = GetEvents(startTime, endTime);
    // Use the existing logger instance if available, otherwise create temporary one
    if (m_logger) {
        return m_logger->ExportEvents(filteredEvents, outputPath, LogFormat::CSV);
    } else {
        Logger tempLogger(L"logs");
        return tempLogger.ExportEvents(filteredEvents, outputPath, LogFormat::CSV);
    }
}

std::vector<AudioEvent> SoundTracker::GetEvents(const std::chrono::system_clock::time_point& startTime,
                                               const std::chrono::system_clock::time_point& endTime) {
    std::lock_guard<std::mutex> lock(m_logMutex);
    std::vector<AudioEvent> filtered;
    
    for (const auto& event : m_events) {
        if (event.timestamp >= startTime && event.timestamp <= endTime) {
            filtered.push_back(event);
        }
    }
    
    return filtered;
}

std::wstring SoundTracker::GetCurrentLogPath() const {
    if (m_logger) {
        return m_logger->GetCurrentLogPath();
    }
    return L"";
}