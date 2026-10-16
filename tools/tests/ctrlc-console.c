// SPDX-License-Identifier: MPL-2.0
// Own-console CTRL_C_EVENT test driver. No parent console, CLR or CRT startup.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0602
#include <windows.h>

static WCHAR executable[4096], command[32768], ready_path[4096], report_path[4096];
static HANDLE report = INVALID_HANDLE_VALUE, job, adb, delivered;
static PROCESS_INFORMATION child;
static JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
static STARTUPINFOW startup;
static volatile LONG received;
static BOOL report_failed;

static void text(const char *value) {
    DWORD size = 0, written;
    while (value[size]) ++size;
    while (size) {
        if (!WriteFile(report, value, size, &written, NULL) || !written) {
            report_failed = TRUE;
            return;
        }
        value += written;
        size -= written;
    }
}

static void digits(DWORD value) {
    char buffer[16];
    DWORD begin = (DWORD)sizeof(buffer);
    buffer[--begin] = '\0';
    do {
        buffer[--begin] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    text(buffer + begin);
}

static void number(const char *key, DWORD value) {
    text(key); text("="); digits(value); text("\n");
}

static BOOL outcome(const char *api, BOOL result) {
    // Capture immediately: logging itself calls Win32 APIs.
    DWORD error = result ? ERROR_SUCCESS : GetLastError();
    text(api); text(result ? " result=1 " : " result=0 ");
    number("error", error);
    return result && !report_failed;
}

static BOOL environment(const WCHAR *name, WCHAR *buffer, DWORD capacity) {
    DWORD size = GetEnvironmentVariableW(name, buffer, capacity);
    if (!size || size >= capacity) { text("Missing or oversized test environment path\n"); return FALSE; }
    return TRUE;
}

static BOOL WINAPI control(DWORD event) {
    if (event != CTRL_C_EVENT) return FALSE;
    InterlockedIncrement(&received);
    SetEvent(delivered);
    return TRUE;
}

static DWORD wait_process(const char *label, HANDLE process, DWORD timeout) {
    DWORD result = WaitForSingleObject(process, timeout);
    DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    text(label); text(" result="); digits(result); text(" "); number("error", error);
    return result;
}

static BOOL exit_code(HANDLE process) {
    DWORD code;
    if (!outcome("GetExitCodeProcess", GetExitCodeProcess(process, &code))) return FALSE;
    number("powershellExitCode", code);
    return !report_failed;
}

static DWORD ready_pid(void) {
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 15000) {
        char pid_text[16];
        DWORD size, i, pid = 0, error, state;
        HANDLE file;
        state = WaitForSingleObject(child.hProcess, 0);
        if (state != WAIT_TIMEOUT) {
            error = state == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            number("powerShellReadyWaitResult", state); number("powerShellReadyWaitError", error);
            text("PowerShell exited before the complete fixture was saved\n");
            exit_code(child.hProcess);
            return 0;
        }
        file = CreateFileW(ready_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                number("readyFileError", error);
                return 0;
            }
            Sleep(25);
            continue;
        }
        if (!outcome("ReadReadyFile", ReadFile(file, pid_text, (DWORD)sizeof(pid_text), &size, NULL))) {
            CloseHandle(file);
            return 0;
        }
        CloseHandle(file);
        if (!size || size > 10) return 0;
        for (i = 0; i < size; ++i) {
            DWORD digit = (DWORD)(pid_text[i] - '0');
            if (digit > 9 || pid > (MAXDWORD - digit) / 10) return 0;
            pid = pid * 10 + digit;
        }
        return pid;
    }
    text("Timed out waiting for all three files and 12 native fixture records\n");
    return 0;
}

