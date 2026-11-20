// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The guards between the tools and the serial port (ReadOnlyPort, InstallPort) answer every frame of the Python
/// guards' table the same way (Fixtures/radio/index.json "guards"), and each rule refuses on its own.
/// </summary>
public sealed class RadioGuardTests
{
    private sealed class NullPort : IRadioPort
    {
        public List<byte[]> Sent { get; } = [];
        public double Timeout { get; set; }
        public int BaudRate { get; set; } = 115200;

        public int? Write(byte[] data)
        {
            Sent.Add(data);
            return data.Length;
        }

        public byte[] Read(int n) => [];
        public void ResetInputBuffer() { }
        public void Flush() { }
    }

    private static string Verdict(ReadOnlyPort g, byte[] frame)
    {
        try
        {
            return g.Validate(frame);
        }
        catch (ToolError e)
        {
            return "refused: " + e.Message;
        }
    }

    private static JsonElement Guards => RadioFixtures.Index.GetProperty("guards");

    [Fact]
    public void ReadOnlyGuardBeforeTheMonitorRuns()
    {
        var g = new ReadOnlyPort(new NullPort());
        foreach (var c in Guards.GetProperty("readonly_before_monitor").EnumerateArray())
            Assert.True(c.GetProperty("verdict").GetString() == Verdict(g, Convert.FromHexString(c.GetProperty("frame").GetString()!)),
                c.GetProperty("name").GetString());
    }

    [Fact]
    public void ReadOnlyGuardAnswersEveryFrameAsPython()
    {
        var n = 0;
        foreach (var c in Guards.GetProperty("readonly").EnumerateArray())
        {
            var limits = c.GetProperty("limits");
            var g = new ReadOnlyPort(new NullPort())
            {
                Monitor = true,
                Limits = new() { ["progmem"] = limits.GetProperty("progmem").GetInt64(), ["q128"] = limits.GetProperty("q128").GetInt64() },
            };
            var want = c.GetProperty("verdict").GetString();
            var got = Verdict(g, Convert.FromHexString(c.GetProperty("frame").GetString()!));
            Assert.True(want == got, $"{c.GetProperty("name").GetString()}: Python '{want}', C# '{got}'");
            n++;
        }
        Assert.True(n > 80);
    }

    private static byte[] RtProgram()
    {
        // rtfixtures.make_program(): boot pattern, a 0x9000-byte application, the slot
        byte[] p = [.. TestData.Pattern(0xC000, 7), .. TestData.Pattern(0x9000, 3),
            .. Enumerable.Repeat((byte)0xFF, 0xE8000 - 0xC000 - 0x9000), .. RadioFixtures.MakeSlot()];
        return [.. p, .. Enumerable.Repeat((byte)0xFF, 0x100000 - p.Length)];
    }

