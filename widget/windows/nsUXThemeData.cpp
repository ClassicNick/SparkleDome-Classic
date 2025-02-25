/* vim: se cin sw=2 ts=2 et : */
/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Util.h"

#include "nsUXThemeData.h"
#include "nsDebug.h"
#include "nsToolkit.h"
#include "WinUtils.h"
#include "nsUXThemeConstants.h"

using namespace mozilla;
using namespace mozilla::widget;

const PRUnichar
nsUXThemeData::kThemeLibraryName[] = L"uxtheme.dll";
const PRUnichar
nsUXThemeData::kDwmLibraryName[] = L"dwmapi.dll";

HANDLE
nsUXThemeData::sThemes[eUXNumClasses];

HMODULE
nsUXThemeData::sThemeDLL = NULL;
HMODULE
nsUXThemeData::sDwmDLL = NULL;

BOOL
nsUXThemeData::sFlatMenus = FALSE;

bool nsUXThemeData::sTitlebarInfoPopulatedAero = false;
bool nsUXThemeData::sTitlebarInfoPopulatedThemed = false;
SIZE nsUXThemeData::sCommandButtons[4];

nsUXThemeData::OpenThemeDataPtr nsUXThemeData::openTheme = NULL;
nsUXThemeData::CloseThemeDataPtr nsUXThemeData::closeTheme = NULL;
nsUXThemeData::DrawThemeBackgroundPtr nsUXThemeData::drawThemeBG = NULL;
nsUXThemeData::DrawThemeEdgePtr nsUXThemeData::drawThemeEdge = NULL;
nsUXThemeData::GetThemeContentRectPtr nsUXThemeData::getThemeContentRect = NULL;
nsUXThemeData::GetThemeBackgroundRegionPtr nsUXThemeData::getThemeBackgroundRegion = NULL;
nsUXThemeData::GetThemeMetricPtr nsUXThemeData::getThemeMetric = NULL;
nsUXThemeData::GetThemePartSizePtr nsUXThemeData::getThemePartSize = NULL;
nsUXThemeData::GetThemeSysFontPtr nsUXThemeData::getThemeSysFont = NULL;
nsUXThemeData::GetThemeColorPtr nsUXThemeData::getThemeColor = NULL;
nsUXThemeData::GetThemeMarginsPtr nsUXThemeData::getThemeMargins = NULL;
nsUXThemeData::IsAppThemedPtr nsUXThemeData::isAppThemed = NULL;
nsUXThemeData::GetCurrentThemeNamePtr nsUXThemeData::getCurrentThemeName = NULL;
nsUXThemeData::GetThemeSysColorPtr nsUXThemeData::getThemeSysColor = NULL;
nsUXThemeData::IsThemeBackgroundPartiallyTransparentPtr nsUXThemeData::isThemeBackgroundPartiallyTransparent = NULL;

nsUXThemeData::DwmExtendFrameIntoClientAreaProc nsUXThemeData::dwmExtendFrameIntoClientAreaPtr = NULL;
nsUXThemeData::DwmIsCompositionEnabledProc nsUXThemeData::dwmIsCompositionEnabledPtr = NULL;
nsUXThemeData::DwmSetIconicThumbnailProc nsUXThemeData::dwmSetIconicThumbnailPtr = NULL;
nsUXThemeData::DwmSetIconicLivePreviewBitmapProc nsUXThemeData::dwmSetIconicLivePreviewBitmapPtr = NULL;
nsUXThemeData::DwmGetWindowAttributeProc nsUXThemeData::dwmGetWindowAttributePtr = NULL;
nsUXThemeData::DwmSetWindowAttributeProc nsUXThemeData::dwmSetWindowAttributePtr = NULL;
nsUXThemeData::DwmInvalidateIconicBitmapsProc nsUXThemeData::dwmInvalidateIconicBitmapsPtr = NULL;
nsUXThemeData::DwmDefWindowProcProc nsUXThemeData::dwmDwmDefWindowProcPtr = NULL;

void
nsUXThemeData::Teardown() {
  Invalidate();
  if(sThemeDLL)
    FreeLibrary(sThemeDLL);
  if(sDwmDLL)
    FreeLibrary(sDwmDLL);
}

