#include "hotkey.h"

#include <windows.h>

#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "utils.h"

namespace {

using HotkeyAction = void (*)();

void OnHotkey(HotkeyAction action) {
  if (action) {
    action();
  }
}

void HandleUnmuteRetryTimer();

// Static variables for internal use
bool is_hide = false;
std::vector<HWND> hwnd_list;
std::unordered_map<std::wstring, bool> original_mute_states;
bool saved_any_session = false;
bool had_unmuted_session = false;
std::atomic_bool pending_unmute(false);
HWND unmute_timer_hwnd = nullptr;
// Retry unmute with a fast/slow cadence to catch late audio session creation.  
constexpr UINT_PTR kUnmuteRetryTimerId = 1;
constexpr UINT kUnmuteRetryFastDelayMs = 200;
constexpr int kUnmuteRetryFastMax = 20;
constexpr UINT kUnmuteRetrySlowDelayMs = 2000;
constexpr int kUnmuteRetrySlowMax = 60;
int unmute_retry_left = 0;
int unmute_retry_slow_left = 0;
bool unmute_watch_active = false;
bool unmute_watch_com_initialized = false;
bool unmute_watch_com_should_uninit = false;
IAudioSessionNotification* unmute_watch_notification = nullptr;
std::vector<IAudioSessionManager2*> unmute_watch_managers;

HWND last_active_hwnd = nullptr;
HWND last_focus_hwnd = nullptr;
HWND bosskey_hwnd = nullptr;
ATOM bosskey_window_class = 0;
HotkeyAction bosskey_action = nullptr;
constexpr UINT kBossKeyHotkeyId = 1;

bool IsChromeWindow(HWND hwnd) {
  if (!hwnd) {
    return false;
  }
  wchar_t buff[256];
  GetClassNameW(hwnd, buff, 255);
  if (wcscmp(buff, L"Chrome_WidgetWin_1") != 0) {
    return false;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

bool EnsureBossKeyWindow() {
  if (bosskey_hwnd) {
    return true;
  }
  if (bosskey_window_class == 0) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam,
                        LPARAM lParam) -> LRESULT {
      if (msg == WM_TIMER && wParam == kUnmuteRetryTimerId) {
        HandleUnmuteRetryTimer();
        return 0;
      }
      if (msg == WM_HOTKEY && wParam == kBossKeyHotkeyId) {
        if (bosskey_action) {
          bosskey_action();
        }
        return 0;
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ChromePlusBossKeyWindow";
    bosskey_window_class = RegisterClassExW(&wc);
    if (bosskey_window_class == 0) {
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
      }
      bosskey_window_class = 1;
    }
  }
  bosskey_hwnd = CreateWindowExW(0, L"ChromePlusBossKeyWindow", L"", 0, 0, 0, 0,
                                 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
  return bosskey_hwnd != nullptr;
}

bool IsSameRootWindow(HWND child, HWND root) {
  if (!child || !root) {
    return false;
  }
  return GetAncestor(child, GA_ROOT) == GetAncestor(root, GA_ROOT);
}

HWND GetThreadFocusWindow(HWND root) {
  if (!IsWindow(root)) {
    return nullptr;
  }
  DWORD thread_id = GetWindowThreadProcessId(root, nullptr);
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (!GetGUIThreadInfo(thread_id, &info)) {
    return nullptr;
  }
  HWND focus = info.hwndFocus ? info.hwndFocus : info.hwndActive;
  if (!IsWindow(focus)) {
    return nullptr;
  }
  if (!IsSameRootWindow(focus, root)) {
    return nullptr;
  }
  if (!IsWindowVisible(focus) || !IsWindowEnabled(focus)) {
    return nullptr;
  }
  return focus;
}

HWND FindFocusableChromeChild(HWND parent) {
  struct EnumState {
    HWND best = nullptr;
  };
  EnumState state;
  EnumChildWindows(
      parent,
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        auto* state = reinterpret_cast<EnumState*>(lparam);
        if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) {
          return true;
        }
        wchar_t cls[256];
        GetClassNameW(hwnd, cls, 255);
        if (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0 ||
            wcscmp(cls, L"Chrome_WidgetWin_0") == 0) {
          state->best = hwnd;
          return false;
        }
        if (!state->best) {
          state->best = hwnd;
        }
        return true;
      },
      reinterpret_cast<LPARAM>(&state));
  return state.best;
}

HWND SelectFocusTarget(HWND root, HWND preferred_focus) {
  if (preferred_focus && IsWindow(preferred_focus) &&
      IsSameRootWindow(preferred_focus, root) &&
      IsWindowVisible(preferred_focus) && IsWindowEnabled(preferred_focus)) {
    return preferred_focus;
  }
  HWND candidate = FindFocusableChromeChild(root);
  return candidate ? candidate : root;
}

void ForceForegroundWindow(HWND hwnd, HWND preferred_focus) {
  if (!IsWindow(hwnd)) {
    return;
  }
  if (IsIconic(hwnd)) {
    ShowWindow(hwnd, SW_RESTORE);
  } else {
    ShowWindow(hwnd, SW_SHOW);
  }

  SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  SetForegroundWindow(hwnd);
  SetActiveWindow(hwnd);
  HWND focus_target = SelectFocusTarget(hwnd, preferred_focus);
  SetFocus(focus_target);
}

BOOL CALLBACK SearchChromeWindow(HWND hwnd, LPARAM lparam) {
  if (IsWindowVisible(hwnd)) {
    wchar_t buff[256];
    GetClassNameW(hwnd, buff, 255);
    if (wcscmp(buff, L"Chrome_WidgetWin_1") ==
        0)  // || wcscmp(buff, L"Chrome_WidgetWin_2")==0 || wcscmp(buff,
            // L"SysShadow")==0 )
    {
      DWORD pid;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid == GetCurrentProcessId()) {
        ShowWindow(hwnd, SW_HIDE);
        hwnd_list.emplace_back(hwnd);
      }
    }
  }
  return true;
}

