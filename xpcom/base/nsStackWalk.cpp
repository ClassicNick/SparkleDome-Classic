/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set shiftwidth=4 tabstop=8 autoindent cindent expandtab: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* API for getting a stack trace of the C/C++ stack on the current thread */

#include "mozilla/Util.h"
#include "mozilla/StackWalk.h"
#include "nsDebug.h"
#include "nsStackWalkPrivate.h"

#include "nsStackWalk.h"

using namespace mozilla;

// The presence of this address is the stack must stop the stack walk. If
// there is no such address, the structure will be {NULL, true}.
struct CriticalAddress {
  void* mAddr;
  bool mInit;
};
static CriticalAddress gCriticalAddress;

#if defined(HAVE_DLOPEN) || defined(XP_MACOSX)
#include <dlfcn.h>
#endif

#define NSSTACKWALK_SUPPORTS_MACOSX \
    (defined(XP_MACOSX) && \
     (defined(__i386) || defined(__ppc__) || defined(HAVE__UNWIND_BACKTRACE)))

#define NSSTACKWALK_SUPPORTS_LINUX \
    (defined(linux) && \
     ((defined(__GNUC__) && (defined(__i386) || defined(PPC))) || \
      defined(HAVE__UNWIND_BACKTRACE)))

#define NSSTACKWALK_SUPPORTS_SOLARIS \
    (defined(__sun) && \
     (defined(__sparc) || defined(sparc) || defined(__i386) || defined(i386)))

#if NSSTACKWALK_SUPPORTS_MACOSX
#include <pthread.h>
#include <errno.h>
#include <CoreServices/CoreServices.h>

typedef void
malloc_logger_t(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                uintptr_t result, uint32_t num_hot_frames_to_skip);
extern malloc_logger_t *malloc_logger;

static void
stack_callback(void *pc, void *sp, void *closure)
{
  const char *name = reinterpret_cast<char *>(closure);
  Dl_info info;

  // On Leopard dladdr returns the wrong value for "new_sem_from_pool". The
  // stack shows up as having two pthread_cond_wait$UNIX2003 frames. The
  // correct one is the first that we find on our way up, so the
  // following check for gCriticalAddress.mAddr is critical.
  if (gCriticalAddress.mAddr || dladdr(pc, &info) == 0  ||
      info.dli_sname == NULL || strcmp(info.dli_sname, name) != 0)
    return;
  gCriticalAddress.mAddr = pc;
}

#ifdef DEBUG
#define MAC_OS_X_VERSION_10_7_HEX 0x00001070

static int32_t OSXVersion()
{
  static int32_t gOSXVersion = 0x0;
  if (gOSXVersion == 0x0) {
    OSErr err = ::Gestalt(gestaltSystemVersion, (SInt32*)&gOSXVersion);
    MOZ_ASSERT(err == noErr);
  }
  return gOSXVersion;
}

static bool OnLionOrLater()
{
  return (OSXVersion() >= MAC_OS_X_VERSION_10_7_HEX);
}
#endif

static void
my_malloc_logger(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                 uintptr_t result, uint32_t num_hot_frames_to_skip)
{
  static bool once = false;
  if (once)
    return;
  once = true;

  // On Leopard dladdr returns the wrong value for "new_sem_from_pool". The
  // stack shows up as having two pthread_cond_wait$UNIX2003 frames.
  const char *name = "new_sem_from_pool";
  NS_StackWalk(stack_callback, /* skipFrames */ 0, /* maxFrames */ 0,
               const_cast<char*>(name), 0, nullptr);
}

// This is called from NS_LogInit() and from the stack walking functions, but
// only the first call has any effect.  We need to call this function from both
// places because it must run before any mutexes are created, and also before
// any objects whose refcounts we're logging are created.  Running this
// function during NS_LogInit() ensures that we meet the first criterion, and
// running this function during the stack walking functions ensures we meet the
// second criterion.
void
StackWalkInitCriticalAddress()
{
  if(gCriticalAddress.mInit)
    return;
  gCriticalAddress.mInit = true;
  // We must not do work when 'new_sem_from_pool' calls realloc, since
  // it holds a non-reentrant spin-lock and we will quickly deadlock.
  // new_sem_from_pool is not directly accessible using dlsym, so
  // we force a situation where new_sem_from_pool is on the stack and
  // use dladdr to check the addresses.

  // malloc_logger can be set by external tools like 'Instruments' or 'leaks'
  malloc_logger_t *old_malloc_logger = malloc_logger;
  malloc_logger = my_malloc_logger;

  pthread_cond_t cond;
  int r = pthread_cond_init(&cond, 0);
  MOZ_ASSERT(r == 0);
  pthread_mutex_t mutex;
  r = pthread_mutex_init(&mutex,0);
  MOZ_ASSERT(r == 0);
  r = pthread_mutex_lock(&mutex);
  MOZ_ASSERT(r == 0);
  struct timespec abstime = {0, 1};
  r = pthread_cond_timedwait_relative_np(&cond, &mutex, &abstime);

  // restore the previous malloc logger
  malloc_logger = old_malloc_logger;

  // On Lion, malloc is no longer called from pthread_cond_*wait*. This prevents
  // us from finding the address, but that is fine, since with no call to malloc
  // there is no critical address.
  MOZ_ASSERT(OnLionOrLater() || gCriticalAddress.mAddr != NULL);
  MOZ_ASSERT(r == ETIMEDOUT);
  r = pthread_mutex_unlock(&mutex);
  MOZ_ASSERT(r == 0);
  r = pthread_mutex_destroy(&mutex);
  MOZ_ASSERT(r == 0);
  r = pthread_cond_destroy(&cond);
  MOZ_ASSERT(r == 0);
}

static bool IsCriticalAddress(void* aPC)
{
  return gCriticalAddress.mAddr == aPC;
}
#else
static bool IsCriticalAddress(void* aPC)
{
  return false;
}
// We still initialize gCriticalAddress.mInit so that this code behaves
// the same on all platforms. Otherwise a failure to init would be visible
// only on OS X.
void
StackWalkInitCriticalAddress()
{
  gCriticalAddress.mInit = true;
}
#endif

#if defined(_WIN32) && (defined(_M_IX86) || defined(_M_AMD64) || defined(_M_IA64)) // WIN32 x86 stack walking code

#include "nscore.h"
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <malloc.h>
#include "plstr.h"

#include "nspr.h"
#include <imagehlp.h>
// We need a way to know if we are building for WXP (or later), as if we are, we
// need to use the newer 64-bit APIs. API_VERSION_NUMBER seems to fit the bill.
// A value of 9 indicates we want to use the new APIs.
#if API_VERSION_NUMBER >= 9
#define USING_WXP_VERSION 1
#endif