void
nsUXThemeData::Initialize()
{
  ::ZeroMemory(sThemes, sizeof(sThemes));
  NS_ASSERTION(!sThemeDLL, "nsUXThemeData being initialized twice!");

  if (GetThemeDLL()) {
    openTheme = (OpenThemeDataPtr)GetProcAddress(sThemeDLL, "OpenThemeData");
    closeTheme = (CloseThemeDataPtr)GetProcAddress(sThemeDLL, "CloseThemeData");
    drawThemeBG = (DrawThemeBackgroundPtr)GetProcAddress(sThemeDLL, "DrawThemeBackground");
    drawThemeEdge = (DrawThemeEdgePtr)GetProcAddress(sThemeDLL, "DrawThemeEdge");
    getThemeContentRect = (GetThemeContentRectPtr)GetProcAddress(sThemeDLL, "GetThemeBackgroundContentRect");
    getThemeBackgroundRegion = (GetThemeBackgroundRegionPtr)GetProcAddress(sThemeDLL, "GetThemeBackgroundRegion");
    getThemeMetric = (GetThemeMetricPtr)GetProcAddress(sThemeDLL, "GetThemeMetric");
    getThemePartSize = (GetThemePartSizePtr)GetProcAddress(sThemeDLL, "GetThemePartSize");
    getThemeSysFont = (GetThemeSysFontPtr)GetProcAddress(sThemeDLL, "GetThemeSysFont");
    getThemeColor = (GetThemeColorPtr)GetProcAddress(sThemeDLL, "GetThemeColor");
    getThemeMargins = (GetThemeMarginsPtr)GetProcAddress(sThemeDLL, "GetThemeMargins");
    isAppThemed = (IsAppThemedPtr)GetProcAddress(sThemeDLL, "IsAppThemed");
    getCurrentThemeName = (GetCurrentThemeNamePtr)GetProcAddress(sThemeDLL, "GetCurrentThemeName");
    getThemeSysColor = (GetThemeSysColorPtr)GetProcAddress(sThemeDLL, "GetThemeSysColor");
    isThemeBackgroundPartiallyTransparent = (IsThemeBackgroundPartiallyTransparentPtr)GetProcAddress(sThemeDLL, "IsThemeBackgroundPartiallyTransparent");
  }
  if (GetDwmDLL()) {
    dwmExtendFrameIntoClientAreaPtr = (DwmExtendFrameIntoClientAreaProc)::GetProcAddress(sDwmDLL, "DwmExtendFrameIntoClientArea");
    dwmIsCompositionEnabledPtr = (DwmIsCompositionEnabledProc)::GetProcAddress(sDwmDLL, "DwmIsCompositionEnabled");
    dwmSetIconicThumbnailPtr = (DwmSetIconicThumbnailProc)::GetProcAddress(sDwmDLL, "DwmSetIconicThumbnail");
    dwmSetIconicLivePreviewBitmapPtr = (DwmSetIconicLivePreviewBitmapProc)::GetProcAddress(sDwmDLL, "DwmSetIconicLivePreviewBitmap");
    dwmGetWindowAttributePtr = (DwmGetWindowAttributeProc)::GetProcAddress(sDwmDLL, "DwmGetWindowAttribute");
    dwmSetWindowAttributePtr = (DwmSetWindowAttributeProc)::GetProcAddress(sDwmDLL, "DwmSetWindowAttribute");
    dwmInvalidateIconicBitmapsPtr = (DwmInvalidateIconicBitmapsProc)::GetProcAddress(sDwmDLL, "DwmInvalidateIconicBitmaps");
    dwmDwmDefWindowProcPtr = (DwmDefWindowProcProc)::GetProcAddress(sDwmDLL, "DwmDefWindowProc");
    CheckForCompositor(true);
  }

  Invalidate();
}

void
nsUXThemeData::Invalidate() {
  for(int i = 0; i < eUXNumClasses; i++) {
    if(sThemes[i]) {
      closeTheme(sThemes[i]);
      sThemes[i] = NULL;
    }
  }
  BOOL useFlat = false;
  sFlatMenus = ::SystemParametersInfo(SPI_GETFLATMENU, 0, &useFlat, 0) ?
                   useFlat : false;
}