std::vector<DWORD> GetAppPids() {
  std::vector<DWORD> pids;
  wchar_t current_exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, current_exe_path, MAX_PATH);
  wchar_t* exe_name = wcsrchr(current_exe_path, L'\\');
  if (exe_name) {
    ++exe_name;
  } else {
    exe_name = current_exe_path;
  }

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return pids;
  }

  PROCESSENTRY32W pe32;
  pe32.dwSize = sizeof(PROCESSENTRY32W);

  if (Process32FirstW(snapshot, &pe32)) {
    do {
      if (_wcsicmp(pe32.szExeFile, exe_name) == 0) {
        pids.emplace_back(pe32.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &pe32));
  }

  CloseHandle(snapshot);
  return pids;
}

std::optional<std::wstring> GetSessionKey(IAudioSessionControl2* session2) {
  LPWSTR session_key = nullptr;
  if (SUCCEEDED(session2->GetSessionInstanceIdentifier(&session_key)) &&
      session_key) {
    std::wstring key(session_key);
    CoTaskMemFree(session_key);
    return key;
  }
  return std::nullopt;
}

bool ShouldUnmuteUnknownSession() {
  if (!saved_any_session) {
    return true;
  }
  return had_unmuted_session;
}

void ResetMuteStateTracking() {
  original_mute_states.clear();
  saved_any_session = false;
  had_unmuted_session = false;
}

bool IsUnmuteTrackingActive() {
  return unmute_retry_left > 0 || unmute_retry_slow_left > 0 ||
         unmute_watch_active;
}

void ClearMuteStatesIfIdle() {
  if (!IsUnmuteTrackingActive()) {
    ResetMuteStateTracking();
  }
}

void AddAudioDevice(std::vector<IMMDevice*>& devices,
                    std::unordered_set<std::wstring>& seen_ids,
                    IMMDevice* device) {
  if (!device) {
    return;
  }
  LPWSTR device_id = nullptr;
  if (SUCCEEDED(device->GetId(&device_id)) && device_id) {
    if (seen_ids.insert(device_id).second) {
      devices.emplace_back(device);
    } else {
      device->Release();
    }
    CoTaskMemFree(device_id);
  } else {
    device->Release();
  }
}

