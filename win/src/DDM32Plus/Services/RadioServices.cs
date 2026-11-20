// SPDX-License-Identifier: GPL-3.0-or-later
using System.Management;
using System.Text;
using System.Windows.Threading;
using DDM32Plus.Core.Radio;
using DDM32Plus.Localization;

namespace DDM32Plus.Services;

/// <summary>
/// Windows' device list (Win32_PnPEntity) for the CH340 cable: only reads the list, opens no port. A cable without
/// its driver is listed too (problem code 28), so the missing driver is found here.
/// </summary>
public static class CableScanner
{
    public static CableStatus Scan()
    {
        try
        {
            var devices = new List<PnpDevice>();
            using var q = new ManagementObjectSearcher(
                "SELECT PNPDeviceID, Name, ConfigManagerErrorCode FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'USB\\\\VID_1A86%'");
            foreach (var o in q.Get())
            {
                using (o)
                    devices.Add(new PnpDevice(o["PNPDeviceID"] as string ?? "", o["Name"] as string ?? "",
                        Convert.ToInt32(o["ConfigManagerErrorCode"] ?? 0, System.Globalization.CultureInfo.InvariantCulture)));
            }
            return CableCheck.Evaluate(devices);
        }
        catch (Exception e) when (e is ManagementException or System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
        {
            return new CableStatus(CableState.NoCable, null, 0, []);
        }
    }

    /// <summary>The cable's state in a sentence (tr/en).</summary>
    public static string Text(CableStatus s) => s.State switch
    {
        CableState.Ok => Loc.F("Cable_Ok", s.Port),
        CableState.NoDriver => Loc.F("Cable_NoDriver", s.ErrorCode),
        CableState.Problem => Loc.F("Cable_Problem", s.ErrorCode),
        CableState.NoPort => Loc.T("Cable_NoPort"),
        _ => Loc.T("Cable_None"),
    };

    public static string Short(CableStatus s, string? port) => s.State switch
    {
        CableState.Ok => port ?? s.Port ?? "",
        CableState.NoDriver => Loc.T("Cable_StatusNoDriver"),
        _ => Loc.T("Cable_StatusNone"),
    };
}

/// <summary>How a tool run ended: its error (null when it passed), or that radio access is off in this build.</summary>
public sealed record RunEnd(ToolError? Error, bool AccessOff);

/// <summary>
/// Runs one radio tool (backup, install, restore) off the window's thread. Its serial port comes only from
/// RadioAccess.Factory: in a build without radio access the tool stops before any port is opened. Every line goes to
/// the page and to a log file next to the run's folder; READY lines start the "switch the radio on" countdown.
/// </summary>
public sealed class ToolRunner(Dispatcher dispatcher)
{
    public bool Busy { get; private set; }

    /// <param name="writes">The tool writes to the radio (install, restore): its port comes from
    /// RadioAccess.WriteFactory, off even in the developer build.</param>
    public async Task<RunEnd> RunAsync(Action<IReadOnlyList<string>, ToolContext> tool, List<string> argv, string logPath,
        Action<string> line, bool writes)
    {
        Busy = true;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
            await using var log = new StreamWriter(logPath, append: true, new UTF8Encoding(false));
            await log.WriteLineAsync($"# {DateTime.Now:yyyy-MM-dd HH:mm:ss} {string.Join(' ', argv)}");
            void Print(string s)
            {
                foreach (var l in s.Split('\n'))
                {
                    lock (log)
                    {
                        log.WriteLine(l);
                        log.Flush();
                    }
                    dispatcher.BeginInvoke(() => line(l));
                }
            }
            var ctx = new ToolContext { Print = Print, Serial = writes ? RadioAccess.WriteFactory : RadioAccess.Factory, Codec = Codec.Dm32 };
            return await Task.Run(() =>
            {
                try
                {
                    tool(argv, ctx);
                    return new RunEnd(null, false);
                }
                catch (RadioAccess.DisabledException)
                {
                    Print("Radio access is off in this build: no port was opened.");
                    return new RunEnd(new ToolError("RadioAccessOff", "no port was opened"), true);
                }
                catch (ToolError e)
                {
                    Print(e.Record);
                    return new RunEnd(e, false);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or TimeoutException or InvalidOperationException)
                {
                    var err = new ToolError("OSError", e.Message);
                    Print(err.Record);
                    return new RunEnd(err, false);
                }
            });
        }
        finally
        {
            Busy = false;
        }
    }

    /// <summary>(port, seconds) of a READY line: the tool has the port open and waits for the radio.</summary>
    public static (string Port, int Seconds)? Ready(string line)
    {
        var m = System.Text.RegularExpressions.Regex.Match(line, @"^READY(?::| on) (\S+?):? .*Waiting (\d+)s\.");
        return m.Success ? (m.Groups[1].Value, int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture)) : null;
    }

    /// <summary>Backup progress 0..100 from a progress line (two passes of 1 MiB + 16 MiB), or null.</summary>
    public static int? BackupProgress(string line)
    {
        var m = System.Text.RegularExpressions.Regex.Match(line, @"^(progmem|q128) pass ([12]): \d+% \((\d+)/\d+\)");
        if (!m.Success)
            return null;
        const long p = 1 << 20, q = 1 << 24;
        var b = (int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture) - 1) * (p + q) + (m.Groups[1].Value == "q128" ? p : 0);
        return (int)Math.Min(100, (b + long.Parse(m.Groups[3].Value, System.Globalization.CultureInfo.InvariantCulture)) * 100 / (2 * (p + q)));
    }

    /// <summary>Install progress from "...: N% (n/total)" lines, or null.</summary>
    public static int? StepProgress(string line)
    {
        var m = System.Text.RegularExpressions.Regex.Match(line, @"checked: (\d+)% \(");
        return m.Success ? int.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture) : null;
    }
}