HANDLE
nsUXThemeData::GetTheme(nsUXThemeClass cls) {
  NS_ASSERTION(cls < eUXNumClasses, "Invalid theme class!");
  if(!sThemeDLL)
    return NULL;
  if(!sThemes[cls])
  {
    sThemes[cls] = openTheme(NULL, GetClassName(cls));
  }
  return sThemes[cls];
}

HMODULE
nsUXThemeData::GetThemeDLL() {

  static DWORD winver = 0;

  if(!winver) {
    winver = GetVersion();

    // reorder from [build-num][minor][major] to [major][minor][build-num]
    winver = (winver << 24) | ((winver >> 8 & 0xff) << 16) | (winver >> 16 & 0xffff);
  }

  // Windows version < 5.1.2474 should not load uxtheme (due to incompatible uxtheme)
  if (winver >= 0x50109aa && !sThemeDLL)
    sThemeDLL = ::LoadLibraryW(kThemeLibraryName);
  return sThemeDLL;
}

HMODULE
nsUXThemeData::GetDwmDLL() {
  if (!sDwmDLL && WinUtils::GetWindowsVersion() >= WinUtils::VISTA_VERSION)
    sDwmDLL = ::LoadLibraryW(kDwmLibraryName);
  return sDwmDLL;
}

const wchar_t *nsUXThemeData::GetClassName(nsUXThemeClass cls) {
  switch(cls) {
    case eUXButton:
      return L"Button";
    case eUXEdit:
      return L"Edit";
    case eUXTooltip:
      return L"Tooltip";
    case eUXRebar:
      return L"Rebar";
    case eUXMediaRebar:
      return L"Media::Rebar";
    case eUXCommunicationsRebar:
      return L"Communications::Rebar";
    case eUXBrowserTabBarRebar:
      return L"BrowserTabBar::Rebar";
    case eUXToolbar:
      return L"Toolbar";
    case eUXMediaToolbar:
      return L"Media::Toolbar";
    case eUXCommunicationsToolbar:
      return L"Communications::Toolbar";
    case eUXProgress:
      return L"Progress";
    case eUXTab:
      return L"Tab";
    case eUXScrollbar:
      return L"Scrollbar";
    case eUXTrackbar:
      return L"Trackbar";
    case eUXSpin:
      return L"Spin";
    case eUXStatus:
      return L"Status";
    case eUXCombobox:
      return L"Combobox";
    case eUXHeader:
      return L"Header";
    case eUXListview:
      return L"Listview";
    case eUXMenu:
      return L"Menu";
    case eUXWindowFrame:
      return L"Window";
    default:
      NS_NOTREACHED("unknown uxtheme class");
      return L"";
  }
}

// static
void
nsUXThemeData::InitTitlebarInfo()
{
  // Pre-populate with generic metrics. These likley will not match
  // the current theme, but they insure the buttons at least show up.
  sCommandButtons[0].cx = GetSystemMetrics(SM_CXSIZE);
  sCommandButtons[0].cy = GetSystemMetrics(SM_CYSIZE);
  sCommandButtons[1].cx = sCommandButtons[2].cx = sCommandButtons[0].cx;
  sCommandButtons[1].cy = sCommandButtons[2].cy = sCommandButtons[0].cy;
  sCommandButtons[3].cx = sCommandButtons[0].cx * 3;
  sCommandButtons[3].cy = sCommandButtons[0].cy;

  // Use system metrics for pre-vista, otherwise trigger a
  // refresh on the next layout.
  sTitlebarInfoPopulatedAero = sTitlebarInfoPopulatedThemed =
    (WinUtils::GetWindowsVersion() < WinUtils::VISTA_VERSION);
}

