#include "tabbookmark.h"

#include <windows.h>
#include <vector>

#include "config.h"
#include "hotkey.h"
#include "iaccessible.h"
#include "utils.h"

namespace {

HHOOK mouse_hook = nullptr;
static POINT lbutton_down_point = {-1, -1};

struct DragNewTabState {
  int mode = 0;
  HWND hwnd = nullptr;
  POINT drop_point = {-1, -1};
  int start_tab_count = 0;
  NodePtr start_selected_tab = nullptr;
  int start_selected_index = -1;
  std::vector<NodePtr> start_tabs;
  int check_attempts = 0;
  bool armed = false;
  bool pending = false;
};

DragNewTabState drag_new_tab_state;
UINT_PTR drag_new_tab_timer = 0;
UINT_PTR drag_new_tab_restore_timer = 0;
NodePtr drag_new_tab_restore_tab = nullptr;
int drag_new_tab_restore_attempts = 0;

constexpr UINT kDragNewTabCheckIntervalMs = 80;
constexpr int kDragNewTabMaxAttempts = 12;
constexpr int kDragNewTabRestoreAttempts = 4;

#define KEY_PRESSED 0x8000
bool IsPressed(int key) {
  return key && (::GetKeyState(key) & KEY_PRESSED) != 0;
}

// Compared with `IsOnlyOneTab`, this function additionally implements tick
// fault tolerance to prevent users from directly closing the window when
// they click too fast.
bool IsNeedKeep(const NodePtr& top_container_view) {
  if (!config.IsKeepLastTab()) {
    return false;
  }

  auto tab_count = GetTabCount(top_container_view);
  bool keep_tab = (tab_count == 1);

  static auto last_closing_tab_tick = GetTickCount64();
  auto tick = GetTickCount64() - last_closing_tab_tick;
  last_closing_tab_tick = GetTickCount64();

  if (tick > 50 && tick <= 250 && tab_count == 2) {
    keep_tab = true;
  }

  return keep_tab;
}

// When `top_container_view` is not found, the find-in-page bar may be open
// and focused. Use `IsOnFindBarPane` to check if the click occurred on the
// bar. If so, return nullptr to avoid interfering with find operations
// (#157). Otherwise, close the bar and retry finding `top_container_view`
// to fix issues where double-click and right-click close actions fail when
// the bar is open (#187). Closing the bar typically has no side effects,
// except that clicks on other tabs or bookmarks will also dismiss the bar
// when it is open.
NodePtr HandleFindBar(HWND hwnd, POINT pt) {
  NodePtr top_container_view = GetTopContainerView(hwnd);
  if (!top_container_view) {
    if (IsOnFindBarPane(pt)) {
      return nullptr;
    }
    ExecuteCommand(IDC_CLOSE_FIND_OR_STOP, hwnd);
    top_container_view = GetTopContainerView(hwnd);
    if (!top_container_view) {
      return nullptr;
    }
  }
  return top_container_view;
}

// Use the mouse wheel to switch tabs
bool HandleMouseWheel(LPARAM lParam, PMOUSEHOOKSTRUCT pmouse) {
  if (!config.IsWheelTab() && !config.IsWheelTabWhenPressRightButton()) {
    return false;
  }

  HWND hwnd = GetFocus();
  NodePtr top_container_view = GetTopContainerView(hwnd);

  PMOUSEHOOKSTRUCTEX pwheel = reinterpret_cast<PMOUSEHOOKSTRUCTEX>(lParam);
  int zDelta = GET_WHEEL_DELTA_WPARAM(pwheel->mouseData);

  auto switch_tabs = [&]() {
    hwnd = GetTopWnd(hwnd);
    if (zDelta > 0) {
      ExecuteCommand(IDC_SELECT_PREVIOUS_TAB, hwnd);
    } else {
      ExecuteCommand(IDC_SELECT_NEXT_TAB, hwnd);
    }
    return true;
  };

  // If the mouse wheel is used to switch tabs when the mouse is on the tab bar.
  if (config.IsWheelTab() && IsOnTheTabBar(top_container_view, pmouse->pt)) {
    return switch_tabs();
  }

  // If it is used to switch tabs when the right button is held.
  if (config.IsWheelTabWhenPressRightButton() && IsPressed(VK_RBUTTON)) {
    return switch_tabs();
  }

  return false;
}

// Double-click to close tab.
bool HandleDoubleClick(PMOUSEHOOKSTRUCT pmouse) {
  if (!config.IsDoubleClickClose()) {
    return false;
  }

  POINT pt = pmouse->pt;
  HWND hwnd = WindowFromPoint(pt);
  NodePtr top_container_view = HandleFindBar(hwnd, pt);
  if (!top_container_view) {
    return false;
  }

  bool is_on_one_tab = IsOnOneTab(top_container_view, pt);
  bool is_on_close_button = IsOnCloseButton(top_container_view, pt);
  if (!is_on_one_tab || is_on_close_button) {
    return false;
  }

  if (IsOnlyOneTab(top_container_view)) {
    ExecuteCommand(IDC_NEW_TAB, hwnd);
    ExecuteCommand(IDC_WINDOW_CLOSE_OTHER_TABS, hwnd);
  } else {
    ExecuteCommand(IDC_CLOSE_TAB, hwnd);
  }
  return true;
}

// Right-click to close tab (Hold Shift to show the original menu).
bool HandleRightClick(PMOUSEHOOKSTRUCT pmouse) {
  if (IsPressed(VK_SHIFT) || !config.IsRightClickClose()) {
    return false;
  }

  POINT pt = pmouse->pt;
  HWND hwnd = WindowFromPoint(pt);
  NodePtr top_container_view = HandleFindBar(hwnd, pt);
  if (!top_container_view) {
    return false;
  }

  if (IsOnOneTab(top_container_view, pt)) {
    if (IsNeedKeep(top_container_view)) {
      ExecuteCommand(IDC_NEW_TAB, hwnd);
      ExecuteCommand(IDC_WINDOW_CLOSE_OTHER_TABS, hwnd);
    } else {
      // Attempt new SendKey function which includes a `dwExtraInfo`
      // value (GetMagicCode()).
      SendKey(VK_MBUTTON);
    }
    return true;
  }
  return false;
}

// Preserve the last tab when the middle button is clicked on the tab.
bool HandleMiddleClick(PMOUSEHOOKSTRUCT pmouse) {
  POINT pt = pmouse->pt;
  HWND hwnd = WindowFromPoint(pt);
  NodePtr top_container_view = HandleFindBar(hwnd, pt);
  if (!top_container_view) {
    return false;
  }

  bool is_on_one_tab = IsOnOneTab(top_container_view, pt);
  bool keep_tab = IsNeedKeep(top_container_view);

  if (is_on_one_tab && keep_tab) {
    ExecuteCommand(IDC_NEW_TAB, hwnd);
    ExecuteCommand(IDC_WINDOW_CLOSE_OTHER_TABS, hwnd);
    return true;
  }

  return false;
}

// Check if mouse movement is a drag operation.
// Since `MouseProc` hook doesn't handle any drag-related events,
// this detection can return early to avoid interference.
bool HandleDrag(PMOUSEHOOKSTRUCT pmouse) {
  // Add drag detection logic for
  // https://github.com/Bush2021/chrome_plus/issues/152
  static int dragThresholdX = GetSystemMetrics(SM_CXDRAG);
  static int dragThresholdY = GetSystemMetrics(SM_CYDRAG);
  if (lbutton_down_point.x < 0 || lbutton_down_point.y < 0) {
    return false;
  }
  int dx = pmouse->pt.x - lbutton_down_point.x;
  int dy = pmouse->pt.y - lbutton_down_point.y;
  return (abs(dx) > dragThresholdX || abs(dy) > dragThresholdY);
}

bool IsDragNewTabEnabled() {
  int mode = config.GetDragNewTabMode();
  return mode == 1 || mode == 2;
}

void ResetDragNewTabState() {
  drag_new_tab_state.mode = 0;
  drag_new_tab_state.hwnd = nullptr;
  drag_new_tab_state.drop_point = {-1, -1};
  drag_new_tab_state.start_tab_count = 0;
  drag_new_tab_state.start_selected_tab = nullptr;
  drag_new_tab_state.start_selected_index = -1;
  drag_new_tab_state.start_tabs.clear();
  drag_new_tab_state.check_attempts = 0;
  drag_new_tab_state.armed = false;
  drag_new_tab_state.pending = false;
  if (drag_new_tab_restore_timer != 0) {
    KillTimer(nullptr, drag_new_tab_restore_timer);
    drag_new_tab_restore_timer = 0;
  }
  drag_new_tab_restore_tab = nullptr;
  drag_new_tab_restore_attempts = 0;
}

NodePtr FindNewTabAfterDrag(const std::vector<NodePtr>& tabs) {
  if (drag_new_tab_state.start_tabs.empty()) {
    return nullptr;
  }
  for (const auto& tab : tabs) {
    bool existed = false;
    for (const auto& old_tab : drag_new_tab_state.start_tabs) {
      if (tab.Get() == old_tab.Get()) {
        existed = true;
        break;
      }
    }
    if (!existed) {
      return tab;
    }
  }
  return nullptr;
}

int GetTabIndex(const std::vector<NodePtr>& tabs, const NodePtr& target_tab) {  
  if (!target_tab) {
    return -1;
  }
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].Get() == target_tab.Get()) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool IsTabInList(const std::vector<NodePtr>& tabs, const NodePtr& target_tab) {
  return GetTabIndex(tabs, target_tab) >= 0;
}