std::vector<IMMDevice*> CollectAudioDevices(IMMDeviceEnumerator* enumerator) {  
  std::vector<IMMDevice*> devices;
  if (!enumerator) {
    return devices;
  }
  std::unordered_set<std::wstring> seen_ids;
  constexpr ERole kRoles[] = {eConsole, eMultimedia, eCommunications};
  for (auto role : kRoles) {
    IMMDevice* device = nullptr;
    if (SUCCEEDED(
            enumerator->GetDefaultAudioEndpoint(eRender, role, &device))) {
      AddAudioDevice(devices, seen_ids, device);
    }
  }

  IMMDeviceCollection* collection = nullptr;
  if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                               &collection)) &&
      collection) {
    UINT count = 0;
    if (SUCCEEDED(collection->GetCount(&count))) {
      for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (SUCCEEDED(collection->Item(i, &device))) {
          AddAudioDevice(devices, seen_ids, device);
        }
      }
    }
    collection->Release();
  }

  return devices;
}

struct MuteProcessResult {
  bool saw_session = false;
  bool any_mute_state_known = false;
  bool had_muted_session = false;
  bool did_mute = false;
  bool did_unmute = false;
};

void ProcessSessions(IAudioSessionManager2* manager,
                     const std::vector<DWORD>& pids,
                     bool set_mute,
                     bool save_mute_state,
                     bool force_unmute,
                     MuteProcessResult* result) {
  if (!manager) {
    return;
  }
  IAudioSessionEnumerator* session_enumerator = nullptr;
  HRESULT hr = manager->GetSessionEnumerator(&session_enumerator);
  if (FAILED(hr) || !session_enumerator) {
    return;
  }

  int session_count = 0;
  session_enumerator->GetCount(&session_count);
  for (int i = 0; i < session_count; ++i) {
    IAudioSessionControl* session = nullptr;
    hr = session_enumerator->GetSession(i, &session);
    if (FAILED(hr) || !session) {
      continue;
    }
    IAudioSessionControl2* session2 = nullptr;
    hr = session->QueryInterface(__uuidof(IAudioSessionControl2),
                                 (void**)&session2);
    if (SUCCEEDED(hr) && session2) {
      DWORD session_pid = 0;
      if (FAILED(session2->GetProcessId(&session_pid))) {
        session2->Release();
        session->Release();
        continue;
      }

      for (DWORD pid : pids) {
        if (session_pid == pid) {
          if (result) {
            result->saw_session = true;
          }
          auto session_key = GetSessionKey(session2);
          ISimpleAudioVolume* volume = nullptr;
          if (SUCCEEDED(session2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                 (void**)&volume))) {
            BOOL is_muted = FALSE;
            bool mute_known = SUCCEEDED(volume->GetMute(&is_muted));
            if (mute_known && result) {
              result->any_mute_state_known = true;
              if (is_muted == TRUE) {
                result->had_muted_session = true;
              }
            }
            if (save_mute_state) {
              if (mute_known) {
                saved_any_session = true;
                if (is_muted == FALSE) {
                  had_unmuted_session = true;
                }
                if (session_key) {
                  original_mute_states[*session_key] = (is_muted == TRUE);      
                }
              }
            }

            if (set_mute) {
              if (!mute_known || is_muted == FALSE) {
                if (SUCCEEDED(volume->SetMute(TRUE, nullptr)) && result &&
                    (!mute_known || is_muted == FALSE)) {
                  result->did_mute = true;
                }
              }
            } else if (force_unmute) {
              if (!mute_known || is_muted == TRUE) {
                if (SUCCEEDED(volume->SetMute(FALSE, nullptr)) && result &&
                    (!mute_known || is_muted == TRUE)) {
                  result->did_unmute = true;
                }
              }
            } else {
              // Only unmute sessions we muted before. If the session key is not
              // recorded (e.g. session recreated), unmute to avoid stuck
              // system-level mute.
              bool should_unmute = true;
              if (session_key) {
                auto it = original_mute_states.find(*session_key);
                if (it != original_mute_states.end()) {
                  should_unmute = !it->second;
                }
              } else {
                should_unmute = ShouldUnmuteUnknownSession();
              }
              if (should_unmute) {
                if (SUCCEEDED(volume->SetMute(FALSE, nullptr)) && result &&
                    (!mute_known || is_muted == TRUE)) {
                  result->did_unmute = true;
                }
              }
            }
            volume->Release();
          }
          break;
        }
      }
      session2->Release();
    }
    session->Release();
  }

  session_enumerator->Release();
}

