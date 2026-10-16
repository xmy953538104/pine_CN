// SPDX-License-Identifier: MPL-2.0
// A controlled EOF fixture, compiled without CLR or CRT startup/standard streams.
// All JSONL bytes come from the same production-sink fixture used by fake-adb.cs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static WCHAR path_buffer[4096];
static BYTE copy_buffer[4096];

static void write_bytes(HANDLE handle, const BYTE *bytes, DWORD size) {
    while (size) {
        DWORD written;
        if (!WriteFile(handle, bytes, size, &written, NULL) || !written) ExitProcess(79);
        bytes += written;
        size -= written;
    }
}

#define FAIL(text) do { write_bytes(GetStdHandle(STD_ERROR_HANDLE), (const BYTE *)(text), (DWORD)(sizeof(text) - 1)); ExitProcess(78); } while (0)

static void environment_path(const WCHAR *name) {
    DWORD capacity = (DWORD)(sizeof(path_buffer) / sizeof(path_buffer[0]));
    DWORD size = GetEnvironmentVariableW(name, path_buffer, capacity);
    if (!size || size >= capacity) FAIL("Native fake ADB path is missing or too long\n");
}

static void write_pid(const WCHAR *name) {
    char digits[16];
    DWORD pid = GetCurrentProcessId();
    DWORD begin = (DWORD)sizeof(digits);
    HANDLE file;
    do {
        digits[--begin] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid);
    environment_path(name);
    file = CreateFileW(path_buffer, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) FAIL("Native fake ADB cannot create its PID marker\n");
    write_bytes(file, (const BYTE *)digits + begin, (DWORD)sizeof(digits) - begin);
    if (!CloseHandle(file)) FAIL("Native fake ADB cannot close its PID marker\n");
}

void WINAPI FakeAdbMain(void) {
    const WCHAR *argument = GetCommandLineW();
    BOOL quoted = FALSE;
    BOOL discovery = TRUE;
    static const WCHAR devices[] = L"devices";
    DWORD i;
    HANDLE fixture;
    HANDLE output;
    DWORD read;
    DWORD flags;
    // Skip the possibly quoted executable path. The collector supplies only
    // fixed, unquoted command tokens (devices -l, or -s SERIAL logcat ...).
    while (*argument) {
        if (*argument == L'"') quoted = !quoted;
        else if (!quoted && (*argument == L' ' || *argument == L'\t')) break;
        ++argument;
    }
    while (*argument == L' ' || *argument == L'\t') ++argument;
    for (i = 0; discovery && i < (sizeof(devices) / sizeof(devices[0])) - 1; ++i) {
        if (argument[i] != devices[i]) discovery = FALSE;
    }
    if (discovery && argument[i] && argument[i] != L' ' && argument[i] != L'\t') discovery = FALSE;
    if (discovery) {
        static const BYTE devices_text[] = "List of devices attached\ntest-device\tdevice\n";
        write_bytes(GetStdHandle(STD_OUTPUT_HANDLE), devices_text, (DWORD)(sizeof(devices_text) - 1));
        ExitProcess(0);
    }
    write_pid(L"PINE_FAKE_ADB_PID");
    environment_path(L"PINE_FAKE_ADB_FIXTURE");
    fixture = CreateFileW(path_buffer, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fixture == INVALID_HANDLE_VALUE) FAIL("Native fake ADB cannot open the production sink fixture\n");
    output = GetStdHandle(STD_OUTPUT_HANDLE);
    for (;;) {
        if (!ReadFile(fixture, copy_buffer, (DWORD)sizeof(copy_buffer), &read, NULL)) FAIL("Native fake ADB cannot read the fixture\n");
        if (!read) break;
        write_bytes(output, copy_buffer, read);
    }
    if (!CloseHandle(fixture)) FAIL("Native fake ADB cannot close the fixture\n");
    if (!CloseHandle(output)) FAIL("Native fake ADB cannot close stdout\n");
    if (GetHandleInformation(output, &flags) || GetLastError() != ERROR_INVALID_HANDLE) FAIL("Native fake ADB stdout is still valid\n");
    if (!SetStdHandle(STD_OUTPUT_HANDLE, NULL)) FAIL("Native fake ADB cannot clear stdout\n");
    write_pid(L"PINE_FAKE_ADB_CLOSED");
    // Keep both this process and stderr alive. The collector must observe stdout
    // EOF promptly and terminate us, without waiting on stderr or process exit.
    Sleep(60000);
    ExitProcess(0);
}
