// ==WindhawkMod==
// @id              tray-hover-expand
// @name            Tray hover expand
// @description     Open the hidden tray icons flyout on hover instead of clicking the chevron; optionally collapse it when the cursor leaves
// @version         1.3.0
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

## How the chevron is detected
The "Show Hidden Icons" chevron has no language-independent unique identifier on
Windows 11 (it shares AutomationId `SystemTrayIcon` with the clock, volume,
battery, etc., and exposes no ExpandCollapse pattern). So detection is a hybrid:
1. Match the button by name (keywords cover English and Polish by default).
2. If no name matches (other languages), fall back to the leftmost tray button
   with the configured AutomationId, which is normally the chevron.

## Notes
- For unsupported languages, either add the button's name to the `keywords` list
  in the source, or rely on the leftmost-tray-icon fallback.
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
- trayIconAutomationId: SystemTrayIcon
  $name: Tray icon AutomationId (fallback)
  $description: Used only when the chevron is not matched by name (non-English/Polish systems). The leftmost tray button with this AutomationId is then assumed to be the chevron.
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
    std::wstring trayIconAutomationId = L"SystemTrayIcon";
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

    // Strategia hybrydowa:
    //  1) dopasowanie po nazwie (pewne dla znanych języków),
    //  2) fallback: najlewszy przycisk o danym AutomationId (językowo-niezależny,
    //     ale heurystyczny) — używany tylko, gdy nazwa nie pasuje.
    IUIAutomationElement* nameMatch = nullptr;
    IUIAutomationElement* leftmost = nullptr;
    LONG leftmostX = 0;

    IUIAutomationElementArray* pArr = nullptr;
    if (pCond && SUCCEEDED(pRoot->FindAll(TreeScope_Subtree, pCond, &pArr)) && pArr) {
        int n = 0; pArr->get_Length(&n);
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = nullptr;
            if (FAILED(pArr->GetElement(i, &e)) || !e) continue;

            BSTR name = nullptr;
            e->get_CurrentName(&name);
            bool matched = (name && NameMatches(name));
            if (name) SysFreeString(name);
            if (matched) {
                nameMatch = e;           // zachowujemy referencję; nazwa wygrywa
                break;
            }

            // Kandydat do fallbacku: przycisk zasobnika o danym AutomationId.
            BSTR aid = nullptr;
            e->get_CurrentAutomationId(&aid);
            bool isTrayIcon = (aid && g_settings.trayIconAutomationId == aid);
            if (aid) SysFreeString(aid);

            if (isTrayIcon) {
                RECT r;
                if (SUCCEEDED(e->get_CurrentBoundingRectangle(&r)) &&
                    (!leftmost || r.left < leftmostX)) {
                    if (leftmost) leftmost->Release();
                    leftmost = e;        // zachowujemy nowego najlewszego
                    leftmostX = r.left;
                    e = nullptr;
                }
            }
            if (e) e->Release();
        }
        pArr->Release();
    }
    if (pCond) pCond->Release();
    pRoot->Release();

    if (nameMatch) {
        if (leftmost) leftmost->Release();
        return nameMatch;
    }
    return leftmost;   // nullptr, jeśli nic nie pasuje
}

static bool PtInRectPad(const RECT& r, POINT pt, int pad) {
    return pt.x >= r.left - pad && pt.x <= r.right + pad &&
           pt.y >= r.top - pad  && pt.y <= r.bottom + pad;
}

static const ULONGLONG ACTION_COOLDOWN_MS = 300;

// Tani (czysto Win32) test, czy schowek jest otwarty — bez zapytań UIA.
// Chevron i tak nie wspiera ExpandCollapse, więc widoczność okna flyoutu
// jest tym samym sygnałem, którego używał fallback w GetState().
static bool IsFlyoutOpen() {
    HWND f = FindWindowW(g_settings.flyoutClass.c_str(), nullptr);
    return f && IsWindowVisible(f);
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
    RECT cachedRect = {};
    bool haveRect = false;
    ULONGLONG leftAt = 0;
    ULONGLONG nextRefind = 0;
    ULONGLONG nextRectRefresh = 0;
    ULONGLONG lastOpenAt = 0;
    bool overBtnPrev = false;

    while (g_running) {
        ULONGLONG now = GetTickCount64();

        // Drogo i RZADKO: odśwież referencję przycisku (pasek mógł się przebudować).
        if (!pBtn || now >= nextRefind) {
            if (pBtn) { pBtn->Release(); pBtn = nullptr; }
            pBtn = FindOverflowButton(pAuto);
            nextRefind = now + 3000;
            nextRectRefresh = 0;          // wymuś świeży prostokąt poniżej
        }

        if (pBtn) {
            // Drogo i RZADKO: pobierz prostokąt przycisku przez UIA tylko co ~750 ms,
            // a nie co tick. Chevron prawie nigdy nie zmienia pozycji.
            if (now >= nextRectRefresh) {
                RECT r;
                if (SUCCEEDED(pBtn->get_CurrentBoundingRectangle(&r))) {
                    cachedRect = r;
                    haveRect = true;
                } else {
                    pBtn->Release(); pBtn = nullptr; haveRect = false;
                    Sleep(g_settings.pollInterval);
                    continue;
                }
                nextRectRefresh = now + 750;
            }
        }

        if (haveRect) {
            // TANIO i CO TICK: tylko lokalne wywołania Win32.
            POINT pt; GetCursorPos(&pt);
            bool overBtn = PtInRectPad(cachedRect, pt, g_settings.pad);
            bool cooling = (now - lastOpenAt < ACTION_COOLDOWN_MS);

            // Stan schowka sprawdzamy tanio (widoczność okna flyoutu) i tylko
            // gdy jest potrzebny: na zboczu wejścia albo przy auto-zwijaniu.
            bool needState = (overBtn && !overBtnPrev && !cooling) || g_settings.autoClose;
            bool flyoutVisible = needState ? IsFlyoutOpen() : false;

            // Otwórz tylko na zboczu wejścia kursora na przycisk i tylko gdy
            // schowek nie jest otwarty. Chevron działa jak przełącznik, więc
            // każde nadmiarowe Invoke by go zamknęło.
            if (overBtn && !overBtnPrev && !cooling && !flyoutVisible) {
                DoExpand(pBtn);
                lastOpenAt = now;
                leftAt = 0;
                Wh_Log(L"OPEN");
            }
            overBtnPrev = overBtn;

            // Auto-zwijanie po wyjściu kursora poza przycisk i okno schowka.
            if (g_settings.autoClose && flyoutVisible && !cooling) {
                bool overFlyout = CursorOverFlyout(pt);
                if (overBtn || overFlyout) {
                    leftAt = 0;
                } else if (leftAt == 0) {
                    leftAt = now;
                } else if (now - leftAt >= (ULONGLONG)g_settings.grace) {
                    DoCollapse(pBtn);
                    leftAt = 0;
                    Wh_Log(L"CLOSE after grace");
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

    PCWSTR aid = Wh_GetStringSetting(L"trayIconAutomationId");
    if (aid) { g_settings.trayIconAutomationId = aid; Wh_FreeStringSetting(aid); }
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