// Define these as static pointers so that we can load the DLL on the
// fly (and not introduce a link-time dependency on it). Tip o' the
// hat to Matt Pietrick for this idea. See:
//
//   http://msdn.microsoft.com/library/periodic/period97/F1/D3/S245C6.htm
//
extern "C" {

typedef DWORD (__stdcall *SYMSETOPTIONSPROC)(DWORD);
extern SYMSETOPTIONSPROC _SymSetOptions;

typedef BOOL (__stdcall *SYMINITIALIZEPROC)(HANDLE, LPSTR, BOOL);
extern SYMINITIALIZEPROC _SymInitialize;

typedef BOOL (__stdcall *SYMCLEANUPPROC)(HANDLE);
extern SYMCLEANUPPROC _SymCleanup;

typedef BOOL (__stdcall *STACKWALKPROC)(DWORD,
                                        HANDLE,
                                        HANDLE,
                                        LPSTACKFRAME,
                                        LPVOID,
                                        PREAD_PROCESS_MEMORY_ROUTINE,
                                        PFUNCTION_TABLE_ACCESS_ROUTINE,
                                        PGET_MODULE_BASE_ROUTINE,
                                        PTRANSLATE_ADDRESS_ROUTINE);
extern  STACKWALKPROC _StackWalk;

#ifdef USING_WXP_VERSION
typedef BOOL (__stdcall *STACKWALKPROC64)(DWORD,
                                          HANDLE,
                                          HANDLE,
                                          LPSTACKFRAME64,
                                          PVOID,
                                          PREAD_PROCESS_MEMORY_ROUTINE64,
                                          PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                          PGET_MODULE_BASE_ROUTINE64,
                                          PTRANSLATE_ADDRESS_ROUTINE64);
extern  STACKWALKPROC64 _StackWalk64;
#endif

typedef LPVOID (__stdcall *SYMFUNCTIONTABLEACCESSPROC)(HANDLE, DWORD);
extern  SYMFUNCTIONTABLEACCESSPROC _SymFunctionTableAccess;

#ifdef USING_WXP_VERSION
typedef LPVOID (__stdcall *SYMFUNCTIONTABLEACCESSPROC64)(HANDLE, DWORD64);
extern  SYMFUNCTIONTABLEACCESSPROC64 _SymFunctionTableAccess64;
#endif

typedef DWORD (__stdcall *SYMGETMODULEBASEPROC)(HANDLE, DWORD);
extern  SYMGETMODULEBASEPROC _SymGetModuleBase;

#ifdef USING_WXP_VERSION
typedef DWORD64 (__stdcall *SYMGETMODULEBASEPROC64)(HANDLE, DWORD64);
extern  SYMGETMODULEBASEPROC64 _SymGetModuleBase64;
#endif

typedef BOOL (__stdcall *SYMGETSYMFROMADDRPROC)(HANDLE, DWORD, PDWORD, PIMAGEHLP_SYMBOL);
extern  SYMGETSYMFROMADDRPROC _SymGetSymFromAddr;

#ifdef USING_WXP_VERSION
typedef BOOL (__stdcall *SYMFROMADDRPROC)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
extern  SYMFROMADDRPROC _SymFromAddr;
#endif

typedef DWORD ( __stdcall *SYMLOADMODULE)(HANDLE, HANDLE, PSTR, PSTR, DWORD, DWORD);
extern  SYMLOADMODULE _SymLoadModule;

#ifdef USING_WXP_VERSION
typedef DWORD ( __stdcall *SYMLOADMODULE64)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD);
extern  SYMLOADMODULE64 _SymLoadModule64;
#endif

typedef DWORD ( __stdcall *SYMUNDNAME)(PIMAGEHLP_SYMBOL, PSTR, DWORD);
extern  SYMUNDNAME _SymUnDName;

typedef DWORD ( __stdcall *SYMGETMODULEINFO)( HANDLE, DWORD, PIMAGEHLP_MODULE);
extern  SYMGETMODULEINFO _SymGetModuleInfo;

#ifdef USING_WXP_VERSION
typedef BOOL ( __stdcall *SYMGETMODULEINFO64)( HANDLE, DWORD64, PIMAGEHLP_MODULE64);
extern  SYMGETMODULEINFO64 _SymGetModuleInfo64;
#endif

typedef BOOL ( __stdcall *ENUMLOADEDMODULES)( HANDLE, PENUMLOADED_MODULES_CALLBACK, PVOID);
extern  ENUMLOADEDMODULES _EnumerateLoadedModules;

#ifdef USING_WXP_VERSION
typedef BOOL ( __stdcall *ENUMLOADEDMODULES64)( HANDLE, PENUMLOADED_MODULES_CALLBACK64, PVOID);
extern  ENUMLOADEDMODULES64 _EnumerateLoadedModules64;
#endif

typedef BOOL (__stdcall *SYMGETLINEFROMADDRPROC)(HANDLE, DWORD, PDWORD, PIMAGEHLP_LINE);
extern  SYMGETLINEFROMADDRPROC _SymGetLineFromAddr;

#ifdef USING_WXP_VERSION
typedef BOOL (__stdcall *SYMGETLINEFROMADDRPROC64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
extern  SYMGETLINEFROMADDRPROC64 _SymGetLineFromAddr64;
#endif

extern HANDLE hStackWalkMutex; 

bool EnsureSymInitialized();

bool EnsureImageHlpInitialized();

/*
 * SymGetModuleInfoEspecial
 *
 * Attempt to determine the module information.
 * Bug 112196 says this DLL may not have been loaded at the time
 *  SymInitialize was called, and thus the module information
 *  and symbol information is not available.
 * This code rectifies that problem.
 * Line information is optional.
 */
BOOL SymGetModuleInfoEspecial(HANDLE aProcess, DWORD aAddr, PIMAGEHLP_MODULE aModuleInfo, PIMAGEHLP_LINE aLineInfo);

struct WalkStackData {
  uint32_t skipFrames;
  HANDLE thread;
  bool walkCallingThread;
  HANDLE process;
  HANDLE eventStart;
  HANDLE eventEnd;
  void **pcs;
  uint32_t pc_size;
  uint32_t pc_count;
  uint32_t pc_max;
  void **sps;
  uint32_t sp_size;
  uint32_t sp_count;
  void *platformData;
};

void PrintError(char *prefix, WalkStackData* data);
unsigned int WINAPI WalkStackThread(void* data);
void WalkStackMain64(struct WalkStackData* data);
#if !defined(_WIN64)
void WalkStackMain(struct WalkStackData* data);
#endif


// Define these as static pointers so that we can load the DLL on the
// fly (and not introduce a link-time dependency on it). Tip o' the
// hat to Matt Pietrick for this idea. See:
//
//   http://msdn.microsoft.com/library/periodic/period97/F1/D3/S245C6.htm
//

SYMSETOPTIONSPROC _SymSetOptions;

SYMINITIALIZEPROC _SymInitialize;

SYMCLEANUPPROC _SymCleanup;

STACKWALKPROC _StackWalk;
#ifdef USING_WXP_VERSION
STACKWALKPROC64 _StackWalk64;
#else
#define _StackWalk64 0
#endif

SYMFUNCTIONTABLEACCESSPROC _SymFunctionTableAccess;
#ifdef USING_WXP_VERSION
SYMFUNCTIONTABLEACCESSPROC64 _SymFunctionTableAccess64;
#else
#define _SymFunctionTableAccess64 0
#endif

SYMGETMODULEBASEPROC _SymGetModuleBase;
#ifdef USING_WXP_VERSION
SYMGETMODULEBASEPROC64 _SymGetModuleBase64;
#else
#define _SymGetModuleBase64 0
#endif

SYMGETSYMFROMADDRPROC _SymGetSymFromAddr;
#ifdef USING_WXP_VERSION
SYMFROMADDRPROC _SymFromAddr;
#else
#define _SymFromAddr 0
#endif

SYMLOADMODULE _SymLoadModule;
#ifdef USING_WXP_VERSION
SYMLOADMODULE64 _SymLoadModule64;
#else
#define _SymLoadModule64 0
#endif

SYMUNDNAME _SymUnDName;

SYMGETMODULEINFO _SymGetModuleInfo;
#ifdef USING_WXP_VERSION
SYMGETMODULEINFO64 _SymGetModuleInfo64;
#else
#define _SymGetModuleInfo64 0
#endif

ENUMLOADEDMODULES _EnumerateLoadedModules;
#ifdef USING_WXP_VERSION
ENUMLOADEDMODULES64 _EnumerateLoadedModules64;
#else
#define _EnumerateLoadedModules64 0
#endif

SYMGETLINEFROMADDRPROC _SymGetLineFromAddr;
#ifdef USING_WXP_VERSION
SYMGETLINEFROMADDRPROC64 _SymGetLineFromAddr64;
#else
#define _SymGetLineFromAddr64 0
#endif

DWORD gStackWalkThread;
CRITICAL_SECTION gDbgHelpCS;

}

// Routine to print an error message to standard error.
// Will also call callback with error, if data supplied.
void PrintError(const char *prefix)
{
    LPVOID lpMsgBuf;
    DWORD lastErr = GetLastError();
    FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL,
      lastErr,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
      (LPSTR) &lpMsgBuf,
      0,
      NULL
    );
    fprintf(stderr, "### ERROR: %s: %s",
                    prefix, lpMsgBuf ? lpMsgBuf : "(null)\n");
    fflush(stderr);
    LocalFree(lpMsgBuf);
}