bool InitDragNewTabState(HWND hwnd, const NodePtr& top_container_view) {
  int mode = config.GetDragNewTabMode();
  if (mode != 1 && mode != 2) {
    ResetDragNewTabState();
    return false;
  }
  if (!hwnd || !top_container_view) {
    ResetDragNewTabState();
    return false;
  }

  drag_new_tab_state.mode = mode;
  drag_new_tab_state.hwnd = hwnd;
  drag_new_tab_state.start_tab_count = GetTabCount(top_container_view);
  drag_new_tab_state.start_selected_tab = GetSelectedTab(top_container_view);
  drag_new_tab_state.start_tabs = GetTabs(top_container_view);
  drag_new_tab_state.start_selected_index =
      GetTabIndex(drag_new_tab_state.start_tabs,
                  drag_new_tab_state.start_selected_tab);
  return !drag_new_tab_state.start_tabs.empty();
}

NodePtr GetTabByIndex(const std::vector<NodePtr>& tabs, int index) {
  if (index < 0 || index >= static_cast<int>(tabs.size())) {
    return nullptr;
  }
  return tabs[index];
}

int GetMoveStepsToEnd(const std::vector<NodePtr>& tabs,
                      const NodePtr& target_tab) {
  int index = GetTabIndex(tabs, target_tab);
  if (index < 0) {
    return 0;
  }
  int last_index = static_cast<int>(tabs.size()) - 1;
  return last_index > index ? last_index - index : 0;
}