    [Fact]
    public void InstallGuardFollowsThePythonSequences()
    {
        var original = RtProgram();
        var target = (byte[])original.Clone();
        TestData.Pattern(0x300, 11).CopyTo(target, 0xC000);
        target[0xC000 + 0x5000] = 0;
        target[0xE8000 + 0x20] ^= 1;
        var crcs = InstallPlan.CrcList(RadioFixtures.MakeData());
        var other = crcs.ToList();
        other[5] ^= 1;
        var named = new Dictionary<string, object>
        {
            ["original"] = original, ["target"] = target, ["crcs"] = crcs, ["other_crcs"] = other,
            ["original_app"] = original[0xC000..], ["target_app"] = target[0xC000..],
        };
        foreach (var seq in Guards.GetProperty("install_sequences").EnumerateArray())
        {
            var name = seq.GetProperty("name").GetString();
            var g = new InstallPort(new NullPort(), original, target, seq.GetProperty("skip_original").GetBoolean())
            {
                Monitor = true,
                Limits = new() { ["progmem"] = 0x100000, ["q128"] = 0x1000000 },
            };
            Assert.Equal(seq.GetProperty("todo").EnumerateArray().Select(x => x.GetInt32()), g.Todo);
            Assert.Equal(seq.GetProperty("plan").EnumerateArray().Select(x => x.GetString()), g.Plan.Select(Convert.ToHexStringLower));
            var i = 0;
            foreach (var step in seq.GetProperty("steps").EnumerateArray())
            {
                var kind = step.GetProperty("step").GetString();
                var arg = step.GetProperty("arg");
                string[] Names() => [.. arg.EnumerateArray().Select(x => x.GetString()!)];
                object result;
                try
                {
                    switch (kind)
                    {
                        case "frame": result = g.Validate(Convert.FromHexString(arg.GetString()!)); break;
                        case "plan": result = g.Validate(g.Plan[g.Index]); break;
                        case "plan_all":
                            while (g.Index < g.Plan.Count)
                                g.Validate(g.Plan[g.Index]);
                            result = "ok";
                            break;
                        case "authorize":
                            g.Authorize((byte[])named[Names()[0]], (IList<uint>)named[Names()[1]], (IList<uint>)named[Names()[2]]);
                            result = "ok";
                            break;
                        case "authorize_saved": g.AuthorizeSavedBackup(); result = "ok"; break;
                        case "finish": g.FinishWrites(); result = "ok"; break;
                        case "reset_ok":
                            g.AuthorizeReset((byte[])named[Names()[0]], (IList<uint>)named[Names()[1]], (IList<uint>)named[Names()[2]]);
                            result = "ok";
                            break;
                        case "reset_app": g.AuthorizeResetApplication((byte[])named[Names()[0]]); result = "ok"; break;
                        case "reset_frames": result = g.ResetPlan.Select(g.Validate).ToList(); break;
                        default: throw new InvalidOperationException(kind);
                    }
                }
                catch (ToolError e)
                {
                    result = "refused: " + e.Message;
                }
                var where = $"{name} step {i++} ({kind})";
                var want = step.GetProperty("result");
                if (want.ValueKind == JsonValueKind.Array)
                    Assert.Equal(want.EnumerateArray().Select(x => x.GetString()), (List<string>)result);
                else
                    Assert.True(want.GetString() == result as string, $"{where}: Python '{want.GetString()}', C# '{result}'");
                Assert.True(step.GetProperty("index").GetInt32() == g.Index, where);
                Assert.True(step.GetProperty("enabled").GetBoolean() == g.Enabled, where);
                Assert.True(step.GetProperty("closed").GetBoolean() == g.ClosedWrites, where);
            }
        }
    }

    // --- each rule on its own --------------------------------------------------------------------------------------
    [Fact]
    public void NeverBelowTheApplicationAreaOrOnTheDataChip()
    {
        var original = RtProgram();
        var boot = (byte[])original.Clone();
        boot[0x100] ^= 1;
        Assert.Throws<ValueError>(() => new InstallPort(new NullPort(), original, boot));
        var target = (byte[])original.Clone();
        target[0xD000] ^= 1;
        var g = new InstallPort(new NullPort(), original, target) { Monitor = true, Limits = new() { ["progmem"] = 0x100000, ["q128"] = 0x1000000 } };
        Assert.All(g.Plan, f => Assert.Equal("040000000100", Convert.ToHexString(f[6..12])));   // the program flash SSI only
        Assert.All(g.Todo, a => Assert.True(a >= 0xC000));
        g.Authorize(original, [1, 2], [1, 2]);
        // a write to the data chip, an erase of the boot area, a whole-chip erase: refused even while writes are on
        byte[] q128 = [0x14, 0x0a, 0x00, 0x00, 0x03, 0x12];
        Assert.StartsWith("refused", Verdict(g, MonitorFrame(5, [.. q128, 0, 1, 0, 0, 0x06])));
        Assert.StartsWith("refused", Verdict(g, RamMonitor.SpiPacket([0x20, 0x00, 0x80, 0x00])));
        Assert.StartsWith("refused", Verdict(g, RamMonitor.SpiPacket([0xC7])));
        // and planned frames only in their order
        Assert.StartsWith("refused", Verdict(g, g.Plan[1]));
        Assert.Equal("program_write_enable", Verdict(g, g.Plan[0]));
        Assert.StartsWith("refused", Verdict(g, g.Plan[0]));                          // not twice
    }