bool
EnsureImageHlpInitialized()
{
    static bool gInitialized = false;

    if (gInitialized)
        return gInitialized;

    // Hope that our first call doesn't happen during static
    // initialization.  If it does, this CreateThread call won't
    // actually start the thread until after the static initialization
    // is done, which means we'll deadlock while waiting for it to
    // process a stack.
    HANDLE readyEvent = ::CreateEvent(NULL, FALSE /* auto-reset*/,
                            FALSE /* initially non-signaled */, NULL);
    unsigned int threadID;
    HANDLE hStackWalkThread = (HANDLE)
      _beginthreadex(NULL, 0, WalkStackThread, (void*)readyEvent,
                     0, &threadID);
    gStackWalkThread = threadID;
    if (hStackWalkThread == NULL) {
        PrintError("CreateThread");
        return false;
    }
    ::CloseHandle(hStackWalkThread);

    // Wait for the thread's event loop to start before posting events to it.
    ::WaitForSingleObject(readyEvent, INFINITE);
    ::CloseHandle(readyEvent);

    ::InitializeCriticalSection(&gDbgHelpCS);

    HMODULE module = ::LoadLibraryW(L"DBGHELP.DLL");
    if (!module) {
        module = ::LoadLibraryW(L"IMAGEHLP.DLL");
        if (!module) return false;
    }

    _SymSetOptions = (SYMSETOPTIONSPROC) ::GetProcAddress(module, "SymSetOptions");
    if (!_SymSetOptions) return false;

    _SymInitialize = (SYMINITIALIZEPROC) ::GetProcAddress(module, "SymInitialize");
    if (!_SymInitialize) return false;

    _SymCleanup = (SYMCLEANUPPROC)GetProcAddress(module, "SymCleanup");
    if (!_SymCleanup) return false;

#ifdef USING_WXP_VERSION
    _StackWalk64 = (STACKWALKPROC64)GetProcAddress(module, "StackWalk64");
#endif
    _StackWalk = (STACKWALKPROC)GetProcAddress(module, "StackWalk");
    if (!_StackWalk64  && !_StackWalk) return false;

#ifdef USING_WXP_VERSION
    _SymFunctionTableAccess64 = (SYMFUNCTIONTABLEACCESSPROC64) GetProcAddress(module, "SymFunctionTableAccess64");
#endif
    _SymFunctionTableAccess = (SYMFUNCTIONTABLEACCESSPROC) GetProcAddress(module, "SymFunctionTableAccess");
    if (!_SymFunctionTableAccess64 && !_SymFunctionTableAccess) return false;

#ifdef USING_WXP_VERSION
    _SymGetModuleBase64 = (SYMGETMODULEBASEPROC64)GetProcAddress(module, "SymGetModuleBase64");
#endif
    _SymGetModuleBase = (SYMGETMODULEBASEPROC)GetProcAddress(module, "SymGetModuleBase");
    if (!_SymGetModuleBase64 && !_SymGetModuleBase) return false;

    _SymGetSymFromAddr = (SYMGETSYMFROMADDRPROC)GetProcAddress(module, "SymGetSymFromAddr");
#ifdef USING_WXP_VERSION
    _SymFromAddr = (SYMFROMADDRPROC)GetProcAddress(module, "SymFromAddr");
#endif
    if (!_SymFromAddr && !_SymGetSymFromAddr) return false;

#ifdef USING_WXP_VERSION
    _SymLoadModule64 = (SYMLOADMODULE64)GetProcAddress(module, "SymLoadModule64");
#endif
    _SymLoadModule = (SYMLOADMODULE)GetProcAddress(module, "SymLoadModule");
    if (!_SymLoadModule64 && !_SymLoadModule) return false;

    _SymUnDName = (SYMUNDNAME)GetProcAddress(module, "SymUnDName");
    if (!_SymUnDName) return false;

#ifdef USING_WXP_VERSION
    _SymGetModuleInfo64 = (SYMGETMODULEINFO64)GetProcAddress(module, "SymGetModuleInfo64");
#endif
    _SymGetModuleInfo = (SYMGETMODULEINFO)GetProcAddress(module, "SymGetModuleInfo");
    if (!_SymGetModuleInfo64 && !_SymGetModuleInfo) return false;

#ifdef USING_WXP_VERSION
    _EnumerateLoadedModules64 = (ENUMLOADEDMODULES64)GetProcAddress(module, "EnumerateLoadedModules64");
#endif
    _EnumerateLoadedModules = (ENUMLOADEDMODULES)GetProcAddress(module, "EnumerateLoadedModules");
    if (!_EnumerateLoadedModules64 && !_EnumerateLoadedModules) return false;

#ifdef USING_WXP_VERSION
    _SymGetLineFromAddr64 = (SYMGETLINEFROMADDRPROC64)GetProcAddress(module, "SymGetLineFromAddr64");
#endif
    _SymGetLineFromAddr = (SYMGETLINEFROMADDRPROC)GetProcAddress(module, "SymGetLineFromAddr");
    if (!_SymGetLineFromAddr64 && !_SymGetLineFromAddr) return false;

    return gInitialized = true;
}

void
WalkStackMain64(struct WalkStackData* data)
{
#ifdef USING_WXP_VERSION
    // Get the context information for the thread. That way we will
    // know where our sp, fp, pc, etc. are and can fill in the
    // STACKFRAME64 with the initial values.
    CONTEXT context;
    HANDLE myProcess = data->process;
    HANDLE myThread = data->thread;
    DWORD64 addr;
    DWORD64 spaddr;
    STACKFRAME64 frame64;
    // skip our own stack walking frames
    int skip = (data->walkCallingThread ? 3 : 0) + data->skipFrames;
    BOOL ok;

    // Get a context for the specified thread.
    if (!data->platformData) {
        memset(&context, 0, sizeof(CONTEXT));
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(myThread, &context)) {
            if (data->walkCallingThread) {
                PrintError("GetThreadContext");
            }
            return;
        }
    } else {
        context = *static_cast<CONTEXT*>(data->platformData);
    }

    // Setup initial stack frame to walk from
    memset(&frame64, 0, sizeof(frame64));
#ifdef _M_IX86
    frame64.AddrPC.Offset    = context.Eip;
    frame64.AddrStack.Offset = context.Esp;
    frame64.AddrFrame.Offset = context.Ebp;
#elif defined _M_AMD64
    frame64.AddrPC.Offset    = context.Rip;
    frame64.AddrStack.Offset = context.Rsp;
    frame64.AddrFrame.Offset = context.Rbp;
#elif defined _M_IA64
    frame64.AddrPC.Offset    = context.StIIP;
    frame64.AddrStack.Offset = context.SP;
    frame64.AddrFrame.Offset = context.RsBSP;
#else
#error "Should not have compiled this code"
#endif
    frame64.AddrPC.Mode      = AddrModeFlat;
    frame64.AddrStack.Mode   = AddrModeFlat;
    frame64.AddrFrame.Mode   = AddrModeFlat;
    frame64.AddrReturn.Mode  = AddrModeFlat;

    // Now walk the stack
    while (1) {

        // debug routines are not threadsafe, so grab the lock.
        EnterCriticalSection(&gDbgHelpCS);
        ok = _StackWalk64(
#ifdef _M_AMD64
          IMAGE_FILE_MACHINE_AMD64,
#elif defined _M_IA64
          IMAGE_FILE_MACHINE_IA64,
#elif defined _M_IX86
          IMAGE_FILE_MACHINE_I386,
#else
#error "Should not have compiled this code"
#endif
          myProcess,
          myThread,
          &frame64,
          &context,
          NULL,
          _SymFunctionTableAccess64, // function table access routine
          _SymGetModuleBase64,       // module base routine
          0
        );
        LeaveCriticalSection(&gDbgHelpCS);

        if (ok) {
            addr = frame64.AddrPC.Offset;
            spaddr = frame64.AddrStack.Offset;
         } else {
            addr = 0;
            spaddr = 0;
            if (data->walkCallingThread) {
                PrintError("WalkStack64");
            }
        }

        if (!ok || (addr == 0)) {
            break;
        }

        if (skip-- > 0) {
            continue;
        }

        if (data->pc_count < data->pc_size)
            data->pcs[data->pc_count] = (void*)addr;
        ++data->pc_count;

        if (data->sp_count < data->sp_size)
            data->sps[data->sp_count] = (void*)spaddr;
        ++data->sp_count;

        if (data->pc_max != 0 && data->pc_count == data->pc_max)
            break;

        if (frame64.AddrReturn.Offset == 0)
            break;
    }
    return;
