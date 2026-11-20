// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using DDM32Plus.Core.Radio;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The C# FakeRadio against the Python FakeRadio (the C# tools must send it the bytes the Python tools sent to the
/// Python one), then the two faster update paths on it: (A) quick update with a reused backup and (B) install without
/// backup. No serial port: the radio is in memory.
/// </summary>
public sealed class FastInstallTests(RadioInputs inputs) : IClassFixture<RadioInputs>
{
    private const int Start = 0xC000, Slot = 0xE8000, Size = 0x100000, Sector = 4096;

    private static List<byte[]> PythonWrites(string scenario) =>
        [.. RadioFixtures.Scenario(scenario).GetProperty("events").EnumerateArray()
            .Where(e => e[0].GetString() == "w").Select(e => Convert.FromBase64String(e[1].GetString()!))];

    private (FakeRadio Radio, List<string> Lines, ToolError? Error) Run(Action<IReadOnlyList<string>, ToolContext> tool,
        byte[] program, List<string> argv, byte[]? data = null, Action<FakeRadio>? setup = null, bool stock = false)
    {
        var clock = new VirtualClock();
        var radio = new FakeRadio(program, data ?? inputs.Data, clock);
        setup?.Invoke(radio);
        var lines = new List<string>();
        var opened = false;
        var ctx = new ToolContext
        {
            Print = s => lines.AddRange(s.Split('\n')), Clock = clock, ReleaseKeyHex = TestData.TestPublicHex,
            Codec = stock ? RadioFixtures.SyntheticCodecTables() : Codec.Dm32,
            Serial = _ =>
            {
                opened = true;
                return radio;
            },
        };
        ToolError? error = null;
        try
        {
            tool(["--port", "SIMULATED", .. argv], ctx);
        }
        catch (ToolError e)
        {
            error = e;
        }
        if (!opened)
            Assert.Empty(radio.Sent);
        return (radio, lines, error);
    }

    private string Out(string name) => Path.Combine(inputs.Root, $"fast-{name}-{Guid.NewGuid():N}"[..40]);

    // --- the C# FakeRadio is the Python one, as far as the tools can tell ----------------------------------------
    public static TheoryData<string> CrossChecked() =>
    [
        "backup_ok", "backup_radio_off", "install_ok", "install_live_program_changed", "install_readback_corrupt",
        "install_slot_changed", "install_wrong_flash_id", "restore_ok", "restore_readback_corrupt", "from_stock_ok",
        "restore_stock_ok",
    ];

    [Theory]
    [MemberData(nameof(CrossChecked))]
    public void TheCSharpFakeRadioDrawsTheSameBytesAsThePythonOne(string name)
    {
        var sc = RadioFixtures.Scenario(name);
        var r = sc.GetProperty("radio");
        byte[] program = r.GetProperty("image").GetString() switch
        {
            var h when h == Hash.Sha(inputs.Program) => inputs.Program,
            var h when h == Hash.Sha(inputs.Target) => inputs.Target,
            var h when h == Hash.Sha(inputs.Stock) => inputs.Stock,
            var h when h == Hash.Sha(Changed()) => Changed(),
            var h when h == Hash.Sha(Broken()) => Broken(),
            var h when h == Hash.Sha(StockInstalled()) => StockInstalled(),
            var h => throw new InvalidOperationException("unknown image " + h),
        };
        var outDir = Out(name);
        var argv = sc.GetProperty("args").EnumerateArray().Select(a => a.GetString()!
            .Replace("{out}", outDir).Replace("{backup}", inputs.Backup).Replace("{stock_backup}", inputs.StockBackup)
            .Replace("{plus}", inputs.Plus)).ToList();
        Action<IReadOnlyList<string>, ToolContext> tool = sc.GetProperty("tool").GetString() switch
        {
            "backup" => BackupTool.Run,
            "install_plus" => InstallTool.Run,
            _ => RestoreStockTool.Run,
        };
        var (radio, lines, error) = Run(tool, program, argv, setup: f =>
        {
            if (r.TryGetProperty("data_jedec", out var j))
                f.DataJedec = j.GetString()!;
            if (r.TryGetProperty("corrupt_after_writes", out var c))
                f.CorruptAfterWrites = c.GetInt64();
            if (r.GetProperty("setup").ValueKind == JsonValueKind.String)
                f.Silent = true;
        }, stock: sc.GetProperty("stock_codec").GetBoolean());
        var want = PythonWrites(name);
        Assert.Equal(want.Count, radio.Sent.Count);
        for (var i = 0; i < want.Count; i++)
            Assert.True(want[i].AsSpan().SequenceEqual(radio.Sent[i]), $"{name}: write {i} differs");
        Assert.Equal(sc.GetProperty("error").ValueKind == JsonValueKind.Null, error is null);
        Assert.Empty(radio.Errors);
        _ = lines;
    }