NodePtr ResolveRestoreTab(const std::vector<NodePtr>& tabs) {
  if (drag_new_tab_state.start_selected_tab) {
    int index = GetTabIndex(tabs, drag_new_tab_state.start_selected_tab);
    if (index >= 0) {
      return tabs[index];
    }
  }
  return GetTabByIndex(tabs, drag_new_tab_state.start_selected_index);
}

void MoveSelectedTabToEnd(HWND hwnd, int steps) {
  if (!hwnd || steps <= 0) {
    return;
  }
  for (int i = 0; i < steps; ++i) {
    ExecuteCommand(IDC_MOVE_TAB_NEXT, hwnd);
  }
}

void CALLBACK DragNewTabRestoreTimerProc(HWND, UINT, UINT_PTR timer_id,
                                         DWORD) {
  if (drag_new_tab_restore_attempts <= 0) {
    KillTimer(nullptr, timer_id);
    drag_new_tab_restore_timer = 0;
    drag_new_tab_restore_tab = nullptr;
    return;
  }

  --drag_new_tab_restore_attempts;
  if (!drag_new_tab_state.hwnd) {
    KillTimer(nullptr, timer_id);
    drag_new_tab_restore_timer = 0;
    drag_new_tab_restore_tab = nullptr;
    return;
  }
  NodePtr top_container_view = GetTopContainerView(drag_new_tab_state.hwnd);
  if (!top_container_view) {
    return;
  }
  auto tabs = GetTabs(top_container_view);
  NodePtr restore_tab = ResolveRestoreTab(tabs);
  if (!restore_tab) {
    return;
  }
  NodePtr selected_tab = GetSelectedTab(top_container_view);
  if (!selected_tab || selected_tab.Get() != restore_tab.Get()) {
    SelectTab(restore_tab);
  }
  if (drag_new_tab_restore_attempts <= 0) {
    KillTimer(nullptr, timer_id);
    drag_new_tab_restore_timer = 0;
    drag_new_tab_restore_tab = nullptr;
  }
}