#endif
}


#if !defined(_WIN64)
void
WalkStackMain(struct WalkStackData* data)
{
    // Get the context information for the thread. That way we will
    // know where our sp, fp, pc, etc. are and can fill in the
    // STACKFRAME with the initial values.
    CONTEXT context;
    HANDLE myProcess = data->process;
    HANDLE myThread = data->thread;
    DWORD addr;
    STACKFRAME frame;
    int skip = data->skipFrames; // skip our own stack walking frames
    BOOL ok;

    // Get a context for the specified thread.
    memset(&context, 0, sizeof(CONTEXT));
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(myThread, &context)) {
        PrintError("GetThreadContext");
        return;
    }

    // Setup initial stack frame to walk from
#if defined _M_IX86
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset    = context.Eip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrStack.Offset = context.Esp;
    frame.AddrStack.Mode   = AddrModeFlat;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrFrame.Mode   = AddrModeFlat;
#else
    PrintError("Unknown platform. No stack walking.");
    return;
#endif

    // Now walk the stack
    while (1) {

        // debug routines are not threadsafe, so grab the lock.
        EnterCriticalSection(&gDbgHelpCS);
        ok = _StackWalk(
            IMAGE_FILE_MACHINE_I386,
            myProcess,
            myThread,
            &frame,
            &context,
            0,                        // read process memory routine
            _SymFunctionTableAccess,  // function table access routine
            _SymGetModuleBase,        // module base routine
            0                         // translate address routine
          );
        LeaveCriticalSection(&gDbgHelpCS);

        if (ok)
            addr = frame.AddrPC.Offset;
        else {
            addr = 0;
            PrintError("WalkStack");
        }

        if (!ok || (addr == 0)) {
            break;
        }

        if (skip-- > 0) {
            continue;
        }

        if (data->pc_count < data->pc_size)
            data->pcs[data->pc_count] = (void*)addr;
        ++data->pc_count;

        if (frame.AddrReturn.Offset == 0)
            break;
    }

    return;

}
#endif

static
void PerformStackWalk(struct WalkStackData* data)
{
#if defined(_WIN64)
    WalkStackMain64(data);
#else
    if (_StackWalk64)
        WalkStackMain64(data);
    else
        WalkStackMain(data);
#endif
}

unsigned int WINAPI
WalkStackThread(void* aData)
{
    BOOL msgRet;
    MSG msg;

    // Call PeekMessage to force creation of a message queue so that
    // other threads can safely post events to us.
    ::PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    // and tell the thread that created us that we're ready.
    HANDLE readyEvent = (HANDLE)aData;
    ::SetEvent(readyEvent);

    while ((msgRet = ::GetMessage(&msg, (HWND)-1, 0, 0)) != 0) {
        if (msgRet == -1) {
            PrintError("GetMessage");
        } else {
            DWORD ret;

            struct WalkStackData *data = (WalkStackData *)msg.lParam;
            if (!data)
              continue;

            // Don't suspend the calling thread until it's waiting for
            // us; otherwise the number of frames on the stack could vary.
            ret = ::WaitForSingleObject(data->eventStart, INFINITE);
            if (ret != WAIT_OBJECT_0)
                PrintError("WaitForSingleObject");

            // Suspend the calling thread, dump his stack, and then resume him.
            // He's currently waiting for us to finish so now should be a good time.
            ret = ::SuspendThread( data->thread );
            if (ret == -1) {
                PrintError("ThreadSuspend");
            }
            else {
                PerformStackWalk(data);

                ret = ::ResumeThread(data->thread);
                if (ret == -1) {
                    PrintError("ThreadResume");
                }
            }

            ::SetEvent(data->eventEnd);
        }
    }

    return 0;
}

/**
 * Walk the stack, translating PC's found into strings and recording the
 * chain in aBuffer. For this to work properly, the DLLs must be rebased
 * so that the address in the file agrees with the address in memory.
 * Otherwise StackWalk will return FALSE when it hits a frame in a DLL
 * whose in memory address doesn't match its in-file address.
 */

EXPORT_XPCOM_API(nsresult)
NS_StackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void *aClosure, uintptr_t aThread,
             void *aPlatformData)
{
    StackWalkInitCriticalAddress();
    static HANDLE myProcess = NULL;
    HANDLE myThread;
    DWORD walkerReturn;
    struct WalkStackData data;

    if (!EnsureImageHlpInitialized())
        return NS_ERROR_FAILURE;

    HANDLE targetThread = ::GetCurrentThread();
    data.walkCallingThread = true;
    if (aThread) {
        HANDLE threadToWalk = reinterpret_cast<HANDLE> (aThread);
        // walkCallingThread indicates whether we are walking the caller's stack
        data.walkCallingThread = (threadToWalk == targetThread);
        targetThread = threadToWalk;
    }

    // We need to avoid calling fprintf and friends if we're walking the stack of
    // another thread, in order to avoid deadlocks.
    const bool shouldBeThreadSafe = !!aThread;

    // Have to duplicate handle to get a real handle.
    if (!myProcess) {
        if (!::DuplicateHandle(::GetCurrentProcess(),
                               ::GetCurrentProcess(),
                               ::GetCurrentProcess(),
                               &myProcess,
                               PROCESS_ALL_ACCESS, FALSE, 0)) {
            if (!shouldBeThreadSafe) {
                PrintError("DuplicateHandle (process)");
            }
            return NS_ERROR_FAILURE;
        }
    }
    if (!::DuplicateHandle(::GetCurrentProcess(),
                           targetThread,
                           ::GetCurrentProcess(),
                           &myThread,
                           THREAD_ALL_ACCESS, FALSE, 0)) {
        if (!shouldBeThreadSafe) {
            PrintError("DuplicateHandle (thread)");
        }
        return NS_ERROR_FAILURE;
    }

    data.skipFrames = aSkipFrames;
    data.thread = myThread;
    data.process = myProcess;
    void *local_pcs[1024];
    data.pcs = local_pcs;
    data.pc_count = 0;
    data.pc_size = ArrayLength(local_pcs);
    data.pc_max = aMaxFrames;
    void *local_sps[1024];
    data.sps = local_sps;
    data.sp_count = 0;
    data.sp_size = ArrayLength(local_sps);
    data.platformData = aPlatformData;

    if (aThread) {
        // If we're walking the stack of another thread, we don't need to
        // use a separate walker thread.
        WalkStackMain64(&data);

        if (data.pc_count > data.pc_size) {
            data.pcs = (void**) _alloca(data.pc_count * sizeof(void*));
            data.pc_size = data.pc_count;
            data.pc_count = 0;
            data.sps = (void**) _alloca(data.sp_count * sizeof(void*));
            data.sp_size = data.sp_count;
            data.sp_count = 0;
            PerformStackWalk(&data);
        }
    } else {
        data.eventStart = ::CreateEvent(NULL, FALSE /* auto-reset*/,
                              FALSE /* initially non-signaled */, NULL);
        data.eventEnd = ::CreateEvent(NULL, FALSE /* auto-reset*/,
                            FALSE /* initially non-signaled */, NULL);

        ::PostThreadMessage(gStackWalkThread, WM_USER, 0, (LPARAM)&data);

        walkerReturn = ::SignalObjectAndWait(data.eventStart,
                           data.eventEnd, INFINITE, FALSE);
        if (walkerReturn != WAIT_OBJECT_0 && !shouldBeThreadSafe)
            PrintError("SignalObjectAndWait (1)");
        if (data.pc_count > data.pc_size) {
            data.pcs = (void**) _alloca(data.pc_count * sizeof(void*));
            data.pc_size = data.pc_count;
            data.pc_count = 0;
            data.sps = (void**) _alloca(data.sp_count * sizeof(void*));
            data.sp_size = data.sp_count;
            data.sp_count = 0;
            ::PostThreadMessage(gStackWalkThread, WM_USER, 0, (LPARAM)&data);
            walkerReturn = ::SignalObjectAndWait(data.eventStart,
                               data.eventEnd, INFINITE, FALSE);
            if (walkerReturn != WAIT_OBJECT_0 && !shouldBeThreadSafe)
                PrintError("SignalObjectAndWait (2)");
        }

        ::CloseHandle(data.eventStart);
        ::CloseHandle(data.eventEnd);
    }

    ::CloseHandle(myThread);

    for (uint32_t i = 0; i < data.pc_count; ++i)
        (*aCallback)(data.pcs[i], data.sps[i], aClosure);

    return data.pc_count == 0 ? NS_ERROR_FAILURE : NS_OK;
}

