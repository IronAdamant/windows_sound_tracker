#include "DeviceWatcher.h"
#include "DebugLog.h"

#include <devpropdef.h>

namespace {

// Defined locally to avoid initguid.h ordering issues across toolchains.
const DEVPROPKEY kDevpkeyName = {{0xb725f130, 0x47ef, 0x101a, {0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac}}, 10};
const DEVPROPKEY kDevpkeyInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

// "\\?\USB#VID_0781&PID_5567#4C53...#{a5dcbf10-...}" -> "USB\VID_0781&PID_5567\4C53..."
std::wstring InstanceIdFromSymbolicLink(const std::wstring& link) {
    std::wstring s = link;
    if (s.rfind(L"\\\\?\\", 0) == 0 || s.rfind(L"\\??\\", 0) == 0) s.erase(0, 4);
    size_t guid = s.rfind(L"#{");
    if (guid != std::wstring::npos) s.erase(guid);
    for (auto& c : s) {
        if (c == L'#') c = L'\\';
    }
    return s;
}

bool IsInterestingDevice(const std::wstring& instanceId) {
    static const wchar_t* prefixes[] = {L"USB", L"HID", L"BTH", L"USBSTOR", L"SCSI", L"STORAGE", L"SWD\\WPDBUSENUM"};
    for (const wchar_t* p : prefixes) {
        if (_wcsnicmp(instanceId.c_str(), p, wcslen(p)) == 0) return true;
    }
    return false;
}

}  // namespace

DeviceWatcher::~DeviceWatcher() { Stop(); }

bool DeviceWatcher::Start() {
    if (m_handle) return true;
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    CONFIGRET cr = CM_Register_Notification(&filter, this, &DeviceWatcher::OnNotification, &m_handle);
    if (cr != CR_SUCCESS) {
        m_handle = nullptr;
        LOG_WARN("device", L"CM_Register_Notification failed (CONFIGRET " << cr << L"); device plug/unplug attribution disabled");
        return false;
    }
    LOG_DEBUG("device", L"Device change notifications registered");
    return true;
}

void DeviceWatcher::Stop() {
    if (m_handle) {
        // Waits for in-flight callbacks, so 'this' stays valid for them.
        CM_Unregister_Notification(m_handle);
        m_handle = nullptr;
    }
}

DWORD CALLBACK DeviceWatcher::OnNotification(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                             PCM_NOTIFY_EVENT_DATA data, DWORD) {
    auto* self = static_cast<DeviceWatcher*>(context);
    if (!self || !data || data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) return ERROR_SUCCESS;
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
        self->Record(true, data->u.DeviceInterface.SymbolicLink);
    } else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        self->Record(false, data->u.DeviceInterface.SymbolicLink);
    }
    return ERROR_SUCCESS;
}

std::wstring DeviceWatcher::LookupName(const std::wstring& instanceId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_names.find(instanceId);
        if (it != m_names.end()) return it->second;
    }
    std::wstring name;
    DEVINST devInst = 0;
    // PHANTOM also finds devices that were just removed.
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()), CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS) {
        wchar_t buf[512] = {};
        ULONG size = sizeof(buf);
        DEVPROPTYPE type = 0;
        if (CM_Get_DevNode_PropertyW(devInst, &kDevpkeyName, &type, reinterpret_cast<PBYTE>(buf), &size, 0) == CR_SUCCESS &&
            type == DEVPROP_TYPE_STRING) {
            name = buf;
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!name.empty()) m_names[instanceId] = name;
    return name;
}

void DeviceWatcher::Record(bool arrival, const wchar_t* symbolicLink) {
    if (!symbolicLink) return;
    std::wstring instanceId;
    if (arrival) {
        wchar_t buf[512] = {};
        ULONG size = sizeof(buf);
        DEVPROPTYPE type = 0;
        if (CM_Get_Device_Interface_PropertyW(symbolicLink, &kDevpkeyInstanceId, &type, reinterpret_cast<PBYTE>(buf), &size, 0) == CR_SUCCESS) {
            instanceId = buf;
        }
    }
    if (instanceId.empty()) instanceId = InstanceIdFromSymbolicLink(symbolicLink);
    std::wstring name = LookupName(instanceId);

    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // One physical device exposes several interfaces; keep one entry per device per action.
        for (const auto& c : m_changes) {
            if (c.instanceId == instanceId && c.arrival == arrival && now - c.tick < 3000) return;
        }
        m_changes.push_back(Change{now, arrival, instanceId, name});
        while (m_changes.size() > 64) m_changes.pop_front();
    }
    if (IsInterestingDevice(instanceId)) {
        LOG_INFO("device", (arrival ? L"Device connected: " : L"Device removed: ") << (name.empty() ? L"(unnamed)" : name)
                               << L" [" << instanceId << L"]");
    } else {
        LOG_DEBUG("device", (arrival ? L"Interface arrival: " : L"Interface removal: ") << (name.empty() ? L"(unnamed)" : name)
                                << L" [" << instanceId << L"]");
    }
}

std::wstring DeviceWatcher::DescribeRecentChange(int withinMs) const {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_mutex);
    const Change* best = nullptr;
    for (auto it = m_changes.rbegin(); it != m_changes.rend(); ++it) {
        if (now - it->tick > static_cast<ULONGLONG>(withinMs)) break;
        if (IsInterestingDevice(it->instanceId)) {
            best = &*it;
            break;
        }
        if (!best) best = &*it;
    }
    if (!best) return std::wstring();
    wchar_t ago[32];
    swprintf_s(ago, L"%.1fs", static_cast<double>(now - best->tick) / 1000.0);
    std::wstring name = best->name.empty() ? best->instanceId : best->name;
    return std::wstring(best->arrival ? L"Connected: " : L"Disconnected: ") + name + L" (" + ago + L" before)";
}
