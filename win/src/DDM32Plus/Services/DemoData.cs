// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text;
using DDM32Plus.Core.Radio;
using DDM32Plus.Localization;

namespace DDM32Plus.Services;

/// <summary>
/// Made-up reader output for screenshots and demos (--show health:demo, --show live:demo): lines in exactly the form
/// DebugReaders.Health and DebugReaders.Live print, fed to the pages as if read. No port is opened; the pages say
/// that the data is an example.
/// </summary>
public static class DemoData
{
    public static string Note() => Loc.T("Demo_Note");

    private const string Tasks = "tasks.count=5\ntasks.untracked=0\ntasks.min_free=412\ntasks.min_task=ui\ntasks.free.ui=412\n" +
        "tasks.free.IDLE=452\ntasks.free.p25_lab=640\ntasks.free.host=1180\ntasks.free.scan=868\ntasks.heap_free=6304\n" +
        "tasks.heap_min_free=4096";

    private const string Crash = "crash.built=1\ncrash.valid=0";
    private const string Reset = "reset.built=1\nreset.cause=power\nreset.flags=0x00000001\nreset.uptime_ms=0\nreset.warm=0";

    private static IEnumerable<string> Lines(string text) => text.Split('\n');

    public static IEnumerable<string> HealthLines(long uptimeMs = 5_520_000)
    {
        yield return "--- view tasks (20) status 0";
        yield return $"uptime_ms={uptimeMs}";
        foreach (var l in Lines(Tasks))
            yield return l;
        yield return "--- view crash (21) status 0";
        yield return $"uptime_ms={uptimeMs + 1}";
        foreach (var l in Lines(Crash))
            yield return l;
        yield return "--- view reset (23) status 0";
        yield return $"uptime_ms={uptimeMs + 2}";
        foreach (var l in Lines(Reset))
            yield return l;
    }

    public static IEnumerable<string> LiveLines(int polls = 55)
    {
        yield return "--- view crash (21) status 0";
        yield return "uptime_ms=5520001";
        foreach (var l in Lines(Crash))
            yield return l;
        yield return "--- view reset (23) status 0";
        yield return "uptime_ms=5520002";
        foreach (var l in Lines(Reset))
            yield return l;
        var native = NativeModem.Fields;
        for (var n = 1; n <= polls; n++)
        {
            var up = 5_520_000 + n * 2000;
            var receiving = n % 20 is >= 6 and <= 16;
            var rssi = receiving ? -84 + (int)Math.Round(6 * Math.Sin(n * 0.9)) : -121 + n % 3;
            yield return $"=== poll {n}";
            yield return "--- view core (0) status 0";
            yield return $"uptime_ms={up}";
            var core = new StringBuilder();
            core.Append(CultureInfo.InvariantCulture, $"section=core\nview=5\nstate={(receiving ? "rx" : "idle")}\ntx_deny=none\nmode=p25\nvfo=0\n");
            core.Append("zone_index=0\nzone_pos=3\nzone_name=Yerel\nchannel_number=12\nchannel_name=P25 Test\n");
            core.Append(CultureInfo.InvariantCulture, $"rx_hz=434250000\ntx_hz=434250000\nsquelch=3\nmonitor=0\ntone_ok=1\nrssi_dbm={rssi}\nnoise=9");
            foreach (var l in Lines(core.ToString()))
                yield return l;
            yield return "--- view tasks (20) status 0";
            yield return $"uptime_ms={up}";
            foreach (var l in Lines(Tasks))
                yield return l;
            yield return "--- native (0x21/0) status 0";
            foreach (var f in native)
            {
                var v = f switch
                {
                    "version" => 1, "state" => receiving ? 2 : 1, "frequency_hz" => 434_250_000, "rx_count" => 40 + n,
                    "rx_seen" => 42 + n, "p25_sync" => receiving ? 1 : 0, _ => 0,
                };
                yield return $"{f}={v}";
            }
            yield return "state_name=" + (receiving ? "receive" : "armed");
            yield return "reason_name=ok";
            yield return "--- scan (0x21/12) status 0";
            long[] scan = [1, 31 + n / 4, 12 + n / 8, 3, 9 + n / 10, 9 + n / 10, 7 + n / 12, 6 + n / 12, 2, 1, 1, 180, 420];
            for (var i = 0; i < NativeModem.ScanFields.Length; i++)
                yield return $"{NativeModem.ScanFields[i]}={scan[i]}";
            yield return $"=== end {n}";
        }
    }
}