    private byte[] Changed()
    {
        var c = (byte[])inputs.Program.Clone();
        c[Start + 0x20000] ^= 0xff;
        return c;
    }

    private byte[] Broken()
    {
        var b = (byte[])inputs.Target.Clone();
        Array.Fill(b, (byte)0xFF, Start + 3 * Sector, 2 * Sector);
        Array.Clear(b, Slot + 0x200, 16);
        return b;
    }

    private byte[] StockInstalled() => [.. inputs.Stock.AsSpan(0, Start), .. inputs.Program.AsSpan(Start)];

    // --- (A) quick update: an earlier backup of the same radio --------------------------------------------------
    /// <summary>The radio now runs another DDM32 Plus build than when the backup was taken (boot and slot the same).</summary>
    private byte[] RadioWithOtherBuild() =>
        RadioFixtures.Program([.. RadioFixtures.Pattern(0x900, 21), .. RadioFixtures.Rep(0x9000), .. "DDM32 Plus 1.1"u8]);

    private List<string> Quick(string outDir) => ["--quick", "--backup", inputs.Backup, "--plus", inputs.Plus, "--out", outDir, "--wait", "5"];

    private static List<(int Op, long Addr, int Count, string Mem)> Reads(IEnumerable<byte[]> frames)
    {
        var reads = new List<(int, long, int, string)>();
        foreach (var f in frames.Where(f => f.Length > 6 && f[0] == 2))
        {
            if (f[1] == 9 && f.Length == 18)
                reads.Add((9, RamMonitor.Be32(f.AsSpan(12)), f[16], f[6] == 0x04 ? "progmem" : "q128"));
            if (f[1] == 7 && f.Length == 18)
                reads.Add((7, RamMonitor.Be32(f.AsSpan(12)), f[16] << 8 | f[17], f[6] == 0x04 ? "progmem" : "q128"));
        }
        return reads;
    }

    private static int FirstWrite(FakeRadio radio) =>
        radio.Sent.FindIndex(f => f.Length > 16 && f[1] == 5 && f[16] is 0x20 or 0x02);

    private static JsonElement Receipt(string outDir) =>
        JsonDocument.Parse(File.ReadAllBytes(Path.Combine(outDir, "install-receipt.json"))).RootElement;