MuteProcessResult MuteProcess(const std::vector<DWORD>& pids,
                              bool set_mute,
                              bool save_mute_state = false,
                              bool clear_state = true,
                              bool force_unmute = false) {
  MuteProcessResult result;
  HRESULT hr = CoInitialize(nullptr);
  const bool should_uninit = (hr == S_OK || hr == S_FALSE);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return result;
  }
  IMMDeviceEnumerator* enumerator = nullptr;

  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        IID_PPV_ARGS(&enumerator));
  if (FAILED(hr) || !enumerator) {
    goto Cleanup;
  }
  {
    auto devices = CollectAudioDevices(enumerator);
    for (auto* device_item : devices) {
      IAudioSessionManager2* manager = nullptr;
      hr = device_item->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,   
                                 nullptr, (void**)&manager);
      if (SUCCEEDED(hr) && manager) {
        ProcessSessions(manager, pids, set_mute, save_mute_state,
                        force_unmute, &result);
        manager->Release();
      }
      device_item->Release();
    }
  }

Cleanup:
  if (enumerator) {
    enumerator->Release();
  }
  if (should_uninit) {
    CoUninitialize();
  }

  if (!set_mute && clear_state) {
    ClearMuteStatesIfIdle();
  }
  return result;
}

void StopUnmuteRetries(bool clear_state) {
  KillTimer(unmute_timer_hwnd, kUnmuteRetryTimerId);
  unmute_timer_hwnd = nullptr;
  unmute_retry_left = 0;
  unmute_retry_slow_left = 0;
  if (clear_state) {
    ClearMuteStatesIfIdle();
  }
}

void StartUnmuteRetries() {
  StopUnmuteRetries(false);
  unmute_retry_left = kUnmuteRetryFastMax;
  unmute_retry_slow_left = kUnmuteRetrySlowMax;
  if (unmute_retry_left <= 0 && unmute_retry_slow_left <= 0) {
    return;
  }
  UINT delay =
      (unmute_retry_left > 0) ? kUnmuteRetryFastDelayMs : kUnmuteRetrySlowDelayMs;
  if (bosskey_hwnd && IsWindow(bosskey_hwnd)) {
    unmute_timer_hwnd = bosskey_hwnd;
  } else {
    unmute_timer_hwnd = nullptr;
  }
  if (SetTimer(unmute_timer_hwnd, kUnmuteRetryTimerId, delay, nullptr) == 0) {
    StopUnmuteRetries(true);
  }
}

bool EnsureUnmuteWatchComInitialized() {
  if (unmute_watch_com_initialized) {
    return true;
  }
  HRESULT hr = CoInitialize(nullptr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return false;
  }
  unmute_watch_com_initialized = true;
  unmute_watch_com_should_uninit = (hr == S_OK || hr == S_FALSE);
  return true;
}