static BOOL CALLBACK callbackEspecial(
  PCSTR aModuleName,
  ULONG aModuleBase,
  ULONG aModuleSize,
  PVOID aUserContext)
{
    BOOL retval = TRUE;
    DWORD addr = *(DWORD*)aUserContext;

    /*
     * You'll want to control this if we are running on an
     *  architecture where the addresses go the other direction.
     * Not sure this is even a realistic consideration.
     */
    const BOOL addressIncreases = TRUE;

    /*
     * If it falls inside the known range, load the symbols.
     */
    if (addressIncreases
       ? (addr >= aModuleBase && addr <= (aModuleBase + aModuleSize))
       : (addr <= aModuleBase && addr >= (aModuleBase - aModuleSize))
        ) {
        retval = _SymLoadModule(GetCurrentProcess(), NULL, (PSTR)aModuleName, NULL, aModuleBase, aModuleSize);
        if (!retval)
            PrintError("SymLoadModule");
    }

    return retval;
}


static BOOL CALLBACK callbackEspecial64(
  PCSTR aModuleName,
  DWORD64 aModuleBase,
  ULONG aModuleSize,
  PVOID aUserContext)
{
#ifdef USING_WXP_VERSION
    BOOL retval = TRUE;
    DWORD64 addr = *(DWORD64*)aUserContext;

    /*
     * You'll want to control this if we are running on an
     *  architecture where the addresses go the other direction.
     * Not sure this is even a realistic consideration.
     */
    const BOOL addressIncreases = TRUE;

    /*
     * If it falls in side the known range, load the symbols.
     */
    if (addressIncreases
       ? (addr >= aModuleBase && addr <= (aModuleBase + aModuleSize))
       : (addr <= aModuleBase && addr >= (aModuleBase - aModuleSize))
        ) {
        retval = !!_SymLoadModule64(GetCurrentProcess(), NULL, (PSTR)aModuleName, NULL, aModuleBase, aModuleSize);
        if (!retval)
            PrintError("SymLoadModule64");
    }

    return retval;
#else
    return FALSE;
#endif
}

/*
 * SymGetModuleInfoEspecial
 *
 * Attempt to determine the module information.
 * Bug 112196 says this DLL may not have been loaded at the time
 *  SymInitialize was called, and thus the module information
 *  and symbol information is not available.
 * This code rectifies that problem.
 */
BOOL SymGetModuleInfoEspecial(HANDLE aProcess, DWORD aAddr, PIMAGEHLP_MODULE aModuleInfo, PIMAGEHLP_LINE aLineInfo)
{
    BOOL retval = FALSE;

    /*
     * Init the vars if we have em.
     */
    aModuleInfo->SizeOfStruct = sizeof(IMAGEHLP_MODULE);
    if (nullptr != aLineInfo) {
      aLineInfo->SizeOfStruct = sizeof(IMAGEHLP_LINE);
    }

    /*
     * Give it a go.
     * It may already be loaded.
     */
    retval = _SymGetModuleInfo(aProcess, aAddr, aModuleInfo);

    if (FALSE == retval) {
        BOOL enumRes = FALSE;

        /*
         * Not loaded, here's the magic.
         * Go through all the modules.
         */
        // Need to cast to PENUMLOADED_MODULES_CALLBACK because the
        // constness of the first parameter of
        // PENUMLOADED_MODULES_CALLBACK varies over SDK versions (from
        // non-const to const over time).  See bug 391848 and bug
        // 415426.
        enumRes = _EnumerateLoadedModules(aProcess, (PENUMLOADED_MODULES_CALLBACK)callbackEspecial, (PVOID)&aAddr);
        if (FALSE != enumRes)
        {
            /*
             * One final go.
             * If it fails, then well, we have other problems.
             */
            retval = _SymGetModuleInfo(aProcess, aAddr, aModuleInfo);
            if (!retval)
                PrintError("SymGetModuleInfo");
        }
    }

    /*
     * If we got module info, we may attempt line info as well.
     * We will not report failure if this does not work.
     */
    if (FALSE != retval && nullptr != aLineInfo && nullptr != _SymGetLineFromAddr) {
        DWORD displacement = 0;
        BOOL lineRes = FALSE;
        lineRes = _SymGetLineFromAddr(aProcess, aAddr, &displacement, aLineInfo);
    }

    return retval;
}

// New members were added to IMAGEHLP_MODULE64 (that show up in the
// Platform SDK that ships with VC8, but not the Platform SDK that ships
// with VC7.1, i.e., between DbgHelp 6.0 and 6.1), but we don't need to
// use them, and it's useful to be able to function correctly with the
// older library.  (Stock Windows XP SP2 seems to ship with dbghelp.dll
// version 5.1.)  Since Platform SDK version need not correspond to
// compiler version, and the version number in debughlp.h was NOT bumped
// when these changes were made, ifdef based on a constant that was
// added between these versions.
#ifdef SSRVOPT_SETCONTEXT
#define NS_IMAGEHLP_MODULE64_SIZE (((offsetof(IMAGEHLP_MODULE64, LoadedPdbName) + sizeof(DWORD64) - 1) / sizeof(DWORD64)) * sizeof(DWORD64))
#else
#define NS_IMAGEHLP_MODULE64_SIZE sizeof(IMAGEHLP_MODULE64)
#endif

#ifdef USING_WXP_VERSION
BOOL SymGetModuleInfoEspecial64(HANDLE aProcess, DWORD64 aAddr, PIMAGEHLP_MODULE64 aModuleInfo, PIMAGEHLP_LINE64 aLineInfo)
{
    BOOL retval = FALSE;

    /*
     * Init the vars if we have em.
     */
    aModuleInfo->SizeOfStruct = NS_IMAGEHLP_MODULE64_SIZE;
    if (nullptr != aLineInfo) {
        aLineInfo->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    }

    /*
     * Give it a go.
     * It may already be loaded.
     */
    retval = _SymGetModuleInfo64(aProcess, aAddr, aModuleInfo);

    if (FALSE == retval) {
        BOOL enumRes = FALSE;

        /*
         * Not loaded, here's the magic.
         * Go through all the modules.
         */
        // Need to cast to PENUMLOADED_MODULES_CALLBACK64 because the
        // constness of the first parameter of
        // PENUMLOADED_MODULES_CALLBACK64 varies over SDK versions (from
        // non-const to const over time).  See bug 391848 and bug
        // 415426.
        enumRes = _EnumerateLoadedModules64(aProcess, (PENUMLOADED_MODULES_CALLBACK64)callbackEspecial64, (PVOID)&aAddr);
        if (FALSE != enumRes)
        {
            /*
             * One final go.
             * If it fails, then well, we have other problems.
             */
            retval = _SymGetModuleInfo64(aProcess, aAddr, aModuleInfo);
        }
    }

    /*
     * If we got module info, we may attempt line info as well.
     * We will not report failure if this does not work.
     */
    if (FALSE != retval && nullptr != aLineInfo && nullptr != _SymGetLineFromAddr64) {
        DWORD displacement = 0;
        BOOL lineRes = FALSE;
        lineRes = _SymGetLineFromAddr64(aProcess, aAddr, &displacement, aLineInfo);
        if (!lineRes) {
            // Clear out aLineInfo to indicate that it's not valid
            memset(aLineInfo, 0, sizeof(*aLineInfo));
        }
    }

    return retval;
}
#endif