void QueueDragNewTabRestore(const NodePtr& tab) {
  if (drag_new_tab_restore_timer != 0) {
    KillTimer(nullptr, drag_new_tab_restore_timer);
    drag_new_tab_restore_timer = 0;
  }
  drag_new_tab_restore_tab = nullptr;
  drag_new_tab_restore_attempts = 0;
  if (!tab) {
    return;
  }
  drag_new_tab_restore_tab = tab;
  drag_new_tab_restore_attempts = kDragNewTabRestoreAttempts;
  drag_new_tab_restore_timer = SetTimer(nullptr, 0, kDragNewTabCheckIntervalMs,
                                        DragNewTabRestoreTimerProc);
}

void CALLBACK DragNewTabTimerProc(HWND, UINT, UINT_PTR timer_id, DWORD) {       
  KillTimer(nullptr, timer_id);
  drag_new_tab_timer = 0;

  if (!drag_new_tab_state.pending) {
    return;
  }
  if (drag_new_tab_state.check_attempts <= 0) {
    ResetDragNewTabState();
    return;
  }
  --drag_new_tab_state.check_attempts;

  if (drag_new_tab_state.mode != 1 && drag_new_tab_state.mode != 2) {
    ResetDragNewTabState();
    return;
  }

  NodePtr top_container_view = GetTopContainerView(drag_new_tab_state.hwnd);    
  if (!top_container_view) {
    ResetDragNewTabState();
    return;
  }
  if (drag_new_tab_state.start_tabs.empty()) {
    ResetDragNewTabState();
    return;
  }

  auto tabs = GetTabs(top_container_view);
  NodePtr selected_tab = GetSelectedTab(top_container_view);
  NodePtr new_tab = FindNewTabAfterDrag(tabs);
  if (!new_tab && selected_tab &&
      !IsTabInList(drag_new_tab_state.start_tabs, selected_tab)) {
    new_tab = selected_tab;
  }

  if (!new_tab) {
    drag_new_tab_timer = SetTimer(nullptr, 0, kDragNewTabCheckIntervalMs,
                                  DragNewTabTimerProc);
    return;
  }

  int move_steps = GetMoveStepsToEnd(tabs, new_tab);
  auto ensure_selected = [&](const NodePtr& tab) -> bool {
    if (!tab) {
      return false;
    }
    if (selected_tab && selected_tab.Get() == tab.Get()) {
      return true;
    }
    if (!SelectTab(tab)) {
      return false;
    }
    NodePtr now_selected = GetSelectedTab(top_container_view);
    return now_selected && now_selected.Get() == tab.Get();
  };

  bool new_tab_selected = ensure_selected(new_tab);
  if (!new_tab_selected) {
    if (move_steps > 0 || drag_new_tab_state.mode == 1) {
      drag_new_tab_timer = SetTimer(nullptr, 0, kDragNewTabCheckIntervalMs,
                                    DragNewTabTimerProc);
      return;
    }
  }
  if (move_steps > 0 && new_tab_selected) {
    MoveSelectedTabToEnd(drag_new_tab_state.hwnd, move_steps);
    tabs = GetTabs(top_container_view);
  }

  if (drag_new_tab_state.mode == 2) {
    NodePtr restore_tab = ResolveRestoreTab(tabs);
    if (restore_tab &&
        (!selected_tab || selected_tab.Get() != restore_tab.Get() ||
         new_tab_selected)) {
      SelectTab(restore_tab);
      QueueDragNewTabRestore(restore_tab);
    }
  } else if (!new_tab_selected) {
    SelectTab(new_tab);
  }

  drag_new_tab_state.pending = false;
  drag_new_tab_state.check_attempts = 0;
  drag_new_tab_state.start_tabs.clear();
  drag_new_tab_state.armed = false;
}