    [Fact]
    public void QuickUpdateRewritesTheApplicationAndNothingElse()
    {
        var outDir = Out("quick-ok");
        var (radio, lines, error) = Run(FastInstallTool.Run, RadioWithOtherBuild(), Quick(outDir));
        Assert.Null(error);
        Assert.Contains(lines, l => l.StartsWith("PASS:", StringComparison.Ordinal));
        Assert.Equal(inputs.Target, radio.Images["progmem"]);
        Assert.Equal(inputs.Data, radio.Images["q128"]);
        Assert.All(radio.Writes, w => Assert.True(w.Mem == "progmem" && w.Addr >= Start && w.Addr < Slot));
        Assert.Equal(Enumerable.Range(0, (Slot - Start) / Sector).Select(i => (long)(Start + i * Sector)),
            radio.Writes.Where(w => w.Op == 0x20).Select(w => w.Addr));                // every application sector, once
        Assert.Equal(7, radio.Pokes.Count);
        // before the first write the program flash was read only in the boot area and the codec slot
        var first = FirstWrite(radio);
        var before = Reads(radio.Sent.Take(first)).Where(r => r is { Op: 9, Mem: "progmem" }).ToList();
        Assert.NotEmpty(before);
        Assert.All(before, r => Assert.True(r.Addr + r.Count * Sector <= Start || r.Addr >= Slot, $"read at 0x{r.Addr:X}"));
        var rc = Receipt(outDir);
        foreach (var k in new[] { "installation_verified", "codec_slot_verified", "data_sector_crcs_unchanged", "restart_sent",
                     "live_boot_area_matches_backup", "live_codec_slot_matches_backup" })
            Assert.True(rc.GetProperty(k).GetBoolean(), k);
        Assert.Equal("reused", rc.GetProperty("backup").GetString());
        Assert.Equal(["install-receipt.json"], Directory.GetFiles(outDir).Select(Path.GetFileName));
    }

    [Theory]
    [InlineData("boot", 0x100)]
    [InlineData("slot", Slot + 0x90)]
    public void QuickUpdateRefusesAnotherRadio(string what, int at)
    {
        var program = RadioWithOtherBuild();
        program[at] ^= 1;
        var outDir = Out("quick-" + what);
        var (radio, _, error) = Run(FastInstallTool.Run, program, Quick(outDir));
        Assert.NotNull(error);
        Assert.Contains("does not match the backup", error.Message);
        Assert.Empty(radio.Writes);
        Assert.Empty(radio.Pokes);
        Assert.False(Receipt(outDir).GetProperty("writes_started").GetBoolean());
    }

    [Fact]
    public void QuickUpdateRefusesOtherFlashChips()
    {
        var (radio, _, error) = Run(FastInstallTool.Run, RadioWithOtherBuild(), Quick(Out("quick-jedec")), setup: f => f.DataJedec = "ef4017");
        Assert.Equal("Attached flash IDs differ from the saved unit", error?.Message);
        Assert.Empty(radio.Writes);
        Assert.DoesNotContain(9, radio.Ops);
    }

    [Fact]
    public void QuickUpdateNeverFromStockOrOverAnotherFirmware()
    {
        var outDir = Out("quick-stock");
        var (radio, _, error) = Run(FastInstallTool.Run, inputs.Stock,
            ["--quick", "--backup", inputs.StockBackup, "--plus", inputs.Plus, "--out", outDir], stock: true);
        Assert.Contains("first install from stock firmware needs a new backup", error?.Message);
        Assert.Empty(radio.Sent);                                                        // refused before any port
        Assert.False(Directory.Exists(outDir));
        var open25 = Path.Combine(inputs.Root, "open25-backup-" + Guid.NewGuid().ToString("N")[..6]);
        RadioFixtures.WriteBackup(open25, RadioFixtures.Program([.. "open25-classic-r13"u8, .. RadioFixtures.Pattern(0x900, 3)]), inputs.Data);
        (radio, _, error) = Run(FastInstallTool.Run, inputs.Program, ["--quick", "--backup", open25, "--plus", inputs.Plus, "--out", Out("q25")]);
        Assert.Contains("only for an update over an installed DDM32 Plus", error?.Message);
        Assert.Empty(radio.Sent);
    }

    [Fact]
    public void QuickUpdatePlanOnlyOpensNoPortAndKeepsTheSlot()
    {
        var (radio, lines, error) = Run(FastInstallTool.Run, inputs.Program, [.. Quick(Out("quick-plan")), "--plan-only"]);
        Assert.Null(error);
        Assert.Empty(radio.Sent);
        var plan = JsonDocument.Parse(string.Join("\n", lines)).RootElement;
        Assert.Equal("install_plus_quick", plan.GetProperty("mode").GetString());
        Assert.Equal((Slot - Start) / Sector, plan.GetProperty("changed_sectors").GetInt32());
        Assert.Equal("0xe7000", plan.GetProperty("last").GetString());
        Assert.True(plan.GetProperty("codec_slot_unchanged").GetBoolean());
        Assert.False(plan.GetProperty("data_chip_written").GetBoolean());
    }