bool
EnsureSymInitialized()
{
    static bool gInitialized = false;
    bool retStat;

    if (gInitialized)
        return gInitialized;

    if (!EnsureImageHlpInitialized())
        return false;

    _SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    retStat = _SymInitialize(GetCurrentProcess(), NULL, TRUE);
    if (!retStat)
        PrintError("SymInitialize");

    gInitialized = retStat;
    /* XXX At some point we need to arrange to call _SymCleanup */

    return retStat;
}


EXPORT_XPCOM_API(nsresult)
NS_DescribeCodeAddress(void *aPC, nsCodeAddressDetails *aDetails)
{
    aDetails->library[0] = '\0';
    aDetails->loffset = 0;
    aDetails->filename[0] = '\0';
    aDetails->lineno = 0;
    aDetails->function[0] = '\0';
    aDetails->foffset = 0;

    if (!EnsureSymInitialized())
        return NS_ERROR_FAILURE;

    HANDLE myProcess = ::GetCurrentProcess();
    BOOL ok;

    // debug routines are not threadsafe, so grab the lock.
    EnterCriticalSection(&gDbgHelpCS);

#ifdef USING_WXP_VERSION
    if (_StackWalk64) {
        //
        // Attempt to load module info before we attempt to resolve the symbol.
        // This just makes sure we get good info if available.
        //

        DWORD64 addr = (DWORD64)aPC;
        IMAGEHLP_MODULE64 modInfo;
        IMAGEHLP_LINE64 lineInfo;
        BOOL modInfoRes;
        modInfoRes = SymGetModuleInfoEspecial64(myProcess, addr, &modInfo, &lineInfo);

        if (modInfoRes) {
            PL_strncpyz(aDetails->library, modInfo.ModuleName,
                        sizeof(aDetails->library));
            aDetails->loffset = (char*) aPC - (char*) modInfo.BaseOfImage;
            
            if (lineInfo.FileName) {
                PL_strncpyz(aDetails->filename, lineInfo.FileName,
                            sizeof(aDetails->filename));
                aDetails->lineno = lineInfo.LineNumber;
            }
        }

        ULONG64 buffer[(sizeof(SYMBOL_INFO) +
          MAX_SYM_NAME*sizeof(TCHAR) + sizeof(ULONG64) - 1) / sizeof(ULONG64)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement;
        ok = _SymFromAddr && _SymFromAddr(myProcess, addr, &displacement, pSymbol);

        if (ok) {
            PL_strncpyz(aDetails->function, pSymbol->Name,
                        sizeof(aDetails->function));
            aDetails->foffset = displacement;
        }
    } else
#endif
    {
        //
        // Attempt to load module info before we attempt to resolve the symbol.
        // This just makes sure we get good info if available.
        //

        DWORD_PTR addr = (DWORD_PTR)aPC;
        IMAGEHLP_MODULE modInfo;
        IMAGEHLP_LINE lineInfo;
        BOOL modInfoRes;
        modInfoRes = SymGetModuleInfoEspecial(myProcess, addr, &modInfo, &lineInfo);

        if (modInfoRes) {
            PL_strncpyz(aDetails->library, modInfo.ModuleName,
                        sizeof(aDetails->library));
            aDetails->loffset = (char*) aPC - (char*) modInfo.BaseOfImage;
        }
        
        if (lineInfo.FileName) {
            PL_strncpyz(aDetails->filename, lineInfo.FileName,
                        sizeof(aDetails->filename));
            aDetails->lineno = lineInfo.LineNumber;
        }

#ifdef USING_WXP_VERSION
        ULONG64 buffer[(sizeof(SYMBOL_INFO) +
          MAX_SYM_NAME*sizeof(TCHAR) + sizeof(ULONG64) - 1) / sizeof(ULONG64)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement;

        ok = _SymFromAddr && _SymFromAddr(myProcess, addr, &displacement, pSymbol);
#else
        char buf[sizeof(IMAGEHLP_SYMBOL) + 512];
        PIMAGEHLP_SYMBOL pSymbol = (PIMAGEHLP_SYMBOL) buf;
        pSymbol->SizeOfStruct = sizeof(buf);
        pSymbol->MaxNameLength = 512;

        DWORD displacement;

        ok = _SymGetSymFromAddr(myProcess,
                    frame.AddrPC.Offset,
                    &displacement,
                    pSymbol);
#endif

        if (ok) {
            PL_strncpyz(aDetails->function, pSymbol->Name,
                        sizeof(aDetails->function));
            aDetails->foffset = displacement;
        }
    }

    LeaveCriticalSection(&gDbgHelpCS); // release our lock
    return NS_OK;
}

EXPORT_XPCOM_API(nsresult)
NS_FormatCodeAddressDetails(void *aPC, const nsCodeAddressDetails *aDetails,
                            char *aBuffer, uint32_t aBufferSize)
{
#ifdef USING_WXP_VERSION
    if (_StackWalk64) {
        if (aDetails->function[0])
            _snprintf(aBuffer, aBufferSize, "%s!%s+0x%016lX",
                      aDetails->library, aDetails->function, aDetails->foffset);
        else
            _snprintf(aBuffer, aBufferSize, "0x%016lX", aPC);
    } else {
#endif
        if (aDetails->function[0])
            _snprintf(aBuffer, aBufferSize, "%s!%s+0x%08lX",
                      aDetails->library, aDetails->function, aDetails->foffset);
        else
            _snprintf(aBuffer, aBufferSize, "0x%08lX", aPC);
#ifdef USING_WXP_VERSION
    }
#endif

    aBuffer[aBufferSize - 1] = '\0';

    uint32_t len = strlen(aBuffer);
    if (aDetails->filename[0]) {
        _snprintf(aBuffer + len, aBufferSize - len, " (%s, line %d)\n",
                  aDetails->filename, aDetails->lineno);
    } else {
        aBuffer[len] = '\n';
        if (++len != aBufferSize)
            aBuffer[len] = '\0';
    }
    aBuffer[aBufferSize - 2] = '\n';
    aBuffer[aBufferSize - 1] = '\0';
    return NS_OK;
}

// WIN32 x86 stack walking code
// i386 or PPC Linux stackwalking code or Solaris
#elif HAVE_DLADDR && (HAVE__UNWIND_BACKTRACE || NSSTACKWALK_SUPPORTS_LINUX || NSSTACKWALK_SUPPORTS_SOLARIS || NSSTACKWALK_SUPPORTS_MACOSX)

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nscore.h"
#include <stdio.h>
#include "plstr.h"

// On glibc 2.1, the Dl_info api defined in <dlfcn.h> is only exposed
// if __USE_GNU is defined.  I suppose its some kind of standards
// adherence thing.
//
#if (__GLIBC_MINOR__ >= 1) && !defined(__USE_GNU)
#define __USE_GNU
#endif

// This thing is exported by libstdc++
// Yes, this is a gcc only hack
#if defined(MOZ_DEMANGLE_SYMBOLS)
#include <cxxabi.h>
#include <stdlib.h> // for free()
#endif // MOZ_DEMANGLE_SYMBOLS

void DemangleSymbol(const char * aSymbol, 
                    char * aBuffer,
                    int aBufLen)
{
    aBuffer[0] = '\0';

#if defined(MOZ_DEMANGLE_SYMBOLS)
    /* See demangle.h in the gcc source for the voodoo */
    char * demangled = abi::__cxa_demangle(aSymbol,0,0,0);
    
    if (demangled)
    {
        strncpy(aBuffer,demangled,aBufLen);
        free(demangled);
    }
#endif // MOZ_DEMANGLE_SYMBOLS
}


#if NSSTACKWALK_SUPPORTS_SOLARIS

/*
 * Stack walking code for Solaris courtesy of Bart Smaalder's "memtrak".
 */

#include <synch.h>
#include <ucontext.h>
#include <sys/frame.h>
#include <sys/regset.h>
#include <sys/stack.h>

static int    load_address ( void * pc, void * arg );
static struct bucket * newbucket ( void * pc );
static struct frame * cs_getmyframeptr ( void );
static void   cs_walk_stack ( void * (*read_func)(char * address),
                              struct frame * fp,
                              int (*operate_func)(void *, void *, void *),
                              void * usrarg );
static void   cs_operate ( void (*operate_func)(void *, void *, void *),
                           void * usrarg );

#ifndef STACK_BIAS
#define STACK_BIAS 0
#endif /*STACK_BIAS*/

#define LOGSIZE 4096

/* type of demangling function */
typedef int demf_t(const char *, char *, size_t);

static demf_t *demf;

static int initialized = 0;

#if defined(sparc) || defined(__sparc)
#define FRAME_PTR_REGISTER REG_SP
#endif

#if defined(i386) || defined(__i386)
#define FRAME_PTR_REGISTER EBP
#endif

struct bucket {
    void * pc;
    int index;
    struct bucket * next;
};

struct my_user_args {
    NS_WalkStackCallback callback;
    uint32_t skipFrames;
    uint32_t maxFrames;
    uint32_t numFrames;
    void *closure;
};


static void myinit();

#pragma init (myinit)

static void
myinit()
{

    if (! initialized) {
#ifndef __GNUC__
        void *handle;
        const char *libdem = "libdemangle.so.1";

        /* load libdemangle if we can and need to (only try this once) */
        if ((handle = dlopen(libdem, RTLD_LAZY)) != NULL) {
            demf = (demf_t *)dlsym(handle,
                           "cplus_demangle"); /*lint !e611 */
                /*
                 * lint override above is to prevent lint from
                 * complaining about "suspicious cast".
                 */
        }
#endif /*__GNUC__*/
    }    
    initialized = 1;
}


static int
load_address(void * pc, void * arg)
{
    static struct bucket table[2048];
    static mutex_t lock;
    struct bucket * ptr;
    struct my_user_args * args = (struct my_user_args *) arg;

    unsigned int val = NS_PTR_TO_INT32(pc);

    ptr = table + ((val >> 2)&2047);

    mutex_lock(&lock);
    while (ptr->next) {
        if (ptr->next->pc == pc)
            break;
        ptr = ptr->next;
    }

    int stop = 0;
    if (ptr->next) {
        mutex_unlock(&lock);
    } else {
        (args->callback)(pc, args->closure);
        args->numFrames++;
        if (args->maxFrames != 0 && args->numFrames == args->maxFrames)
          stop = 1;   // causes us to stop getting frames

        ptr->next = newbucket(pc);
        mutex_unlock(&lock);
    }
    return stop;
}


static struct bucket *
newbucket(void * pc)
{
    struct bucket * ptr = (struct bucket *) malloc(sizeof (*ptr));
    static int index; /* protected by lock in caller */
                     
    ptr->index = index++;
    ptr->next = NULL;
    ptr->pc = pc;    
    return (ptr);    
}


static struct frame *
csgetframeptr()
{
    ucontext_t u;
    struct frame *fp;

    (void) getcontext(&u);

    fp = (struct frame *)
        ((char *)u.uc_mcontext.gregs[FRAME_PTR_REGISTER] +
        STACK_BIAS);

    /* make sure to return parents frame pointer.... */

    return ((struct frame *)((ulong_t)fp->fr_savfp + STACK_BIAS));
}


static void
cswalkstack(struct frame *fp, int (*operate_func)(void *, void *, void *),
    void *usrarg)
{

    while (fp != 0 && fp->fr_savpc != 0) {

        if (operate_func((void *)fp->fr_savpc, NULL, usrarg) != 0)
            break;
        /*
         * watch out - libthread stacks look funny at the top
         * so they may not have their STACK_BIAS set
         */

        fp = (struct frame *)((ulong_t)fp->fr_savfp +
            (fp->fr_savfp?(ulong_t)STACK_BIAS:0));
    }
}


static void
cs_operate(int (*operate_func)(void *, void *, void *), void * usrarg)
{
    cswalkstack(csgetframeptr(), operate_func, usrarg);
}

EXPORT_XPCOM_API(nsresult)
NS_StackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void *aClosure, uintptr_t aThread,
             void *aPlatformData)
{
    MOZ_ASSERT(!aThread);
    MOZ_ASSERT(!aPlatformData);
    struct my_user_args args;

    StackWalkInitCriticalAddress();

    if (!initialized)
        myinit();

    args.callback = aCallback;
    args.skipFrames = aSkipFrames; /* XXX Not handled! */
    args.maxFrames = aMaxFrames;
    args.numFrames = 0;
    args.closure = aClosure;
    cs_operate(load_address, &args);
    return args.numFrames == 0 ? NS_ERROR_FAILURE : NS_OK;
}

EXPORT_XPCOM_API(nsresult)
NS_DescribeCodeAddress(void *aPC, nsCodeAddressDetails *aDetails)
{
    aDetails->library[0] = '\0';
    aDetails->loffset = 0;
    aDetails->filename[0] = '\0';
    aDetails->lineno = 0;
    aDetails->function[0] = '\0';
    aDetails->foffset = 0;

    char dembuff[4096];
    Dl_info info;

    if (dladdr(aPC, & info)) {
        if (info.dli_fname) {
            PL_strncpyz(aDetails->library, info.dli_fname,
                        sizeof(aDetails->library));
            aDetails->loffset = (char*)aPC - (char*)info.dli_fbase;
        }
        if (info.dli_sname) {
            aDetails->foffset = (char*)aPC - (char*)info.dli_saddr;
#ifdef __GNUC__
            DemangleSymbol(info.dli_sname, dembuff, sizeof(dembuff));
#else
            if (!demf || demf(info.dli_sname, dembuff, sizeof (dembuff)))
                dembuff[0] = 0;
#endif /*__GNUC__*/
            PL_strncpyz(aDetails->function,
                        (dembuff[0] != '\0') ? dembuff : info.dli_sname,
                        sizeof(aDetails->function));
        }
    }

    return NS_OK;
}

EXPORT_XPCOM_API(nsresult)
NS_FormatCodeAddressDetails(void *aPC, const nsCodeAddressDetails *aDetails,
                            char *aBuffer, uint32_t aBufferSize)
{
    snprintf(aBuffer, aBufferSize, "%p %s:%s+0x%lx\n",
             aPC,
             aDetails->library[0] ? aDetails->library : "??",
             aDetails->function[0] ? aDetails->function : "??",
             aDetails->foffset);
    return NS_OK;
}

#else // not __sun-specific

#if __GLIBC__ > 2 || __GLIBC_MINOR > 1
#define HAVE___LIBC_STACK_END 1
#else
#define HAVE___LIBC_STACK_END 0
#endif

#if HAVE___LIBC_STACK_END
extern void *__libc_stack_end; // from ld-linux.so
#endif
namespace mozilla {
nsresult
FramePointerStackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
                      uint32_t aMaxFrames, void *aClosure, void **bp,
                      void *aStackEnd)
{
  // Stack walking code courtesy Kipp's "leaky".

  int32_t skip = aSkipFrames;
  uint32_t numFrames = 0;
  while (1) {
    void **next = (void**)*bp;
    // bp may not be a frame pointer on i386 if code was compiled with
    // -fomit-frame-pointer, so do some sanity checks.
    // (bp should be a frame pointer on ppc(64) but checking anyway may help
    // a little if the stack has been corrupted.)
    // We don't need to check against the begining of the stack because
    // we can assume that bp > sp
    if (next <= bp ||
        next > aStackEnd ||
        (long(next) & 3)) {
      break;
    }
#if (defined(__ppc__) && defined(XP_MACOSX)) || defined(__powerpc64__)
    // ppc mac or powerpc64 linux
    void *pc = *(bp+2);
    bp += 3;
#else // i386 or powerpc32 linux
    void *pc = *(bp+1);
    bp += 2;
#endif
    if (IsCriticalAddress(pc)) {
      printf("Aborting stack trace, PC is critical\n");
      return NS_ERROR_UNEXPECTED;
    }
    if (--skip < 0) {
      // Assume that the SP points to the BP of the function
      // it called. We can't know the exact location of the SP
      // but this should be sufficient for our use the SP
      // to order elements on the stack.
      (*aCallback)(pc, bp, aClosure);
      numFrames++;
      if (aMaxFrames != 0 && numFrames == aMaxFrames)
        break;
    }
    bp = next;
  }
  return numFrames == 0 ? NS_ERROR_FAILURE : NS_OK;
}

}