void QueueDragNewTabCheck(HWND hwnd, const NodePtr& top_container_view,
                          POINT pt) {
  int mode = config.GetDragNewTabMode();
  if (mode != 1 && mode != 2) {
    ResetDragNewTabState();
    return;
  }
  drag_new_tab_state.mode = mode;
  if (drag_new_tab_state.start_tabs.empty() ||
      drag_new_tab_state.hwnd != hwnd) {
    if (!InitDragNewTabState(hwnd, top_container_view)) {
      return;
    }
  }

  if (drag_new_tab_restore_timer != 0) {
    KillTimer(nullptr, drag_new_tab_restore_timer);
    drag_new_tab_restore_timer = 0;
  }
  drag_new_tab_restore_tab = nullptr;
  drag_new_tab_restore_attempts = 0;
  drag_new_tab_state.drop_point = pt;
  drag_new_tab_state.pending = true;
  drag_new_tab_state.check_attempts = kDragNewTabMaxAttempts;

  if (drag_new_tab_timer != 0) {
    KillTimer(nullptr, drag_new_tab_timer);
    drag_new_tab_timer = 0;
  }
  // Delay the check to allow Chrome to finish the drag-drop tab creation.      
  drag_new_tab_timer =
      SetTimer(nullptr, 0, kDragNewTabCheckIntervalMs, DragNewTabTimerProc);
}

