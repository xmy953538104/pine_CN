using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Runtime.InteropServices;
public class FakeAdb {
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr GetStdHandle(int handle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetStdHandle(int handle, IntPtr value);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetHandleInformation(IntPtr handle, out uint flags);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool WriteFile(IntPtr handle, byte[] bytes, int count, out int written, IntPtr overlapped);
    private static void WriteLine(int standardHandle, string line) {
        // Avoid System.Console entirely: its managed/native stream setup can
        // leave additional stdout references alive on Windows Framework hosts.
        var bytes = new UTF8Encoding(false, true).GetBytes(line + "\n");
        var handle = GetStdHandle(standardHandle);
        while (bytes.Length != 0) {
            int written;
            if (!WriteFile(handle, bytes, bytes.Length, out written, IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            if (written <= 0) throw new IOException("Fake ADB standard-stream write made no progress");
            if (written == bytes.Length) return;
            var remaining = new byte[bytes.Length - written];
            Buffer.BlockCopy(bytes, written, remaining, 0, remaining.Length);
            bytes = remaining;
        }
    }
    private static void CloseOutput() {
        var output = GetStdHandle(-11);
        if (!CloseHandle(output)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        uint flags;
        if (GetHandleInformation(output, out flags) || Marshal.GetLastWin32Error() != 6)
            throw new IOException("Fake ADB stdout handle was not closed");
        if (!SetStdHandle(-11, IntPtr.Zero)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        File.WriteAllText(Environment.GetEnvironmentVariable("PINE_FAKE_ADB_CLOSED"), System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
    }
    public static int Main(string[] args) {
        var mode = Environment.GetEnvironmentVariable("PINE_FAKE_ADB_MODE");
        File.AppendAllText(Environment.GetEnvironmentVariable("PINE_FAKE_ADB_ARGS"), String.Join(" ", args) + "\n");
        if (args.Length > 0 && args[0] == "devices") {
            WriteLine(-11, "List of devices attached");
            if (mode == "discovery-error") { WriteLine(-12, "fake discovery failure"); return 7; }
            if (mode == "none") return 0;
            WriteLine(-11, "test-device\t" + (mode == "unauthorized" ? "unauthorized" : mode == "offline" ? "offline" : mode == "permissions" ? "no permissions (check udev rules)" : "device"));
            if (mode == "multiple") WriteLine(-11, "second-device\tdevice");
            return 0;
        }
        var pidPath = Environment.GetEnvironmentVariable("PINE_FAKE_ADB_PID");
        File.WriteAllText(pidPath + ".tmp", System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
        if (File.Exists(pidPath)) File.Delete(pidPath);
        File.Move(pidPath + ".tmp", pidPath);
        WriteLine(-11, "--------- beginning of main");
        if (mode != "sessions") {
            // Malformed transport input is intentionally synthetic. Valid
            // records below are replayed byte-for-byte from the real sink.
            WriteLine(-11, "not JSON");
            WriteLine(-11, "{\"session_id\":null}");
            WriteLine(-11, "{\"session_id\":5}");
            WriteLine(-11, "{\"session_id\":\"\"}");
            WriteLine(-11, "{\"session_id\":{\"nested\":\"invalid\"}}");
        }
        var fixture = Environment.GetEnvironmentVariable(mode == "sessions" ? "PINE_FAKE_ADB_SESSIONS_FIXTURE" : "PINE_FAKE_ADB_FIXTURE");
        foreach (var line in File.ReadAllLines(fixture, new UTF8Encoding(false, true))) WriteLine(-11, line);
        if (mode == "disconnect") { WriteLine(-12, "device disconnected"); return 1; }
        if (mode == "closed-output") CloseOutput();
        Thread.Sleep(60000);
        return 0;
    }
}