void WINAPI CtrlCTestMain(void) {
    DWORD members[4], count, i, adb_pid, resumed, remaining, elapsed;
    BOOL attached = FALSE, assigned = FALSE, passed = FALSE, in_job = FALSE;
    BOOL saw_self = FALSE, saw_child = FALSE;
    ULONGLONG signaled_at;
    DWORD capacity = (DWORD)(sizeof(report_path) / sizeof(report_path[0]));
    if (!environment(L"PINE_CTRLC_REPORT", report_path, capacity)) ExitProcess(80);
    report = CreateFileW(report_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (report == INVALID_HANDLE_VALUE) ExitProcess(81);
    number("helperPid", GetCurrentProcessId());
    if (!environment(L"PINE_CTRLC_EXE", executable, (DWORD)(sizeof(executable) / sizeof(executable[0]))) ||
        !environment(L"PINE_CTRLC_COMMAND", command, (DWORD)(sizeof(command) / sizeof(command[0]))) ||
        !environment(L"PINE_CTRLC_READY", ready_path, (DWORD)(sizeof(ready_path) / sizeof(ready_path[0])))) goto cleanup;

    // /SUBSYSTEM:WINDOWS must leave this helper detached from the CI console.
    SetLastError(ERROR_SUCCESS);
    count = GetConsoleProcessList(members, (DWORD)(sizeof(members) / sizeof(members[0])));
    i = GetLastError();
    number("initialConsoleMembers", count); number("initialConsoleError", i);
    if (count || i != ERROR_INVALID_HANDLE) { text("Helper was not detached; no signal sent\n"); goto cleanup; }

    job = CreateJobObjectW(NULL, NULL);
    if (!outcome("CreateJobObject", job != NULL)) goto cleanup;
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!outcome("SetInformationJobObject", SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, (DWORD)sizeof(limits)))) goto cleanup;
    startup.cb = (DWORD)sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    // Suspend until the private Job contains the process, before it can spawn ADB.
    if (!outcome("CreateProcess", CreateProcessW(executable, command, NULL, NULL, FALSE,
        CREATE_NEW_CONSOLE | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &startup, &child))) goto cleanup;
    number("powershellPid", child.dwProcessId);
    if (!outcome("AssignProcessToJobObject", AssignProcessToJobObject(job, child.hProcess))) goto cleanup;
    assigned = TRUE;
    resumed = ResumeThread(child.hThread);
    if (!outcome("ResumeThread", resumed != MAXDWORD)) goto cleanup;
    CloseHandle(child.hThread); child.hThread = NULL;

    adb_pid = ready_pid();
    number("fakeAdbPid", adb_pid);
    if (!adb_pid || adb_pid == child.dwProcessId || adb_pid == GetCurrentProcessId()) goto cleanup;
    // Keep the original process handle through the signal and all exit assertions.
    adb = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, adb_pid);
    if (!outcome("OpenFakeAdbProcess", adb != NULL) ||
        !outcome("IsFakeAdbInJob", IsProcessInJob(adb, job, &in_job))) goto cleanup;
    number("fakeAdbInOwnJob", (DWORD)in_job);
    if (!in_job || wait_process("fakeAdbBeforeSignal", adb, 0) != WAIT_TIMEOUT) goto cleanup;

    if (!outcome("AttachConsole", AttachConsole(child.dwProcessId))) goto cleanup;
    attached = TRUE;
    delivered = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!outcome("CreateDeliveryEvent", delivered != NULL)) goto cleanup;
    // Attach resets handlers. This handler affects only the helper, not PowerShell.
    if (!outcome("SetConsoleCtrlHandler", SetConsoleCtrlHandler(control, TRUE))) goto cleanup;
    count = GetConsoleProcessList(members, (DWORD)(sizeof(members) / sizeof(members[0])));
    if (!outcome("GetConsoleProcessList", count != 0)) goto cleanup;
    number("consoleMembers", count);
    if (count > (DWORD)(sizeof(members) / sizeof(members[0]))) goto cleanup;
    for (i = 0; i < count; ++i) {
        number("consolePid", members[i]);
        if (members[i] == GetCurrentProcessId()) saw_self = TRUE;
        else if (members[i] == child.dwProcessId) saw_child = TRUE;
        else { text("Unexpected console member; no signal sent\n"); goto cleanup; }
    }
    if (count != 2 || !saw_self || !saw_child ||
        wait_process("powershellBeforeSignal", child.hProcess, 0) != WAIT_TIMEOUT || report_failed) goto cleanup;

    // CTRL_C cannot target a nonzero process group. Broadcast only in this
    // just-verified private console. There is deliberately no Ctrl+Break fallback.
    text("signal=CTRL_C_EVENT event=0 group=0 attempts=1\n");
    if (report_failed) goto cleanup;
    signaled_at = GetTickCount64();
    if (!outcome("GenerateConsoleCtrlEvent", GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))) goto cleanup;
    if (wait_process("helperReceivedCtrlC", delivered, 10000) != WAIT_OBJECT_0) goto cleanup;
    elapsed = (DWORD)(GetTickCount64() - signaled_at);
    remaining = elapsed < 10000 ? 10000 - elapsed : 0;
    if (wait_process("powershellAfterSignal", child.hProcess, remaining) != WAIT_OBJECT_0) goto cleanup;
    elapsed = (DWORD)(GetTickCount64() - signaled_at);
    number("signalToExitMilliseconds", elapsed);
    if (elapsed > 10000) goto cleanup;
    if (!exit_code(child.hProcess)) goto cleanup;
    count = (DWORD)InterlockedCompareExchange(&received, 0, 0);
    number("helperCtrlCEvents", count);
    if (count != 1) goto cleanup;
    // ADB has no console (the production collector uses CreateNoWindow), so its
    // original handle must be signaled by collector cleanup, before Job closure.
    if (wait_process("fakeAdbAfterSignal", adb, 0) != WAIT_OBJECT_0 || report_failed) goto cleanup;
    passed = TRUE;

cleanup:
    if (!passed) {
        if (assigned) {
            text("failureCleanup=privateJob\n");
            outcome("TerminateJobObject", TerminateJobObject(job, 82));
        } else if (child.hProcess) {
            text("failureCleanup=ownSuspendedProcess\n");
            outcome("TerminateProcess", TerminateProcess(child.hProcess, 82));
        }
        if (child.hProcess) wait_process("cleanupPowerShell", child.hProcess, 2000);
    } else {
        text("failureCleanup=none\n");
    }
    if (attached && !outcome("FreeConsole", FreeConsole())) passed = FALSE;
    if (child.hThread) CloseHandle(child.hThread);
    if (child.hProcess) CloseHandle(child.hProcess);
    if (adb) CloseHandle(adb);
    if (delivered) CloseHandle(delivered);
    // On success both child handles were already signaled before this can kill anything.
    if (job) CloseHandle(job);
    text(passed && !report_failed ? "result=PASS\n" : "result=FAIL\n");
    CloseHandle(report);
    ExitProcess(passed && !report_failed ? 0 : 83);
}