#define X86_OR_PPC (defined(__i386) || defined(PPC) || defined(__ppc__))
#if X86_OR_PPC && (NSSTACKWALK_SUPPORTS_MACOSX || NSSTACKWALK_SUPPORTS_LINUX) // i386 or PPC Linux or Mac stackwalking code

EXPORT_XPCOM_API(nsresult)
NS_StackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void *aClosure, uintptr_t aThread,
             void *aPlatformData)
{
  MOZ_ASSERT(!aThread);
  MOZ_ASSERT(!aPlatformData);
  StackWalkInitCriticalAddress();

  // Get the frame pointer
  void **bp;
#if defined(__i386) 
  __asm__( "movl %%ebp, %0" : "=g"(bp));
#else
  // It would be nice if this worked uniformly, but at least on i386 and
  // x86_64, it stopped working with gcc 4.1, because it points to the
  // end of the saved registers instead of the start.
  bp = (void**) __builtin_frame_address(0);
#endif

  void *stackEnd;
#if HAVE___LIBC_STACK_END
  stackEnd = __libc_stack_end;
#else
  stackEnd = reinterpret_cast<void*>(-1);
#endif
  return FramePointerStackWalk(aCallback, aSkipFrames, aMaxFrames,
                               aClosure, bp, stackEnd);

}