    [Fact]
    public void QuickUpdateReadbackFailureBlocksTheRestartWithoutRetry()
    {
        var (radio, _, error) = Run(FastInstallTool.Run, RadioWithOtherBuild(), Quick(Out("quick-bad")), setup: f => f.CorruptAfterWrites = Start + 0x10);
        Assert.Equal("Post-write verification failed; reset remains disabled", error?.Message);
        Assert.Empty(radio.Pokes);
        Assert.Equal(radio.Writes.Count(w => w.Op == 0x20), radio.Writes.Where(w => w.Op == 0x20).Select(w => w.Addr).Distinct().Count());
    }

    // --- (B) install without backup ------------------------------------------------------------------------------
    private List<string> NoBackup(string outDir) => ["--no-backup", "--plus", inputs.Plus, "--out", outDir, "--wait", "5"];

    [Fact]
    public void NoBackupReadsTheProgramOnceAndKeepsEveryCheck()
    {
        var radioNow = RadioWithOtherBuild();
        var outDir = Out("nb-ok");
        var (radio, lines, error) = Run(FastInstallTool.Run, radioNow, NoBackup(outDir));
        Assert.Null(error);
        Assert.Contains(lines, l => l.StartsWith("PASS:", StringComparison.Ordinal));
        byte[] target = [.. radioNow.AsSpan(0, Start), .. RadioFixtures.PlusApp,
            .. Enumerable.Repeat((byte)0xFF, Slot - Start - RadioFixtures.PlusApp.Length), .. radioNow.AsSpan(Slot)];
        Assert.Equal(target, radio.Images["progmem"]);
        Assert.Equal(inputs.Data, radio.Images["q128"]);
        Assert.All(radio.Writes, w => Assert.True(w.Mem == "progmem" && w.Addr >= Start && w.Addr < Slot));
        // one live read of the whole program flash before the first write: every sector exactly once
        var first = FirstWrite(radio);
        var sectors = Reads(radio.Sent.Take(first)).Where(r => r is { Op: 9, Mem: "progmem" })
            .SelectMany(r => Enumerable.Range(0, r.Count).Select(i => r.Addr + i * Sector)).ToList();
        Assert.Equal(Enumerable.Range(0, Size / Sector).Select(i => (long)i * Sector), sectors);
        // kept in memory: no image file is left, only the receipt
        Assert.Equal(["install-receipt.json"], Directory.GetFiles(outDir).Select(Path.GetFileName));
        var rc = Receipt(outDir);
        Assert.Equal("none", rc.GetProperty("backup").GetString());
        Assert.Equal(Hash.Sha(radioNow), rc.GetProperty("live_program_sha256").GetString());
        Assert.True(rc.GetProperty("restart_sent").GetBoolean());
        Assert.True(rc.GetProperty("data_sector_crcs_unchanged").GetBoolean());
    }

    [Fact]
    public void NoBackupNeverFromStock()
    {
        var outDir = Out("nb-stock");
        var (radio, _, error) = Run(FastInstallTool.Run, inputs.Stock, NoBackup(outDir), stock: true);
        Assert.Contains("first install from stock firmware needs a new backup", error?.Message);
        Assert.Empty(radio.Writes);
        Assert.Empty(radio.Pokes);
        Assert.False(Receipt(outDir).GetProperty("writes_started").GetBoolean());
        (radio, _, error) = Run(FastInstallTool.Run, RadioFixtures.Program([.. "open25-classic-r13"u8, .. RadioFixtures.Pattern(0x900, 3)]), NoBackup(Out("nb-25")));
        Assert.Contains("only for an update over an installed DDM32 Plus", error?.Message);
        Assert.Empty(radio.Writes);
    }

