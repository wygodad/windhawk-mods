// ==WindhawkMod==
// @id              tray-hover-expand
// @name            Tray hover expand
// @description     Open the hidden tray icons flyout on hover instead of clicking the chevron; optionally collapse it when the cursor leaves
// @version         1.0.0
// @author          wygodad
// @github          https://github.com/wygodad
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -luuid -luiautomationcore
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Tray hover expand

Opens the hidden tray icons flyout (the "Show Hidden Icons" chevron) when you
hover the cursor over it, instead of having to click. Optionally collapses it
again once the cursor leaves the opened icons.

It works through UI Automation, so it does not hook internal shell functions —
it is relatively safe and resilient across Windows builds.

## Notes
- If the chevron button is not detected, the matching is name-based. The default
  keywords cover English and Polish; other languages may need the button's name
  added to the `keywords` list in the source.
- If auto-collapse does not work, the flyout window class name may differ on your
  build. Change it in the "Flyout window class" setting.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- autoClose: true
  $name: Collapse when the cursor leaves
  $description: After the cursor leaves the opened icons (and the chevron), the flyout closes itself.
- pollInterval: 50
  $name: Polling interval (ms)
  $description: How often to check the cursor position. Lower = smoother, more CPU.
- grace: 200
  $name: Collapse delay (ms)
  $description: How long the cursor must stay outside the area before the flyout closes (prevents flicker).
- pad: 4
  $name: Hit area padding (pixels)
  $description: Enlarges the hover area around the chevron button.
- flyoutClass: TopLevelWindowForOverflowXamlIsland
  $name: Flyout window class
  $description: Window class name of the opened flyout (used to tell whether the cursor is over the icons). Change it if auto-collapse does not work.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <uiautomation.h>
#include <string>
#include <vector>
#include <algorithm>

// GUID-y na wszelki wypadek (gdyby linker MinGW ich nie dostarczył).
#ifndef __IUIAutomation_FWD_DEFINED__
#error "Brak nagłówków UI Automation"
#endif

struct Settings {
    bool autoClose = true;
    int pollInterval = 50;
    int grace = 200;
    int pad = 4;
    std::wstring flyoutClass = L"TopLevelWindowForOverflowXamlIsland";
    std::vector<std::wstring> keywords = {
        L"ukryte ikony", L"hidden icons", L"rozwiń", L"overflow"
    };
};

static Settings g_settings;
static volatile bool g_running = false;
static HANDLE g_thread = nullptr;

static std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::towlower);
    return r;
}

static bool NameMatches(const std::wstring& name) {
    std::wstring low = ToLower(name);
    for (const auto& k : g_settings.keywords) {
        if (!k.empty() && low.find(ToLower(k)) != std::wstring::npos) return true;
    }
    return false;
}

// ---- Pomocnicze: pobranie wzorców UIA ----

static ExpandCollapseState GetState(IUIAutomationElement* e) {
    IUIAutomationExpandCollapsePattern* p = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(
            UIA_ExpandCollapsePatternId,
            __uuidof(IUIAutomationExpandCollapsePattern), (void**)&p)) && p) {
        ExpandCollapseState s = ExpandCollapseState_Collapsed;
        p->get_CurrentExpandCollapseState(&s);
        p->Release();
        return s;
    }
    // Fallback: stan po widoczności okna flyoutu.
    HWND f = FindWindowW(g_settings.flyoutClass.c_str(), nullptr);
    return (f && IsWindowVisible(f)) ? ExpandCollapseState_Expanded
                                     : ExpandCollapseState_Collapsed;
}

static void DoExpand(IUIAutomationElement* e) {
    IUIAutomationExpandCollapsePattern* p = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(
            UIA_ExpandCollapsePatternId,
            __uuidof(IUIAutomationExpandCollapsePattern), (void**)&p)) && p) {
        p->Expand();
        p->Release();
        return;
    }
    IUIAutomationInvokePattern* inv = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(
            UIA_InvokePatternId,
            __uuidof(IUIAutomationInvokePattern), (void**)&inv)) && inv) {
        inv->Invoke();
        inv->Release();
    }
}

static void DoCollapse(IUIAutomationElement* e) {
    IUIAutomationExpandCollapsePattern* p = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(
            UIA_ExpandCollapsePatternId,
            __uuidof(IUIAutomationExpandCollapsePattern), (void**)&p)) && p) {
        p->Collapse();
        p->Release();
        return;
    }
    // Fallback przy braku ExpandCollapse: ponowne Invoke (toggle).
    IUIAutomationInvokePattern* inv = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(
            UIA_InvokePatternId,
            __uuidof(IUIAutomationInvokePattern), (void**)&inv)) && inv) {
        inv->Invoke();
        inv->Release();
    }
}

// ---- Znajdowanie przycisku chevron ----