    private static byte[] MonitorFrame(byte op, byte[] payload) => RamMonitor.Packet(op, payload);

    [Fact]
    public void NoRestartBeforeTheReadbackMatched()
    {
        var original = RtProgram();
        var target = (byte[])original.Clone();
        target[0xD000] ^= 1;
        var g = new InstallPort(new NullPort(), original, target) { Monitor = true, Limits = new() { ["progmem"] = 0x100000, ["q128"] = 0x1000000 } };
        var poke = RamMonitor.Packet(3, [.. RamMonitor.Be32Bytes(0x18007EF4), .. RamMonitor.Le32Bytes(0x54464F53)]);
        Assert.StartsWith("refused", Verdict(g, poke));
        g.Authorize(original, [7], [7]);
        while (g.Index < g.Plan.Count)
            g.Validate(g.Plan[g.Index]);
        g.FinishWrites();
        Assert.Throws<ValueError>(() => g.AuthorizeReset(original, [7], [7]));       // the readback is not the target
        Assert.Throws<ValueError>(() => g.AuthorizeReset(target, [8], [7]));         // the data chip changed
        Assert.StartsWith("refused", Verdict(g, poke));
        g.AuthorizeReset(target, [7], [7]);
        Assert.Equal("verified_restart_register", Verdict(g, poke));
        Assert.Throws<ValueError>(() => g.AuthorizeReset(target, [7], [7]));         // once
    }

    [Fact]
    public void OnlyTheExactPinnedMonitorImageIsLoaded()
    {
        var g = new ReadOnlyPort(new NullPort());
        var image = RamMonitor.Image;
        image[10] ^= 1;
        var changed = RamMonitor.RomCmd(0x18, [.. RamMonitor.Be32Bytes(0x10000), .. RamMonitor.Be32Bytes(64), .. image.AsSpan(0, 256)]);
        Assert.StartsWith("refused", Verdict(g, changed));
        Assert.StartsWith("refused", Verdict(g, RamMonitor.RomCmd(0x1b, RamMonitor.Be32Bytes(0x10000))));
        Assert.Throws<ValueError>(() => g.BaudRate = 921600);
    }

    [Fact]
    public void RadioAccessIsOffInThisBuild()
    {
        Assert.False(RadioAccess.Enabled);
        Assert.Throws<RadioAccess.DisabledException>(() => RadioAccess.Factory("COM14"));
        using var p = new SerialRadioPort("COM14");
        Assert.Throws<RadioAccess.DisabledException>(p.Open);
        // a tool with the program's own context stops before any port is opened
        using var tmp = new TempDir();
        var lines = new List<string>();
        var ctx = new ToolContext { Print = lines.Add, Clock = new VirtualClock() };
        Assert.Throws<RadioAccess.DisabledException>(() =>
            BackupTool.Run(["--port", "COM14", "--out", tmp.Sub("b"), "--installed-firmware", "x", "--wait", "5"], ctx));
        Assert.Empty(lines);
    }

    [Fact]
    public void FailedWritesAreNeverRetried()
    {
        // the recorded corrupted-readback runs end right after the failed check: no second erase of any sector
        foreach (var name in new[] { "install_readback_corrupt", "from_stock_slot_corrupt", "restore_readback_corrupt" })
        {
            var sc = RadioFixtures.Scenario(name);
            var erases = new HashSet<string>();
            foreach (var e in sc.GetProperty("events").EnumerateArray().Where(e => e[0].GetString() == "w"))
            {
                var f = Convert.FromBase64String(e[1].GetString()!);
                if (f.Length > 16 && f[1] == 5 && f[16] == 0x20)
                    Assert.True(erases.Add(Convert.ToHexString(f)), $"{name}: a sector was erased twice");
            }
            Assert.NotEmpty(erases);
            Assert.DoesNotContain(sc.GetProperty("events").EnumerateArray().Where(e => e[0].GetString() == "w"),
                e => Convert.FromBase64String(e[1].GetString()!) is [_, 3, ..]);   // no restart register
        }
    }
}
