/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsSetDllDirectory_h
#define nsSetDllDirectory_h

#ifndef XP_WIN
#error This file only makes sense on Windows.
#endif

#include <windows.h>
#include <nscore.h>
#include <stdlib.h>

namespace mozilla {

static void SanitizeEnvironmentVariables()
{
  DWORD bufferSize = GetEnvironmentVariableW(L"PATH", NULL, 0);
  if (bufferSize) {
    wchar_t* originalPath = new wchar_t[bufferSize];
    if (bufferSize - 1 == GetEnvironmentVariableW(L"PATH", originalPath, bufferSize)) {
      bufferSize = ExpandEnvironmentStringsW(originalPath, NULL, 0);
      if (bufferSize) {
        wchar_t* newPath = new wchar_t[bufferSize];
        if (ExpandEnvironmentStringsW(originalPath,
                                      newPath,
                                      bufferSize)) {
          SetEnvironmentVariableW(L"PATH", newPath);
        }
        delete[] newPath;
      }
    }
    delete[] originalPath;
  }
}

// Sets the directory from which DLLs can be loaded if the SetDllDirectory OS
// API is available.
// You must call SanitizeEnvironmentVariables before this function when calling
// it the first time.
static inline void NS_SetDllDirectory(const WCHAR *aDllDirectory)
{
  typedef BOOL
  (WINAPI *pfnSetDllDirectory) (LPCWSTR);
  pfnSetDllDirectory setDllDirectory = nullptr;
  setDllDirectory = reinterpret_cast<pfnSetDllDirectory>
      (GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDllDirectoryW"));
  if (setDllDirectory) {
    setDllDirectory(aDllDirectory);
  }
}

}

#endif
