#pragma once
#include <windows.h>
#include <cfgmgr32.h>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

// Records device arrivals and removals (USB, Bluetooth, HID, ...) so a Windows
// "device connected/disconnected" system sound can be attributed to the device
// that actually changed, instead of guessing.
class DeviceWatcher {
public:
    DeviceWatcher() = default;
    ~DeviceWatcher();
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    bool Start();
    void Stop();

    // e.g. "Connected: SanDisk Cruzer Blade (0.4s before)". Empty if nothing
    // changed within the window.
    std::wstring DescribeRecentChange(int withinMs) const;

private:
    static DWORD CALLBACK OnNotification(HCMNOTIFICATION notify, PVOID context, CM_NOTIFY_ACTION action,
                                         PCM_NOTIFY_EVENT_DATA data, DWORD dataSize);
    void Record(bool arrival, const wchar_t* symbolicLink);
    std::wstring LookupName(const std::wstring& instanceId);

    struct Change {
        ULONGLONG tick = 0;
        bool arrival = false;
        std::wstring instanceId;
        std::wstring name;
    };

    HCMNOTIFICATION m_handle = nullptr;
    mutable std::mutex m_mutex;
    std::deque<Change> m_changes;
    std::unordered_map<std::wstring, std::wstring> m_names;
};