// static
void
nsUXThemeData::UpdateTitlebarInfo(HWND aWnd)
{
  if (!aWnd)
    return;

  if (!sTitlebarInfoPopulatedAero && nsUXThemeData::CheckForCompositor()) {
    RECT captionButtons;
    if (SUCCEEDED(nsUXThemeData::dwmGetWindowAttributePtr(aWnd,
                                                          DWMWA_CAPTION_BUTTON_BOUNDS,
                                                          &captionButtons,
                                                          sizeof(captionButtons)))) {
      sCommandButtons[CMDBUTTONIDX_BUTTONBOX].cx = captionButtons.right - captionButtons.left - 3;
      sCommandButtons[CMDBUTTONIDX_BUTTONBOX].cy = (captionButtons.bottom - captionButtons.top) - 1;
      sTitlebarInfoPopulatedAero = true;
    }
  }

  if (sTitlebarInfoPopulatedThemed)
    return;

  // Query a temporary, visible window with command buttons to get
  // the right metrics. 
  nsAutoString className;
  className.AssignLiteral(kClassNameTemp);
  WNDCLASSW wc;
  wc.style         = 0;
  wc.lpfnWndProc   = ::DefWindowProcW;
  wc.cbClsExtra    = 0;
  wc.cbWndExtra    = 0;
  wc.hInstance     = nsToolkit::mDllInstance;
  wc.hIcon         = NULL;
  wc.hCursor       = NULL;
  wc.hbrBackground = NULL;
  wc.lpszMenuName  = NULL;
  wc.lpszClassName = className.get();
  ::RegisterClassW(&wc);

  // Create a transparent descendant of the window passed in. This
  // keeps the window from showing up on the desktop or the taskbar.
  // Note the parent (browser) window is usually still hidden, we
  // don't want to display it, so we can't query it directly.
  HWND hWnd = CreateWindowExW(WS_EX_LAYERED,
                              className.get(), L"",
                              WS_OVERLAPPEDWINDOW,
                              0, 0, 0, 0, aWnd, NULL,
                              nsToolkit::mDllInstance, NULL);
  NS_ASSERTION(hWnd, "UpdateTitlebarInfo window creation failed.");

  ShowWindow(hWnd, SW_SHOW);
  TITLEBARINFOEX info = {0};
  info.cbSize = sizeof(TITLEBARINFOEX);
  SendMessage(hWnd, WM_GETTITLEBARINFOEX, 0, (LPARAM)&info); 
  DestroyWindow(hWnd);

  // Only set if we have valid data for all three buttons we use.
  if ((info.rgrect[2].right - info.rgrect[2].left) == 0 ||
      (info.rgrect[3].right - info.rgrect[3].left) == 0 ||
      (info.rgrect[5].right - info.rgrect[5].left) == 0) {
    NS_WARNING("WM_GETTITLEBARINFOEX query failed to find usable metrics.");
    return;
  }
  // minimize
  sCommandButtons[0].cx = info.rgrect[2].right - info.rgrect[2].left;
  sCommandButtons[0].cy = info.rgrect[2].bottom - info.rgrect[2].top;
  // maximize/restore
  sCommandButtons[1].cx = info.rgrect[3].right - info.rgrect[3].left;
  sCommandButtons[1].cy = info.rgrect[3].bottom - info.rgrect[3].top;
  // close
  sCommandButtons[2].cx = info.rgrect[5].right - info.rgrect[5].left;
  sCommandButtons[2].cy = info.rgrect[5].bottom - info.rgrect[5].top;

  sTitlebarInfoPopulatedThemed = true;
}

// visual style (aero glass, aero basic)
//    theme (aero, luna, zune)
//      theme color (silver, olive, blue)
//        system colors

struct THEMELIST {
  LPCWSTR name;
  int type;
};

const THEMELIST knownThemes[] = {
  { L"aero.msstyles", WINTHEME_AERO },
  { L"aerolite.msstyles", WINTHEME_AERO_LITE },
  { L"luna.msstyles", WINTHEME_LUNA },
  { L"zune.msstyles", WINTHEME_ZUNE },
  { L"royale.msstyles", WINTHEME_ROYALE }
};

const THEMELIST knownColors[] = {
  { L"normalcolor", WINTHEMECOLOR_NORMAL },
  { L"homestead",   WINTHEMECOLOR_HOMESTEAD },
  { L"metallic",    WINTHEMECOLOR_METALLIC }
};

LookAndFeel::WindowsTheme
nsUXThemeData::sThemeId = LookAndFeel::eWindowsTheme_Generic;

bool
nsUXThemeData::sIsDefaultWindowsTheme = false;
bool
nsUXThemeData::sIsHighContrastOn = false;

// static
LookAndFeel::WindowsTheme
nsUXThemeData::GetNativeThemeId()
{
  return sThemeId;
}

// static
bool nsUXThemeData::IsDefaultWindowTheme()
{
  return sIsDefaultWindowsTheme;
}

bool nsUXThemeData::IsHighContrastOn()
{
  return sIsHighContrastOn;
}