// Open bookmarks in a new tab.
bool HandleBookmark(PMOUSEHOOKSTRUCT pmouse) {
  int mode = config.GetBookmarkNewTabMode();
  if (IsPressed(VK_CONTROL) || IsPressed(VK_SHIFT) || mode == 0) {
    return false;
  }

  POINT pt = pmouse->pt;
  HWND hwnd = WindowFromPoint(pt);

  if (!IsOnBookmark(hwnd, pt)) {
    return false;
  }

  if (IsOnExpandedList(hwnd, pt)) {
    // This is only used to determine the expanded dropdown menu of the address
    // bar. When the mouse clicks on it, it may penetrate through to the
    // background, causing a misjudgment that it is on the bookmark. Related
    // issue: https://github.com/Bush2021/chrome_plus/issues/162
    return false;
  }

  NodePtr top_container_view = GetTopContainerView(
      GetFocus());  // Must use `GetFocus()`, otherwise when opening bookmarks
                    // in a bookmark folder (and similar expanded menus),
                    // `top_container_view` cannot be obtained, making it
                    // impossible to correctly determine `is_on_new_tab`. See
                    // #98.

  if (!IsOnNewTab(top_container_view)) {
    if (mode == 1) {
      SendKey(VK_MBUTTON, VK_SHIFT);
    } else if (mode == 2) {
      SendKey(VK_MBUTTON);
    }
    return true;
  }
  return false;
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode != HC_ACTION) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  PMOUSEHOOKSTRUCT pmouse = reinterpret_cast<PMOUSEHOOKSTRUCT>(lParam);

  // Defining a `dwExtraInfo` value to prevent hook the message sent by
  // Chrome++ itself.
  if (pmouse->dwExtraInfo == GetMagicCode()) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  if (wParam == WM_MOUSEMOVE || wParam == WM_NCMOUSEMOVE) {
    if (IsDragNewTabEnabled() && IsPressed(VK_LBUTTON)) {
      POINT pt = pmouse->pt;
      HWND hwnd = WindowFromPoint(pt);
      hwnd = GetTopWnd(hwnd);
      NodePtr top_container_view = GetTopContainerView(hwnd);
      if (top_container_view && IsOnTheTabBar(top_container_view, pt)) {
        drag_new_tab_state.armed = true;
        if (drag_new_tab_state.start_tabs.empty() ||
            drag_new_tab_state.hwnd != hwnd) {
          InitDragNewTabState(hwnd, top_container_view);
        }
      }
    }
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  static bool wheel_tab_ing_with_rbutton = false;
  bool handled = false;
  switch (wParam) {
    case WM_LBUTTONDOWN:
      // Simply record the position of `LBUTTONDOWN` for drag detection
      lbutton_down_point = pmouse->pt;
      drag_new_tab_state.armed = false;
      if (drag_new_tab_timer != 0) {
        KillTimer(nullptr, drag_new_tab_timer);
        drag_new_tab_timer = 0;
      }
      drag_new_tab_state.pending = false;
      drag_new_tab_state.check_attempts = 0;
      drag_new_tab_state.start_tabs.clear();
      break;
    case WM_LBUTTONUP:
      if (IsDragNewTabEnabled()) {
        POINT pt = pmouse->pt;
        HWND hwnd = WindowFromPoint(pt);
        hwnd = GetTopWnd(hwnd);
        NodePtr top_container_view = GetTopContainerView(hwnd);
        if (top_container_view && IsOnTheTabBar(top_container_view, pt) &&
            !IsOnNewTabButton(top_container_view, pt) &&
            (drag_new_tab_state.armed || HandleDrag(pmouse))) {
          QueueDragNewTabCheck(hwnd, top_container_view, pt);
          drag_new_tab_state.armed = false;
          lbutton_down_point = {-1, -1};
          break;
        }
      }
      drag_new_tab_state.armed = false;
      lbutton_down_point = {-1, -1};
      if (HandleBookmark(pmouse)) {
        handled = true;
      }
      break;
    case WM_RBUTTONUP:
      if (wheel_tab_ing_with_rbutton) {
        // Swallow the first RBUTTONUP that follows a wheel-based tab switch to
        // suppress Chrome's context menu; the RBUTTONUP arrives after
        // WM_MOUSEWHEEL.
        wheel_tab_ing_with_rbutton = false;
        handled = true;
      } else if (HandleRightClick(pmouse)) {
        handled = true;
      }
      break;
    case WM_MOUSEWHEEL:
      if (HandleMouseWheel(lParam, pmouse)) {
        // Mark it true only when a tab switch is performed via mouse wheel with
        // right button pressed. Otherwise, normal mouse wheel to switch tabs
        // will swallow irrelevant RBUTTONUP events, causing #198.
        wheel_tab_ing_with_rbutton = IsPressed(VK_RBUTTON);
        handled = true;
      }
      break;
    case WM_LBUTTONDBLCLK:
      if (HandleDoubleClick(pmouse)) {
        // Do not return 1. Returning 1 could cause the keep_tab to fail
        // or trigger double-click operations consecutively when the user
        // double-clicks on the tab page rapidly and repeatedly.
      }
      break;
    case WM_MBUTTONUP:
      if (HandleMiddleClick(pmouse)) {
        handled = true;
      }
      break;
  }
  if (handled) {
    return 1;  // Swallow the event
  }
  return CallNextHookEx(mouse_hook, nCode, wParam, lParam);  // Pass
}