static IUIAutomationElement* FindOverflowButton(IUIAutomation* pAuto) {
    HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!hTaskbar) return nullptr;

    IUIAutomationElement* pRoot = nullptr;
    if (FAILED(pAuto->ElementFromHandle(hTaskbar, &pRoot)) || !pRoot) return nullptr;

    IUIAutomationCondition* pCond = nullptr;
    VARIANT v; VariantInit(&v);
    v.vt = VT_I4; v.lVal = UIA_ButtonControlTypeId;
    pAuto->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &pCond);

    IUIAutomationElement* result = nullptr;
    IUIAutomationElementArray* pArr = nullptr;
    if (pCond && SUCCEEDED(pRoot->FindAll(TreeScope_Subtree, pCond, &pArr)) && pArr) {
        int n = 0; pArr->get_Length(&n);
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = nullptr;
            if (SUCCEEDED(pArr->GetElement(i, &e)) && e) {
                BSTR name = nullptr;
                e->get_CurrentName(&name);
                if (name && NameMatches(name)) {
                    SysFreeString(name);
                    result = e;          // zachowujemy referencję
                    break;
                }
                if (name) SysFreeString(name);
                e->Release();
            }
        }
        pArr->Release();
    }
    if (pCond) pCond->Release();
    pRoot->Release();
    return result;
}

static bool PtInRectPad(const RECT& r, POINT pt, int pad) {
    return pt.x >= r.left - pad && pt.x <= r.right + pad &&
           pt.y >= r.top - pad  && pt.y <= r.bottom + pad;
}

static bool CursorOverFlyout(POINT pt) {
    HWND f = FindWindowW(g_settings.flyoutClass.c_str(), nullptr);
    if (!f || !IsWindowVisible(f)) return false;
    RECT r;
    if (!GetWindowRect(f, &r)) return false;
    return PtInRect(&r, pt);
}

// ---- Wątek roboczy ----

static DWORD WINAPI WorkerThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IUIAutomation* pAuto = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr,
            CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&pAuto)) || !pAuto) {
        Wh_Log(L"Failed to create IUIAutomation");
        CoUninitialize();
        return 0;
    }

    IUIAutomationElement* pBtn = nullptr;
    ULONGLONG leftAt = 0;
    ULONGLONG nextRefind = 0;

    while (g_running) {
        ULONGLONG now = GetTickCount64();

        if (!pBtn || now >= nextRefind) {
            // Okresowo odśwież referencję (pasek mógł się przebudować).
            if (pBtn) { pBtn->Release(); pBtn = nullptr; }
            pBtn = FindOverflowButton(pAuto);
            nextRefind = now + 3000;
        }

        if (pBtn) {
            RECT r;
            if (FAILED(pBtn->get_CurrentBoundingRectangle(&r))) {
                pBtn->Release(); pBtn = nullptr;
                Sleep(g_settings.pollInterval);
                continue;
            }

            POINT pt; GetCursorPos(&pt);
            bool overBtn = PtInRectPad(r, pt, g_settings.pad);
            ExpandCollapseState st = GetState(pBtn);
            bool expanded = (st == ExpandCollapseState_Expanded);

            if (overBtn && !expanded) {
                DoExpand(pBtn);
                leftAt = 0;
            }

            if (g_settings.autoClose && expanded) {
                bool overFlyout = CursorOverFlyout(pt);
                if (overBtn || overFlyout) {
                    leftAt = 0;
                } else {
                    if (leftAt == 0) {
                        leftAt = now;
                    } else if (now - leftAt >= (ULONGLONG)g_settings.grace) {
                        DoCollapse(pBtn);
                        leftAt = 0;
                    }
                }
            } else {
                leftAt = 0;
            }
        }

        Sleep(g_settings.pollInterval);
    }

    if (pBtn) pBtn->Release();
    if (pAuto) pAuto->Release();
    CoUninitialize();
    return 0;
}

// ---- Cykl życia moda ----

static void LoadSettings() {
    g_settings.autoClose = Wh_GetIntSetting(L"autoClose") != 0;
    g_settings.pollInterval = (int)Wh_GetIntSetting(L"pollInterval");
    g_settings.grace = (int)Wh_GetIntSetting(L"grace");
    g_settings.pad = (int)Wh_GetIntSetting(L"pad");
    if (g_settings.pollInterval < 10) g_settings.pollInterval = 10;
    if (g_settings.grace < 0) g_settings.grace = 0;

    PCWSTR fc = Wh_GetStringSetting(L"flyoutClass");
    if (fc) { g_settings.flyoutClass = fc; Wh_FreeStringSetting(fc); }
}

BOOL Wh_ModInit() {
    LoadSettings();
    g_running = true;
    g_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}

void Wh_ModUninit() {
    g_running = false;
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}