class SessionNotification final : public IAudioSessionNotification {
 public:
  SessionNotification() : ref_count_(1) {}

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref_count =
        static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (ref_count == 0) {
      delete this;
    }
    return ref_count;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** ppv_object) override {
    if (!ppv_object) {
      return E_POINTER;
    }
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IAudioSessionNotification)) {
      *ppv_object = static_cast<IAudioSessionNotification*>(this);
      AddRef();
      return S_OK;
    }
    *ppv_object = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnSessionCreated(
      IAudioSessionControl* new_session) override {
    if (!new_session || !unmute_watch_active || is_hide ||
        !pending_unmute.load()) {
      return S_OK;
    }
    IAudioSessionControl2* session2 = nullptr;
    HRESULT hr = new_session->QueryInterface(__uuidof(IAudioSessionControl2),
                                             (void**)&session2);
    if (FAILED(hr) || !session2) {
      return S_OK;
    }
    DWORD session_pid = 0;
    if (FAILED(session2->GetProcessId(&session_pid))) {
      session2->Release();
      return S_OK;
    }
    auto pids = GetAppPids();
    if (std::find(pids.begin(), pids.end(), session_pid) == pids.end()) {
      session2->Release();
      return S_OK;
    }

    ISimpleAudioVolume* volume = nullptr;
    hr = session2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                  (void**)&volume);
    if (SUCCEEDED(hr) && volume) {
      BOOL is_muted = FALSE;
      bool mute_known = SUCCEEDED(volume->GetMute(&is_muted));
      if (mute_known && is_muted == FALSE) {
        pending_unmute.store(false);
      } else if (!mute_known || is_muted == TRUE) {
        if (SUCCEEDED(volume->SetMute(FALSE, nullptr)) &&
            (!mute_known || is_muted == TRUE)) {
          pending_unmute.store(false);
        }
      }
      volume->Release();
    }
    session2->Release();
    return S_OK;
  }

 private:
  LONG ref_count_;
};

void UnregisterUnmuteWatch(bool clear_state) {
  for (auto* manager : unmute_watch_managers) {
    if (manager && unmute_watch_notification) {
      manager->UnregisterSessionNotification(unmute_watch_notification);
    }
    if (manager) {
      manager->Release();
    }
  }
  unmute_watch_managers.clear();
  if (unmute_watch_notification) {
    unmute_watch_notification->Release();
    unmute_watch_notification = nullptr;
  }
  if (unmute_watch_com_initialized && unmute_watch_com_should_uninit) {
    CoUninitialize();
  }
  unmute_watch_com_initialized = false;
  unmute_watch_com_should_uninit = false;
  unmute_watch_active = false;
  if (clear_state) {
    ClearMuteStatesIfIdle();
  }
}

bool RegisterUnmuteWatch() {
  if (unmute_watch_active) {
    return true;
  }
  if (!EnsureUnmuteWatchComInitialized()) {
    return false;
  }
  unmute_watch_notification = new SessionNotification();

  IMMDeviceEnumerator* enumerator = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator));
  if (SUCCEEDED(hr) && enumerator) {
    auto devices = CollectAudioDevices(enumerator);
    for (auto* device_item : devices) {
      IAudioSessionManager2* manager = nullptr;
      hr = device_item->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                 nullptr, (void**)&manager);
      if (SUCCEEDED(hr) && manager) {
        if (SUCCEEDED(
                manager->RegisterSessionNotification(
                    unmute_watch_notification))) {
          unmute_watch_managers.emplace_back(manager);
        } else {
          manager->Release();
        }
      }
      device_item->Release();
    }
    enumerator->Release();
  }
  if (unmute_watch_managers.empty()) {
    UnregisterUnmuteWatch(false);
    return false;
  }
  unmute_watch_active = true;
  return true;
}

void StartUnmuteWatch() {
  UnregisterUnmuteWatch(false);
  RegisterUnmuteWatch();
}

void HandleUnmuteRetryTimer() {
  if (is_hide) {
    StopUnmuteRetries(true);
    return;
  }
  if (!pending_unmute.load()) {
    StopUnmuteRetries(true);
    return;
  }
  auto chrome_pids = GetAppPids();
  auto result = MuteProcess(chrome_pids, false, false, false, true);
  if (result.saw_session) {
    if (result.did_unmute ||
        (result.any_mute_state_known && !result.had_muted_session)) {
      pending_unmute.store(false);
    }
  }
  if (!unmute_watch_active) {
    RegisterUnmuteWatch();
  }
  if (unmute_retry_left > 0) {
    --unmute_retry_left;
    if (unmute_retry_left <= 0 && unmute_retry_slow_left > 0) {
      if (SetTimer(unmute_timer_hwnd, kUnmuteRetryTimerId,
                   kUnmuteRetrySlowDelayMs,
                   nullptr) == 0) {
        StopUnmuteRetries(true);
        return;
      }
    }
  } else if (unmute_retry_slow_left > 0) {
    --unmute_retry_slow_left;
  }
  if (unmute_retry_left <= 0 && unmute_retry_slow_left <= 0) {
    if (pending_unmute.load()) {
      unmute_retry_slow_left = kUnmuteRetrySlowMax;
      if (SetTimer(unmute_timer_hwnd, kUnmuteRetryTimerId,
                   kUnmuteRetrySlowDelayMs,
                   nullptr) == 0) {
        StopUnmuteRetries(true);
      }
      return;
    }
    StopUnmuteRetries(true);
  }
}