int HandleKeepTab(WPARAM wParam) {
  if (!(wParam == 'W' && IsPressed(VK_CONTROL) && !IsPressed(VK_SHIFT)) &&
      !(wParam == VK_F4 && IsPressed(VK_CONTROL))) {
    return 0;
  }

  HWND hwnd = GetFocus();
  if (GetChromeWidgetWin(hwnd) == nullptr) {
    return 0;
  }

  if (IsFullScreen(hwnd)) {
    // Have to exit full screen to find the tab.
    ExecuteCommand(IDC_FULLSCREEN, hwnd);
  }

  HWND tmp_hwnd = hwnd;
  hwnd = GetAncestor(tmp_hwnd, GA_ROOTOWNER);
  ExecuteCommand(IDC_CLOSE_FIND_OR_STOP, tmp_hwnd);

  NodePtr top_container_view = GetTopContainerView(hwnd);
  if (!IsNeedKeep(top_container_view)) {
    return 0;
  }

  ExecuteCommand(IDC_NEW_TAB, hwnd);
  ExecuteCommand(IDC_WINDOW_CLOSE_OTHER_TABS, hwnd);
  return 1;
}

int HandleOpenUrlNewTab(WPARAM wParam) {
  int mode = config.GetOpenUrlNewTabMode();
  if (!(mode != 0 && wParam == VK_RETURN && !IsPressed(VK_MENU))) {
    return 0;
  }

  NodePtr top_container_view = GetTopContainerView(GetForegroundWindow());
  if (IsOmniboxFocus(top_container_view) && !IsOnNewTab(top_container_view)) {
    if (mode == 1) {
      SendKey(VK_MENU, VK_RETURN);
    } else if (mode == 2) {
      SendKey(VK_SHIFT, VK_MENU, VK_RETURN);
    }
    return 1;
  }
  return 0;
}

bool IsHotkeyMatch(UINT hotkey, WPARAM wParam) {
  if (hotkey == 0) {
    return false;
  }

  auto vk = HIWORD(hotkey);
  auto modifiers = LOWORD(hotkey);
  if ((modifiers & MOD_SHIFT) && !IsPressed(VK_SHIFT)) {
    return false;
  }
  if ((modifiers & MOD_CONTROL) && !IsPressed(VK_CONTROL)) {
    return false;
  }
  if ((modifiers & MOD_ALT) && !IsPressed(VK_MENU)) {
    return false;
  }
  if ((modifiers & MOD_WIN) && !IsPressed(VK_LWIN) && !IsPressed(VK_RWIN)) {
    return false;
  }
  if (wParam != vk) {
    return false;
  }

  return true;
}

int HandleTranslateKey(WPARAM wParam) {
  auto hotkey = ParseTranslateKey();
  if (!IsHotkeyMatch(hotkey, wParam)) {
    return 0;
  }

  ExecuteCommand(IDC_SHOW_TRANSLATE);
  keybd_event(VK_RIGHT, 0, 0, 0);
  keybd_event(VK_RIGHT, 0, KEYEVENTF_KEYUP, 0);
  return 1;
}

int HandleSwitchTabKey(WPARAM wParam) {
  auto prev_hotkey = ParseSwitchToPrevKey();
  if (IsHotkeyMatch(prev_hotkey, wParam)) {
    ExecuteCommand(IDC_SELECT_PREVIOUS_TAB);
    return 1;
  }

  auto next_hotkey = ParseSwitchToNextKey();
  if (IsHotkeyMatch(next_hotkey, wParam)) {
    ExecuteCommand(IDC_SELECT_NEXT_TAB);
    return 1;
  }

  return 0;
}

HHOOK keyboard_hook = nullptr;
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && !(lParam & 0x80000000))  // pressed
  {
    if (HandleKeepTab(wParam) != 0) {
      return 1;
    }

    if (HandleOpenUrlNewTab(wParam) != 0) {
      return 1;
    }

    if (HandleTranslateKey(wParam) != 0) {
      return 1;
    }

    if (HandleSwitchTabKey(wParam) != 0) {
      return 1;
    }
  }
  return CallNextHookEx(keyboard_hook, nCode, wParam, lParam);
}

}  // namespace

void TabBookmark() {
  mouse_hook =
      SetWindowsHookEx(WH_MOUSE, MouseProc, hInstance, GetCurrentThreadId());
  keyboard_hook = SetWindowsHookEx(WH_KEYBOARD, KeyboardProc, hInstance,
                                   GetCurrentThreadId());
}