#elif defined(HAVE__UNWIND_BACKTRACE)

// libgcc_s.so symbols _Unwind_Backtrace@@GCC_3.3 and _Unwind_GetIP@@GCC_3.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unwind.h>

struct unwind_info {
    NS_WalkStackCallback callback;
    int skip;
    int maxFrames;
    int numFrames;
    bool isCriticalAbort;
    void *closure;
};

static _Unwind_Reason_Code
unwind_callback (struct _Unwind_Context *context, void *closure)
{
    unwind_info *info = static_cast<unwind_info *>(closure);
    void *pc = reinterpret_cast<void *>(_Unwind_GetIP(context));
    // TODO Use something like '_Unwind_GetGR()' to get the stack pointer.
    if (IsCriticalAddress(pc)) {
        printf("Aborting stack trace, PC is critical\n");
        info->isCriticalAbort = true;
        // We just want to stop the walk, so any error code will do.  Using
        // _URC_NORMAL_STOP would probably be the most accurate, but it is not
        // defined on Android for ARM.
        return _URC_FOREIGN_EXCEPTION_CAUGHT;
    }
    if (--info->skip < 0) {
        (*info->callback)(pc, NULL, info->closure);
        info->numFrames++;
        if (info->maxFrames != 0 && info->numFrames == info->maxFrames) {
            // Again, any error code that stops the walk will do.
            return _URC_FOREIGN_EXCEPTION_CAUGHT;
        }
    }
    return _URC_NO_REASON;
}

EXPORT_XPCOM_API(nsresult)
NS_StackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void *aClosure, uintptr_t aThread,
             void *aPlatformData)
{
    MOZ_ASSERT(!aThread);
    MOZ_ASSERT(!aPlatformData);
    StackWalkInitCriticalAddress();
    unwind_info info;
    info.callback = aCallback;
    info.skip = aSkipFrames + 1;
    info.maxFrames = aMaxFrames;
    info.numFrames = 0;
    info.isCriticalAbort = false;
    info.closure = aClosure;

    (void)_Unwind_Backtrace(unwind_callback, &info);

    // We ignore the return value from _Unwind_Backtrace and instead determine
    // the outcome from |info|.  There are two main reasons for this:
    // - On ARM/Android bionic's _Unwind_Backtrace usually (always?) returns
    //   _URC_FAILURE.  See
    //   https://bugzilla.mozilla.org/show_bug.cgi?id=717853#c110.
    // - If aMaxFrames != 0, we want to stop early, and the only way to do that
    //   is to make unwind_callback return something other than _URC_NO_REASON,
    //   which causes _Unwind_Backtrace to return a non-success code.
    if (info.isCriticalAbort)
        return NS_ERROR_UNEXPECTED;
    return info.numFrames == 0 ? NS_ERROR_FAILURE : NS_OK;
}

#endif

EXPORT_XPCOM_API(nsresult)
NS_DescribeCodeAddress(void *aPC, nsCodeAddressDetails *aDetails)
{
  aDetails->library[0] = '\0';
  aDetails->loffset = 0;
  aDetails->filename[0] = '\0';
  aDetails->lineno = 0;
  aDetails->function[0] = '\0';
  aDetails->foffset = 0;

  Dl_info info;
  int ok = dladdr(aPC, &info);
  if (!ok) {
    return NS_OK;
  }

  PL_strncpyz(aDetails->library, info.dli_fname, sizeof(aDetails->library));
  aDetails->loffset = (char*)aPC - (char*)info.dli_fbase;

  const char * symbol = info.dli_sname;
  int len;
  if (!symbol || !(len = strlen(symbol))) {
    return NS_OK;
  }

  char demangled[4096] = "\0";

  DemangleSymbol(symbol, demangled, sizeof(demangled));

  if (strlen(demangled)) {
    symbol = demangled;
    len = strlen(symbol);
  }

  PL_strncpyz(aDetails->function, symbol, sizeof(aDetails->function));
  aDetails->foffset = (char*)aPC - (char*)info.dli_saddr;
  return NS_OK;
}

EXPORT_XPCOM_API(nsresult)
NS_FormatCodeAddressDetails(void *aPC, const nsCodeAddressDetails *aDetails,
                            char *aBuffer, uint32_t aBufferSize)
{
  if (!aDetails->library[0]) {
    snprintf(aBuffer, aBufferSize, "UNKNOWN %p\n", aPC);
  } else if (!aDetails->function[0]) {
    snprintf(aBuffer, aBufferSize, "UNKNOWN [%s +0x%08lX]\n",
                                   aDetails->library, aDetails->loffset);
  } else {
    snprintf(aBuffer, aBufferSize, "%s+0x%08lX [%s +0x%08lX]\n",
                                   aDetails->function, aDetails->foffset,
                                   aDetails->library, aDetails->loffset);
  }
  return NS_OK;
}

#endif

#else // unsupported platform.

EXPORT_XPCOM_API(nsresult)
NS_StackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
             uint32_t aMaxFrames, void *aClosure, uintptr_t aThread,
             void *aPlatformData)
{
    MOZ_ASSERT(!aThread);
    MOZ_ASSERT(!aPlatformData);
    return NS_ERROR_NOT_IMPLEMENTED;
}

namespace mozilla {
nsresult
FramePointerStackWalk(NS_WalkStackCallback aCallback, uint32_t aSkipFrames,
                      void *aClosure, void **bp)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}
}

EXPORT_XPCOM_API(nsresult)
NS_DescribeCodeAddress(void *aPC, nsCodeAddressDetails *aDetails)
{
    aDetails->library[0] = '\0';
    aDetails->loffset = 0;
    aDetails->filename[0] = '\0';
    aDetails->lineno = 0;
    aDetails->function[0] = '\0';
    aDetails->foffset = 0;
    return NS_ERROR_NOT_IMPLEMENTED;
}

EXPORT_XPCOM_API(nsresult)
NS_FormatCodeAddressDetails(void *aPC, const nsCodeAddressDetails *aDetails,
                            char *aBuffer, uint32_t aBufferSize)
{
    aBuffer[0] = '\0';
    return NS_ERROR_NOT_IMPLEMENTED;
}

#endif
