using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Runtime.InteropServices;
public class FakeAdb {
    [DllImport("kernel32.dll")] private static extern IntPtr GetStdHandle(int handle);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    public static int Main(string[] args) {
        Console.OutputEncoding = new UTF8Encoding(false);
        var mode = Environment.GetEnvironmentVariable("PINE_FAKE_ADB_MODE");
        File.AppendAllText(Environment.GetEnvironmentVariable("PINE_FAKE_ADB_ARGS"), String.Join(" ", args) + "\n");
        if (args.Length > 0 && args[0] == "devices") {
            Console.WriteLine("List of devices attached");
            if (mode == "discovery-error") { Console.Error.WriteLine("fake discovery failure"); return 7; }
            if (mode == "none") return 0;
            Console.WriteLine("test-device\t" + (mode == "unauthorized" ? "unauthorized" : mode == "offline" ? "offline" : mode == "permissions" ? "no permissions (check udev rules)" : "device"));
            if (mode == "multiple") Console.WriteLine("second-device\tdevice");
            return 0;
        }
        var pidPath = Environment.GetEnvironmentVariable("PINE_FAKE_ADB_PID");
        File.WriteAllText(pidPath + ".tmp", System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
        if (File.Exists(pidPath)) File.Delete(pidPath);
        File.Move(pidPath + ".tmp", pidPath);
        Console.WriteLine("--------- beginning of main");
        if (mode != "sessions") {
            // Malformed transport input is intentionally synthetic. Valid
            // records below are replayed byte-for-byte from the real sink.
            Console.WriteLine("not JSON");
            Console.WriteLine("{\"session_id\":null}");
            Console.WriteLine("{\"session_id\":5}");
            Console.WriteLine("{\"session_id\":\"\"}");
            Console.WriteLine("{\"session_id\":{\"nested\":\"invalid\"}}");
        }
        var fixture = Environment.GetEnvironmentVariable(mode == "sessions" ? "PINE_FAKE_ADB_SESSIONS_FIXTURE" : "PINE_FAKE_ADB_FIXTURE");
        foreach (var line in File.ReadAllLines(fixture, new UTF8Encoding(false, true))) Console.WriteLine(line);
        Console.Out.Flush();
        if (mode == "disconnect") { Console.Error.WriteLine("device disconnected"); return 1; }
        if (mode == "closed-output") CloseHandle(GetStdHandle(-11));
        Thread.Sleep(60000);
        return 0;
    }
}
