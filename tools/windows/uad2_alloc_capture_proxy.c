// SPDX-License-Identifier: GPL-2.0-only
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

typedef ULONG_PTR (WINAPI *forward8_fn)(ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                       ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                       ULONG_PTR, ULONG_PTR);
typedef void * (WINAPI *create_plugin2_fn)(int, uint32_t *, int *);
extern IMAGE_DOS_HEADER __ImageBase;

static HMODULE real_module;
static volatile LONG capture_index;

static HMODULE load_real_module(void)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;

    if (real_module != NULL)
        return real_module;
    if (GetModuleFileNameW((HMODULE)&__ImageBase, path, MAX_PATH) == 0)
        return NULL;
    slash = wcsrchr(path, L'\\');
    if (slash == NULL)
        return NULL;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path),
             L"UAD2DriverClient-real.dll");
    real_module = LoadLibraryW(path);
    return real_module;
}

static FARPROC resolve(const char *name)
{
    HMODULE module = load_real_module();
    return module == NULL ? NULL : GetProcAddress(module, name);
}

static ULONG_PTR forward8(const char *name, ULONG_PTR a1, ULONG_PTR a2,
                          ULONG_PTR a3, ULONG_PTR a4, ULONG_PTR a5,
                          ULONG_PTR a6, ULONG_PTR a7, ULONG_PTR a8)
{
    union {
        FARPROC generic;
        forward8_fn typed;
    } function = { .generic = resolve(name) };

    return function.typed == NULL ? 0 :
        function.typed(a1, a2, a3, a4, a5, a6, a7, a8);
}

static void capture_allocation_chain(const uint32_t *record)
{
    wchar_t path[MAX_PATH];
    HANDLE file;
    DWORD written;
    DWORD magic = 0x31464941;
    DWORD version = 1;
    DWORD count = 0;
    LONG index = InterlockedIncrement(&capture_index);
    const uint32_t *cursor = record;

    if (record == NULL)
        return;
    CreateDirectoryW(L"F:\\sdk-recovery", NULL);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"F:\\sdk-recovery\\allocinfo-%lu-%ld.bin",
                 GetCurrentProcessId(), index);
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;

    WriteFile(file, &magic, sizeof(magic), &written, NULL);
    WriteFile(file, &version, sizeof(version), &written, NULL);
    WriteFile(file, &count, sizeof(count), &written, NULL);
    while (cursor != NULL && count < 32) {
        DWORD record_size = cursor[0];
        uint64_t original_pointer = (uint64_t)(uintptr_t)cursor;
        uint64_t next_pointer;
        const uint32_t *next;

        if (record_size < 0xac8 || record_size > 0x4000)
            break;
        next = *(const uint32_t * const *)((const uint8_t *)cursor + 0xac0);
        next_pointer = (uint64_t)(uintptr_t)next;
        WriteFile(file, &record_size, sizeof(record_size), &written, NULL);
        WriteFile(file, &original_pointer, sizeof(original_pointer), &written, NULL);
        WriteFile(file, &next_pointer, sizeof(next_pointer), &written, NULL);
        WriteFile(file, cursor, record_size, &written, NULL);
        count++;
        cursor = next;
    }
    SetFilePointer(file, 8, NULL, FILE_BEGIN);
    WriteFile(file, &count, sizeof(count), &written, NULL);
    CloseHandle(file);
}

void * WINAPI wrap_CreateUAD2PlugIn2(int version, uint32_t *allocation,
                                     int *status)
{
    union {
        FARPROC generic;
        create_plugin2_fn typed;
    } function;

    capture_allocation_chain(allocation);
    function.generic = resolve("CreateUAD2PlugIn2");
    if (function.typed == NULL) {
        if (status != NULL)
            *status = -10;
        return NULL;
    }
    return function.typed(version, allocation, status);
}

#define DEFINE_FORWARD(wrapper, export_name)                                    \
    ULONG_PTR WINAPI wrapper(ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,          \
                             ULONG_PTR a4, ULONG_PTR a5, ULONG_PTR a6,          \
                             ULONG_PTR a7, ULONG_PTR a8)                         \
    {                                                                            \
        return forward8(export_name, a1, a2, a3, a4, a5, a6, a7, a8);          \
    }

DEFINE_FORWARD(wrap_AtomicDecrement, "?AtomicDecrement@UA@@YAHPEAH@Z")
DEFINE_FORWARD(wrap_AtomicIncrement, "?AtomicIncrement@UA@@YAHPEAH@Z")
DEFINE_FORWARD(wrap_CreateUAMutex, "?CreateUAMutex@UA@@YAPEAVCMutex@1@XZ")
DEFINE_FORWARD(wrap_OsDataGetVal, "?OsDataGetVal@UA@@YAIPEBD0PEAIH@Z")
DEFINE_FORWARD(wrap_OsDataLoadVal, "?OsDataLoadVal@UA@@YAIPEBD0PEAIIH@Z")
DEFINE_FORWARD(wrap_OsDataSetVal, "?OsDataSetVal@UA@@YAIPEBD0IH@Z")
DEFINE_FORWARD(wrap_CreateUAD2PlugIn, "CreateUAD2PlugIn")
DEFINE_FORWARD(wrap_DbgPrint, "DbgPrint")
DEFINE_FORWARD(wrap_OpenUAD2ChainManager, "OpenUAD2ChainManager")
DEFINE_FORWARD(wrap_OpenUAD2Driver, "OpenUAD2Driver")
DEFINE_FORWARD(wrap_OpenUAD2Driver2, "OpenUAD2Driver2")

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH && real_module != NULL)
        FreeLibrary(real_module);
    return TRUE;
}