void HideAndShow() {
  auto chrome_pids = GetAppPids();
  if (!is_hide) {
    StopUnmuteRetries(false);
    UnregisterUnmuteWatch(false);
    ResetMuteStateTracking();
    HWND foreground = GetForegroundWindow();
    last_active_hwnd = IsChromeWindow(foreground) ? foreground : nullptr;
    last_focus_hwnd =
        last_active_hwnd ? GetThreadFocusWindow(last_active_hwnd) : nullptr;
    original_mute_states.clear();
    EnumWindows(SearchChromeWindow, 0);
    auto result = MuteProcess(chrome_pids, true, true);
    pending_unmute.store(result.did_mute);
  } else {
    for (auto r_iter = hwnd_list.rbegin(); r_iter != hwnd_list.rend();
         ++r_iter) {
      ShowWindow(*r_iter, SW_SHOW);
      SetWindowPos(*r_iter, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
      SetWindowPos(*r_iter, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE);
    }
    HWND target = IsWindow(last_active_hwnd) ? last_active_hwnd
                                             : (hwnd_list.empty()
                                                    ? nullptr
                                                    : hwnd_list.back());
    if (target) {
      ForceForegroundWindow(target, last_focus_hwnd);
    }
    hwnd_list.clear();
    auto result = MuteProcess(chrome_pids, false, false, false, true);
    if (result.saw_session) {
      if (result.did_unmute ||
          (result.any_mute_state_known && !result.had_muted_session)) {
        pending_unmute.store(false);
      }
    }
    if (pending_unmute.load()) {
      StartUnmuteRetries();
      StartUnmuteWatch();
    }
  }
  is_hide = !is_hide;
}

void Hotkey(std::wstring_view keys, HotkeyAction action) {
  if (keys.empty()) {
    return;
  }
  UINT flag = ParseHotkeys(keys.data());
  if (EnsureBossKeyWindow()) {
    bosskey_action = action;
    UnregisterHotKey(bosskey_hwnd, kBossKeyHotkeyId);
    RegisterHotKey(bosskey_hwnd, kBossKeyHotkeyId, LOWORD(flag), HIWORD(flag));
  }

  std::thread th([flag, action]() {
    RegisterHotKey(nullptr, 0, LOWORD(flag), HIWORD(flag));

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
      if (msg.message == WM_TIMER) {
        if (msg.wParam == kUnmuteRetryTimerId) {
          HandleUnmuteRetryTimer();
          continue;
        }
      }
      if (msg.message == WM_HOTKEY) {
        if (bosskey_hwnd && IsWindow(bosskey_hwnd)) {
          PostMessageW(bosskey_hwnd, WM_HOTKEY, kBossKeyHotkeyId, 0);
        } else {
          OnHotkey(action);
        }
      }
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });
  th.detach();
}

}  // anonymous namespace

UINT ParseTranslateKey() {
  const auto& translate_key = config.GetTranslateKey();
  if (translate_key.empty()) {
    return 0;
  }
  return ParseHotkeys(translate_key.c_str());
}

UINT ParseSwitchToPrevKey() {
  const auto& switch_to_prev = config.GetSwitchToPrevKey();
  if (switch_to_prev.empty()) {
    return 0;
  }
  return ParseHotkeys(switch_to_prev.c_str());
}

UINT ParseSwitchToNextKey() {
  const auto& switch_to_next = config.GetSwitchToNextKey();
  if (switch_to_next.empty()) {
    return 0;
  }
  return ParseHotkeys(switch_to_next.c_str());
}

void GetHotkey() {
  const auto& boss_key = config.GetBossKey();
  if (!boss_key.empty()) {
    Hotkey(boss_key, HideAndShow);
  }
}