static bool
IsWin8OrLater()
{
  OSVERSIONINFOW osInfo;
  osInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
  GetVersionExW(&osInfo);
  return (osInfo.dwMajorVersion > 6) ||
    (osInfo.dwMajorVersion >= 6 && osInfo.dwMinorVersion >= 2);
}

// static
void
nsUXThemeData::UpdateNativeThemeInfo()
{
  // Trigger a refresh of themed button metrics if needed
  sTitlebarInfoPopulatedThemed =
    (WinUtils::GetWindowsVersion() < WinUtils::VISTA_VERSION);

  sIsDefaultWindowsTheme = false;
  sThemeId = LookAndFeel::eWindowsTheme_Generic;

  HIGHCONTRAST highContrastInfo;
  highContrastInfo.cbSize = sizeof(HIGHCONTRAST);
  if (SystemParametersInfo(SPI_GETHIGHCONTRAST, 0, &highContrastInfo, 0)) {
    sIsHighContrastOn = ((highContrastInfo.dwFlags & HCF_HIGHCONTRASTON) != 0);
  } else {
    sIsHighContrastOn = false;
  }

  if (!IsAppThemed() || !getCurrentThemeName) {
    sThemeId = LookAndFeel::eWindowsTheme_Classic;
    return;
  }

  WCHAR themeFileName[MAX_PATH + 1];
  WCHAR themeColor[MAX_PATH + 1];
  if (FAILED(getCurrentThemeName(themeFileName,
                                 MAX_PATH,
                                 themeColor,
                                 MAX_PATH,
                                 NULL, 0))) {
    sThemeId = LookAndFeel::eWindowsTheme_Classic;
    return;
  }

  LPCWSTR themeName = wcsrchr(themeFileName, L'\\');
  themeName = themeName ? themeName + 1 : themeFileName;

  WindowsTheme theme = WINTHEME_UNRECOGNIZED;
  for (int i = 0; i < ArrayLength(knownThemes); ++i) {
    if (!lstrcmpiW(themeName, knownThemes[i].name)) {
      theme = (WindowsTheme)knownThemes[i].type;
      break;
    }
  }

  if (theme == WINTHEME_UNRECOGNIZED)
    return;

  // We're using the default theme if we're using any of Aero, Aero Lite, or
  // luna. However, on Win8, GetCurrentThemeName (see above) returns
  // AeroLite.msstyles for the 4 builtin highcontrast themes as well. Those
  // themes "don't count" as default themes, so we specifically check for high
  // contrast mode in that situation.
  if (!(IsWin8OrLater() && sIsHighContrastOn) &&
      (theme == WINTHEME_AERO || theme == WINTHEME_AERO_LITE || theme == WINTHEME_LUNA)) {
    sIsDefaultWindowsTheme = true;
  }

  if (theme != WINTHEME_LUNA) {
    switch(theme) {
      case WINTHEME_AERO:
        sThemeId = LookAndFeel::eWindowsTheme_Aero;
        return;
      case WINTHEME_AERO_LITE:
        sThemeId = LookAndFeel::eWindowsTheme_AeroLite;
        return;
      case WINTHEME_ZUNE:
        sThemeId = LookAndFeel::eWindowsTheme_Zune;
        return;
      case WINTHEME_ROYALE:
        sThemeId = LookAndFeel::eWindowsTheme_Royale;
        return;
      default:
        NS_WARNING("unhandled theme type.");
        return;
    }
  }

  // calculate the luna color scheme
  WindowsThemeColor color = WINTHEMECOLOR_UNRECOGNIZED;
  for (int i = 0; i < ArrayLength(knownColors); ++i) {
    if (!lstrcmpiW(themeColor, knownColors[i].name)) {
      color = (WindowsThemeColor)knownColors[i].type;
      break;
    }
  }

  switch(color) {
    case WINTHEMECOLOR_NORMAL:
      sThemeId = LookAndFeel::eWindowsTheme_LunaBlue;
      return;
    case WINTHEMECOLOR_HOMESTEAD:
      sThemeId = LookAndFeel::eWindowsTheme_LunaOlive;
      return;
    case WINTHEMECOLOR_METALLIC:
      sThemeId = LookAndFeel::eWindowsTheme_LunaSilver;
      return;
    default:
      NS_WARNING("unhandled theme color.");
      return;
  }
}
