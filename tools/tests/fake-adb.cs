using System;
using System.IO;
using System.Text;
using System.Threading;
public class FakeAdb {
    public static int Main(string[] args) {
        Console.OutputEncoding = new UTF8Encoding(false);
        var mode = Environment.GetEnvironmentVariable("PINE_FAKE_ADB_MODE");
        File.AppendAllText(Environment.GetEnvironmentVariable("PINE_FAKE_ADB_ARGS"), String.Join(" ", args) + "\n");
        if (args.Length > 0 && args[0] == "devices") {
            Console.WriteLine("List of devices attached");
            if (mode == "none") return 0;
            Console.WriteLine("test-device\t" + (mode == "unauthorized" ? "unauthorized" : mode == "offline" ? "offline" : "device"));
            if (mode == "multiple") Console.WriteLine("second-device\tdevice");
            return 0;
        }
        File.WriteAllText(Environment.GetEnvironmentVariable("PINE_FAKE_ADB_PID"), System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
        Console.WriteLine("--------- beginning of main");
        Console.WriteLine("{\"session_id\":\"session-a\",\"title_id\":\"0100\",\"message\":\"中文 日志\"}");
        Console.WriteLine("{\"session_id\":\"session-b\",\"title_id\":\"0200\",\"message\":\"other\"}");
        Console.Out.Flush();
        if (mode == "disconnect") { Console.Error.WriteLine("device disconnected"); return 1; }
        Thread.Sleep(60000);
        return 0;
    }
}