    [Fact]
    public void NoBackupReadbackFailureBlocksTheRestart()
    {
        var (radio, _, error) = Run(FastInstallTool.Run, RadioWithOtherBuild(), NoBackup(Out("nb-bad")), setup: f => f.CorruptAfterWrites = Slot + 0x90);
        Assert.Equal("Codec slot changed; restart disabled", error?.Message);
        Assert.Empty(radio.Pokes);
    }

    [Theory]
    [InlineData("--no-backup", "--quick")]
    [InlineData("--no-backup", "--plan-only")]
    [InlineData("--quick")]
    [InlineData("--no-backup", "--backup")]
    public void ArgumentRefusals(params string[] flags)
    {
        List<string> argv = ["--plus", inputs.Plus, "--out", Out("args")];
        foreach (var f in flags)
        {
            argv.Add(f);
            if (f == "--backup")
                argv.Add(inputs.Backup);
        }
        var (radio, _, error) = Run(FastInstallTool.Run, inputs.Program, argv);
        Assert.Equal("SystemExit", error?.PyType);
        Assert.Empty(radio.Sent);
    }

    // --- the guard's new rules --------------------------------------------------------------------------------------
    private sealed class NullPort : IRadioPort
    {
        public double Timeout { get; set; }
        public int BaudRate { get; set; } = 115200;
        public int? Write(byte[] data) => data.Length;
        public byte[] Read(int n) => [];
        public void ResetInputBuffer() { }
        public void Flush() { }
    }

    [Fact]
    public void IdentityGuardRules()
    {
        var (basis, target, _) = FastInstallTool.QuickInputs(inputs.Backup, inputs.Plus, TestData.TestPublicHex, Codec.Dm32);
        var g = new InstallPort(new NullPort(), basis, target, kind: InstallPort.Authorization.Identity) { Monitor = true };
        Assert.All(g.Todo, a => Assert.True(a < Slot));
        Assert.Throws<ValueError>(() => g.Authorize(basis, [1], [1]));                 // not the full rule here
        var otherBoot = target[..Start];
        otherBoot[5] ^= 1;
        Assert.Throws<ValueError>(() => g.AuthorizeIdentity(otherBoot, target[Slot..]));
        Assert.StartsWith("refused", Verdict(g, g.Plan[0]));
        g.AuthorizeIdentity(target[..Start], target[Slot..]);
        Assert.Equal("program_write_enable", g.Validate(g.Plan[0]));
        // an identity port can never plan the slot
        var slotChanged = (byte[])target.Clone();
        slotChanged[Slot + 4] ^= 1;
        Assert.Throws<ValueError>(() => new InstallPort(new NullPort(), basis, slotChanged, kind: InstallPort.Authorization.Identity));
        // and the full port refuses the identity rule
        var full = new InstallPort(new NullPort(), basis, target);
        Assert.Throws<ValueError>(() => full.AuthorizeIdentity(target[..Start], target[Slot..]));
    }

    [Fact]
    public void DeferredPlanIsReadOnlyUntilSetOnceAndAuthorized()
    {
        var g = new InstallPort(new NullPort()) { Monitor = true, Limits = new() { ["progmem"] = Size, ["q128"] = 0x1000000 } };
        Assert.Throws<ValueError>(() => g.Authorize(inputs.Program, [1], [1]));          // no plan yet
        Assert.StartsWith("refused", Verdict(g, RamMonitor.SpiPacket([0x06])));
        Assert.Equal("sector_read", g.Validate(RamMonitor.Packet(9, [0x04, 0, 0, 0, 1, 0, 0, 0, 0, 0, 16, 1])));
        g.SetImages(inputs.Program, inputs.Target);
        Assert.Throws<ValueError>(() => g.SetImages(inputs.Program, inputs.Program));    // once
        g.Authorize(inputs.Program, [1], [1]);
        Assert.Equal("program_write_enable", g.Validate(g.Plan[0]));
    }

    private static string Verdict(ReadOnlyPort g, byte[] f)
    {
        try
        {
            return g.Validate(f);
        }
        catch (ToolError e)
        {
            return "refused: " + e.Message;
        }
    }
}
