// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// What a tool run gets from its caller: where its lines go, the clock, the only way to a serial port, the release
/// key and the codec tables. The program passes RadioAccess.Factory or RadioAccess.WriteFactory explicitly; the tests
/// pass a replay of a recorded run or a fake radio. Without one the tool gets no port at all (NoPortGiven), so a
/// caller that forgets it can never reach a real radio, in any build.
/// </summary>
public sealed class ToolContext
{
    public sealed class NoPortGivenException() : InvalidOperationException("No serial port factory was given to this tool run");

    public static readonly SerialFactory NoPort = _ => throw new NoPortGivenException();

    public Action<string> Print { get; init; } = _ => { };
    public IClock Clock { get; init; } = SystemClock.Instance;
    public SerialFactory Serial { get; init; } = NoPort;

    /// <summary>The release key to check a release folder with; null: the embedded one.</summary>
    public string? ReleaseKeyHex { get; init; }

    public Codec Codec { get; init; } = Codec.Dm32;
}

/// <summary>argparse for the tools' few options: an unknown, missing or malformed option ends the run (exit 2).</summary>
internal sealed class Args
{
    private readonly Dictionary<string, string?> values = new(StringComparer.Ordinal);

    public Args(IReadOnlyList<string> argv, IReadOnlyCollection<string> withValue, IReadOnlyCollection<string> flags,
        IReadOnlyCollection<string> required)
    {
        for (var i = 0; i < argv.Count; i++)
        {
            var a = argv[i];
            if (withValue.Contains(a))
            {
                if (i + 1 >= argv.Count)
                    throw new ToolExit("2");
                values[a] = argv[++i];
            }
            else if (flags.Contains(a))
                values[a] = null;
            else
                throw new ToolExit("2");
        }
        if (required.Any(r => !values.ContainsKey(r)))
            throw new ToolExit("2");
    }

    public string? Get(string name) => values.TryGetValue(name, out var v) ? v : null;
    public bool Has(string name) => values.ContainsKey(name);

    public int Int(string name, int fallback)
    {
        if (!values.TryGetValue(name, out var v))
            return fallback;
        // Python int(): optional sign, digits, surrounding whitespace, underscores between digits
        var t = v!.Trim().Replace("_", "", StringComparison.Ordinal);
        return int.TryParse(t, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var n) ? n : throw new ToolExit("2");
    }
}

/// <summary>Files as Python writes them on Windows (text mode: "\n" becomes "\r\n").</summary>
internal static class PyFile
{
    public static void WriteText(string path, string text) =>
        File.WriteAllBytes(path, Encoding.UTF8.GetBytes(text.Replace("\n", "\r\n", StringComparison.Ordinal)));

    /// <summary>open(path, 'x'): only a new file.</summary>
    public static void CreateText(string path, string text)
    {
        using var f = new FileStream(path, FileMode.CreateNew, FileAccess.Write);
        f.Write(Encoding.UTF8.GetBytes(text.Replace("\n", "\r\n", StringComparison.Ordinal)));
    }

    /// <summary>Path.mkdir(parents=True, exist_ok=False)</summary>
    public static void MakeNewDir(string path)
    {
        if (Directory.Exists(path) || File.Exists(path))
            throw new FileExists($"[WinError 183] Cannot create a file when that file already exists: '{path}'");
        Directory.CreateDirectory(path);
    }
}

/// <summary>
/// The install and restore logic of pc/radiotools/install_plus.py and install_firstboot.py that reads only files:
/// the codec slot check, the release folder check (build manifest, linker map, signed release gate), the target
/// images and the write plan.
/// </summary>
public static partial class InstallPlan
{
    public const int Start = 0xC000, Size = 0x100000, Sector = 4096, Slot = 0xE8000;
    public const uint SlotMagic = 0x43524444, SlotV2 = 2;

    public static readonly Dictionary<string, long> CodecSymbols = new()
    {
        ["__codec_slot"] = 0x030e8000, ["__codec_slot_end"] = 0x03100000, ["__codec_run_lo"] = 0x43a00,
        ["__codec_run_hi"] = 0x58000, ["__codec_tab_lo"] = 0x18000e48, ["__codec_tab_hi"] = 0x18003e38,
    };

    private static uint Word(ReadOnlySpan<byte> b, int off) => BitConverter.ToUInt32(b.Slice(off, 4));

    /// <summary>The prepared codec slot: header, version and its own CRC. Returns its stock tag.</summary>
    public static long SlotCheck(byte[] program)
    {
        var s = program.AsSpan(Slot, Size - Slot);
        if (Word(s, 0) != SlotMagic || Word(s, 4) != SlotV2)
            throw new ValueError("No prepared codec slot (v2) at 0xE8000: not the Classic layout (stock firmware: first install with --from-stock)");
        long codeOff = Word(s, 8), codeLen = Word(s, 12), tabOff = Word(s, 20), tabLen = Word(s, 24);
        long relOff = Word(s, 0x44), relCount = Word(s, 0x48), crc = Word(s, 0x40);
        foreach (var (off, n) in new[] { (codeOff, codeLen), (tabOff, tabLen), (relOff, 4 * relCount) })
            if (off + n > s.Length)
                throw new ValueError("Codec slot parts run past the slot");
        var got = Crc32.Compute(s.Slice((int)relOff, (int)(4 * relCount)),
            Crc32.Compute(s.Slice((int)tabOff, (int)tabLen), Crc32.Compute(s.Slice((int)codeOff, (int)codeLen))));
        if (got != crc)
            throw new ValueError("Codec slot CRC mismatch");
        return Word(s, 0x3C);
    }

    [GeneratedRegex(@"(0x[0-9a-fA-F]+)\s+(__codec_\w+)\s*=")]
    private static partial Regex SymbolLine();

    /// <summary>The application of a DDM32 Plus build folder after its build manifest and map checks.</summary>
    public static byte[] BuildApplication(string folder)
    {
        var image = Path.Combine(folder, "rfoundry_dm32.bin");
        var app = File.ReadAllBytes(image);
        using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.ChangeExtension(image, ".json")));
        var m = doc.RootElement;
        bool True(string k) => m.TryGetProperty(k, out var v) && v.ValueKind is not (JsonValueKind.False or JsonValueKind.Null) &&
                               !(v.ValueKind == JsonValueKind.Number && v.GetDouble() == 0) &&
                               !(v.ValueKind == JsonValueKind.String && v.GetString()!.Length == 0);
        if (!True("plus_enabled") || !True("forced_rebuild_identical"))
            throw new ValueError("Expected a reproducible DDM32 Plus build manifest");
        var outputs = new Dictionary<string, string?>();
        if (m.TryGetProperty("outputs", out var o) && o.ValueKind == JsonValueKind.Object)
            foreach (var p in o.EnumerateObject())
                outputs[Path.GetFileName(p.Name.Replace('\\', '/'))] = p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString() : null;
        if (outputs.GetValueOrDefault("rfoundry_dm32.bin") != Hash.Sha(app))
            throw new ValueError("Application differs from its build manifest");
        var mapping = File.ReadAllBytes(Path.ChangeExtension(image, ".map"));
        if (outputs.GetValueOrDefault("rfoundry_dm32.map") != Hash.Sha(mapping))
            throw new ValueError("Linker map differs from its build manifest");
        string text;
        try
        {
            text = new UTF8Encoding(false, true).GetString(mapping);
        }
        catch (DecoderFallbackException)
        {
            throw new ToolError("UnicodeDecodeError", "the linker map is not UTF-8");
        }
        var symbols = new Dictionary<string, long>();
        foreach (Match x in SymbolLine().Matches(text))
            symbols[x.Groups[2].Value] = Convert.ToInt64(x.Groups[1].Value[2..], 16);
        if (CodecSymbols.Any(kv => !symbols.TryGetValue(kv.Key, out var v) || v != kv.Value))
            throw new ValueError("Linker layout differs from the reserved codec location");
        if (app.Length == 0 || app.Length > Slot - Start)
            throw new ValueError("The application must end before the codec slot");
        return app;
    }

    /// <summary>The release's application after its build checks and the signed release gate.</summary>
    public static byte[] PlusApplication(string folder, string? keyHex)
    {
        var app = BuildApplication(folder);
        try
        {
            SignedManifest.ReleaseGate(app, folder, keyHex);
        }
        catch (ManifestException e)
        {
            throw new ValueError(e.Message, "ManifestError");
        }
        return app;
    }

    public static (byte[] Original, byte[] Target, byte[] Data, long Tag) InstallInputs(string backup, string plus, string? keyHex)
    {
        var images = UnitImage.SavedImages(backup);
        var original = images["progmem-pass1.bin"];
        var tag = SlotCheck(original);
        var app = PlusApplication(plus, keyHex);
        byte[] target = [.. original.AsSpan(0, Start), .. app, .. Enumerable.Repeat((byte)0xFF, Slot - Start - app.Length), .. original.AsSpan(Slot)];
        return (original, target, images["q128-pass1.bin"], tag);
    }

    public static (byte[] Original, byte[] Target, byte[] Data, Codec.Profile Profile) StockInputs(string backup, string plus,
        string? keyHex, Codec codec)
    {
        var images = UnitImage.SavedImages(backup);
        var original = images["progmem-pass1.bin"];
        var app = PlusApplication(plus, keyHex);
        var (target, prof) = UnitImage.ComposeFromStock(original, app, codec);
        if (SlotCheck(target) != prof.Tag || !target.AsSpan(0, Start).SequenceEqual(original.AsSpan(0, Start)))
            throw new ValueError("Prepared codec slot did not validate");
        return (original, target, images["q128-pass1.bin"], prof);
    }

    /// <summary>Every application and slot sector rewritten from the backup (a synthetic inverted basis).</summary>
    public static (byte[] Basis, byte[] Target, byte[] Data) RestoreInputs(string backup, bool checkSlot = true)
    {
        var images = UnitImage.SavedImages(backup);
        var target = images["progmem-pass1.bin"];
        if (checkSlot)
            SlotCheck(target);
        var basis = (byte[])target.Clone();
        for (var i = Start; i < basis.Length; i++)
            basis[i] ^= 0xFF;
        return (basis, target, images["q128-pass1.bin"]);
    }

    public static List<int> Todo(byte[] original, byte[] target)
    {
        var todo = new List<int>();
        for (var a = Start; a < Size; a += Sector)
            if (!original.AsSpan(a, Sector).SequenceEqual(target.AsSpan(a, Sector)))
                todo.Add(a);
        return todo;
    }

    /// <summary>The write plan printed before anything else (install_plus.make_plan).</summary>
    public static PyDict MakePlan(string mode, byte[] original, byte[] target, long? tag, Codec.Profile? prof = null)
    {
        var todo = Todo(original, target);
        if (mode is "install_plus" or "install_plus_quick" or "install_plus_no_backup" && todo.Any(a => a >= Slot))
            throw new ToolExit("Refusing: the plan would touch the codec slot");
        if (todo.Any(a => a < Start) || !original.AsSpan(0, Start).SequenceEqual(target.AsSpan(0, Start)))
            throw new ToolExit("Refusing: the plan would touch the boot area");
        var plan = new PyDict
        {
            ["mode"] = mode, ["changed_sectors"] = todo.Count,
            ["first"] = todo.Count > 0 ? PyJson.Hex(todo[0]) : null, ["last"] = todo.Count > 0 ? PyJson.Hex(todo[^1]) : null,
            ["codec_slot_stock_tag"] = tag, ["target_application_sha256"] = Hash.Sha(target.AsSpan(Start)),
            ["boot_area_unchanged"] = original.AsSpan(0, Start).SequenceEqual(target.AsSpan(0, Start)),
            ["codec_slot_unchanged"] = original.AsSpan(Slot).SequenceEqual(target.AsSpan(Slot)),
            ["codec_slot_rewritten_from_backup"] = mode == "restore",
            ["codec_slot_prepared_from_backup"] = mode == "install_plus_from_stock",
            ["data_chip_written"] = false,
        };
        if (prof is not null)
        {
            plan["stock_profile"] = prof.Name;
            plan["stock_version"] = UnitImage.StockVersion(prof);
            plan["stock_version_tested"] = UnitImage.TestedStock.Contains(prof.Name);
            plan["slot_sectors"] = todo.Count(a => a >= Slot);
        }
        return plan;
    }

    public static List<uint> CrcList(byte[] data)
    {
        var out_ = new List<uint>();
        for (var i = 0; i < data.Length; i += Sector)
            out_.Add(Crc32.Compute(data.AsSpan(i, Math.Min(Sector, data.Length - i))));
        return out_;
    }
}

/// <summary>The radio steps shared by the tools (install_firstboot's helpers).</summary>
internal static class Steps
{
    public const int Start = 0xC000, Size = 0x100000, Sector = 4096;

    public static List<uint> DeviceCrcs(MonitorRadio radio, string memory, int size, string label, Action<string> print)
    {
        var out_ = new List<uint>();
        var last = -1;
        for (var a = 0; a < size; a += 16 * Sector)
        {
            var n = Math.Min(16, (size - a) / Sector);
            out_.AddRange(radio.Sums(memory, a, n).Select(s => s.Crc));
            var percent = (int)((long)(a + n * Sector) * 100 / size);
            if (percent / 10 != last)
            {
                print($"{label}: {percent}%");
                last = percent / 10;
            }
        }
        return out_;
    }

    public static byte[] ReadProgram(MonitorRadio radio, string path, string label, Action<string> print, int start = 0)
    {
        if (File.Exists(path) || Directory.Exists(path))
            throw new FileExists(path);
        var partial = path + ".partial";
        var last = -1;
        using (var out_ = new FileStream(partial, FileMode.CreateNew, FileAccess.ReadWrite))
        {
            radio.Dump(out_, "progmem", start, Size, (done, total) =>
            {
                var p = (int)(done * 100 / total);
                if (p / 10 != last)
                {
                    print($"{label}: {p}%");
                    last = p / 10;
                }
            });
            out_.Flush(true);
        }
        var data = File.ReadAllBytes(partial);
        if (data.Length != Size - start)
            throw new ValueError("Incomplete program readback");
        File.Move(partial, path, overwrite: false);
        return data;
    }

    /// <summary>The planned writes, one sector at a time, each read back; never retried (radio.retries = 0).</summary>
    public static void WritePlan(MonitorRadio radio, InstallPort guard, Action<int, int>? progress)
    {
        radio.Retries = 0;
        for (var i = 0; i < guard.Todo.Count; i++)
        {
            var a = guard.Todo[i];
            radio.WriteCmd("progmem", [0x20, (byte)(a >> 16), (byte)(a >> 8), (byte)a], $"erase 0x{a:x}", 3);
            for (var q = a; q < a + Sector; q += 256)
            {
                var page = guard.Target.AsSpan(q, 256);
                if (page.ContainsAnyExcept((byte)0xFF))
                    radio.WriteCmd("progmem", [0x02, (byte)(q >> 16), (byte)(q >> 8), (byte)q, .. page], $"program 0x{q:x}", 1);
            }
            var got = new List<byte>(Sector);
            for (var q = a; q < a + Sector; q += 1024)
                got.AddRange(radio.ReadFlash("progmem", q, 1024));
            if (!got.ToArray().AsSpan().SequenceEqual(guard.Target.AsSpan(a, Sector)))
                throw new ValueError($"Sector readback mismatch at 0x{a:x}; stopped");
            progress?.Invoke(i + 1, guard.Todo.Count);
        }
        guard.FinishWrites();
        radio.Retries = 3;
    }

    /// <summary>The attached chips must be the backup's; sets the guard's read limits.</summary>
    public static void CheckChips(MonitorRadio radio, ReadOnlyPort guard, Dictionary<string, string> expected)
    {
        foreach (var memory in new[] { "progmem", "q128" })
        {
            var got = radio.Jedec(memory);
            if (Convert.ToHexStringLower(got) != expected[memory])
                throw new ValueError("Attached flash IDs differ from the saved unit");
            guard.Limits[memory] = BackupTool.Capacity(got, memory);
        }
    }

    public static void CheckMonitor(MonitorRadio radio)
    {
        if (radio.V6 is null || (long)radio.V6["board"]! != 1 || (long)radio.V6["flags"]! != 0 || radio.Configured is null)
            throw new ValueError("Monitor configuration/identity did not validate");
    }
}

/// <summary>pc/radiotools/dm32_readonly_backup.py: the two-pass backup through the pinned RAM monitor.</summary>
public static class BackupTool
{
    public static long Capacity(byte[] jedec, string memory)
    {
        if (jedec.Length != 3 || jedec[0] is 0 or 255 || jedec[1] is 0 or 255)
            throw new ValueError("Missing/malformed flash JEDEC ID");
        int[] allowed = memory == "progmem" ? [20, 21] : memory == "q128" ? [24] : [];
        if (!allowed.Contains(jedec[2]))
            throw new ValueError($"Unsupported {memory} JEDEC density {Convert.ToHexStringLower(jedec)}; do not assume a capacity");
        return 1L << jedec[2];
    }

    private static PyDict FileRecord(string path, string memory, long size, int capturePass)
    {
        var data = File.ReadAllBytes(path);
        if (data.Length != size)
            throw new ValueError("Incomplete capture");
        var half = (int)(size / 2);
        var blank = !data.AsSpan().ContainsAnyExcept((byte)0xFF);
        var repeated = data.AsSpan(0, half).SequenceEqual(data.AsSpan(half));
        var sectors = new List<string>();
        for (var i = 0; i < size; i += 4096)
            sectors.Add(Hash.Sha(data.AsSpan(i, 4096)));
        return new PyDict
        {
            ["file"] = Path.GetFileName(path), ["memory"] = memory, ["bytes"] = size, ["pass"] = capturePass,
            ["sha256"] = Hash.Sha(data), ["sector_bytes"] = 4096, ["sector_sha256"] = sectors, ["all_blank"] = blank,
            ["suspicious_repeated_halves"] = repeated && !blank,
        };
    }

    private static PyDict Capture(MonitorRadio radio, string memory, long size, string destination, int capturePass,
        Action<string> print)
    {
        var partial = destination + ".partial";
        if (File.Exists(destination))
            throw new FileExists(destination);
        var last = -1L;
        using (var out_ = new FileStream(partial, FileMode.CreateNew, FileAccess.ReadWrite))
        {
            radio.Dump(out_, memory, 0, size, (done, total) =>
            {
                var percent = (int)(done * 100 / total);
                if (percent / 5 != last)
                {
                    last = percent / 5;
                    print($"{memory} pass {capturePass}: {percent}% ({done}/{total})");
                }
            });
            out_.Flush(true);
        }
        var record = FileRecord(partial, memory, size, capturePass);
        File.Move(partial, destination, overwrite: false);
        record["file"] = Path.GetFileName(destination);
        return record;
    }

    private static PyDict Compare(PyDict first, PyDict second)
    {
        if ((string)first["memory"]! != (string)second["memory"]! || (long)first["bytes"]! != (long)second["bytes"]!)
            throw new ValueError("Capture ranges differ");
        var a = (List<string>)first["sector_sha256"]!;
        var b = (List<string>)second["sector_sha256"]!;
        var changed = new List<long>();
        for (var i = 0; i < Math.Min(a.Count, b.Count); i++)
            if (a[i] != b[i])
                changed.Add(i * 4096L);
        var n = (long)first["bytes"]! / 4096;
        if (a.Count != n || b.Count != n)
            throw new ValueError("Missing sector hashes");
        return new PyDict { ["memory"] = first["memory"], ["identical"] = changed.Count == 0, ["changed_sector_addresses"] = changed };
    }

    /// <summary>--port COMx --out NEWDIR --installed-firmware TEXT [--wait 1..120]</summary>
    public static void Run(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var args = new Args(argv, ["--port", "--out", "--installed-firmware", "--wait"], [], ["--port", "--out", "--installed-firmware"]);
        var wait = args.Int("--wait", 120);
        if (wait < 1 || wait > 120)
            throw new ToolExit("2");
        var outDir = args.Get("--out")!;
        _ = RamMonitor.Image;
        PyFile.MakeNewDir(outDir);
        var report = new PyDict
        {
            ["upstream_commit"] = RamMonitor.UpstreamPin, ["host_tool_sha256"] = RamMonitor.HostToolSha256,
            ["ram_monitor_sha256"] = RamMonitor.Sha256, ["installed_firmware_user_reported"] = args.Get("--installed-firmware"),
            ["started_utc"] = ctx.Clock.UtcStamp(), ["flash_written"] = false, ["rf_command_sent"] = false,
            ["backup_verified"] = false, ["captures"] = new List<object?>(),
        };
        var raw = ctx.Serial(args.Get("--port")!);
        var guard = new ReadOnlyPort(raw);
        var manifest = Path.Combine(outDir, "backup-manifest.partial.json");
        try
        {
            raw.Open();
            ctx.Print($"READY: {args.Get("--port")} open; switch radio OFF then ON normally, cable seated, no keys held. Waiting {wait}s.");
            var radio = new MonitorRadio(guard, ctx.Print, ctx.Clock, retries: 3);
            radio.Start(wait);
            if (radio.V6 is null)
                throw new ValueError("Expected pinned universal monitor protocol");
            radio.Configure(MonitorRadio.Dm32);
            report["ram_monitor_footprint"] = radio.V6;
            var chips = new PyDict();
            report["chips"] = chips;
            foreach (var memory in new[] { "progmem", "q128" })
            {
                var jedec = radio.Jedec(memory);
                var size = Capacity(jedec, memory);
                guard.Limits[memory] = size;
                chips[memory] = new PyDict { ["jedec"] = Convert.ToHexStringLower(jedec), ["bytes"] = size };
                ctx.Print($"{memory}: JEDEC {Convert.ToHexStringLower(jedec)}, {size} bytes");
            }
            radio.ProgmemSize = guard.Limits["progmem"];
            PyFile.WriteText(manifest, PyJson.Dumps(report) + "\n");
            var captures = (List<object?>)report["captures"]!;
            foreach (var capturePass in new[] { 1, 2 })
                foreach (var memory in new[] { "progmem", "q128" })
                {
                    captures.Add(Capture(radio, memory, guard.Limits[memory],
                        Path.Combine(outDir, $"{memory}-pass{capturePass}.bin"), capturePass, ctx.Print));
                    PyFile.WriteText(manifest, PyJson.Dumps(report) + "\n");
                }
            var comparison = new List<object?> { Compare((PyDict)captures[0]!, (PyDict)captures[2]!), Compare((PyDict)captures[1]!, (PyDict)captures[3]!) };
            report["comparison"] = comparison;
            report["backup_verified"] = comparison.All(c => (bool)((PyDict)c!)["identical"]!) &&
                                        !captures.Any(c => (bool)((PyDict)c!)["all_blank"]! || (bool)((PyDict)c!)["suspicious_repeated_halves"]!);
            if (!(bool)report["backup_verified"]!)
                throw new ValueError("Captures changed, are blank, or suggest address aliasing; preserve and investigate");
            var data = File.ReadAllBytes(Path.Combine(outDir, "q128-pass1.bin"));
            var codeplug = data.AsSpan(0x1000, 0xc9000 - 0x1000).ToArray();
            using (var f = new FileStream(Path.Combine(outDir, "stock-codeplug-pages.raw.bin"), FileMode.CreateNew))
                f.Write(codeplug);
            report["codeplug_raw"] = new PyDict
            {
                ["file"] = "stock-codeplug-pages.raw.bin", ["start"] = 0x1000, ["end_exclusive"] = 0xc9000,
                ["bytes"] = codeplug.Length, ["sha256"] = Hash.Sha(codeplug), ["vendor_cps_export"] = false,
            };
            report["completed_utc"] = ctx.Clock.UtcStamp();
            report["outgoing_commands"] = guard.Counts;
            PyFile.CreateText(Path.Combine(outDir, "backup-manifest.json"), PyJson.Dumps(report) + "\n");
            ctx.Print("VERIFIED: both complete flash ranges read twice and matching. Power cycle the radio to start its firmware normally.");
        }
        catch (Exception e)
        {
            report["backup_verified"] = false;
            report["error"] = Describe(e);
            report["outgoing_commands"] = guard.Counts;
            PyFile.WriteText(manifest, PyJson.Dumps(report) + "\n");
            throw;
        }
        finally
        {
            raw.Close();
        }
    }

    internal static string Describe(Exception e) => e is ToolError t ? t.Record : $"{e.GetType().Name}: {e.Message}";
}

/// <summary>pc/radiotools/install_plus.py: install a DDM32 Plus release, or put the radio back from its backup.</summary>
public static class InstallTool
{
    /// <summary>--port COMx --backup B (--plus R [--from-stock] | --restore) --out NEWDIR [--wait 1..600] [--plan-only]</summary>
    public static void Run(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var args = new Args(argv, ["--port", "--backup", "--plus", "--out", "--wait"], ["--from-stock", "--restore", "--plan-only"],
            ["--port", "--backup", "--out"]);
        var restore = args.Has("--restore");
        var plus = args.Get("--plus");
        var fromStock = args.Has("--from-stock");
        if (restore == !string.IsNullOrEmpty(plus) || (fromStock && string.IsNullOrEmpty(plus)))
            throw new ToolExit("2");
        var wait = args.Int("--wait", 600);
        if (wait < 1 || wait > 600)
            throw new ToolExit("2");
        var backup = args.Get("--backup")!;
        var outDir = args.Get("--out")!;
        Codec.Profile? prof = null;
        string mode;
        byte[] original, target, data;
        long? tag;
        if (restore)
        {
            mode = "restore";
            (original, target, data) = InstallPlan.RestoreInputs(backup);
            tag = null;
        }
        else if (fromStock)
        {
            mode = "install_plus_from_stock";
            (original, target, data, prof) = InstallPlan.StockInputs(backup, plus!, ctx.ReleaseKeyHex, ctx.Codec);
            tag = prof.Tag;
        }
        else
        {
            mode = "install_plus";
            (original, target, data, var t) = InstallPlan.InstallInputs(backup, plus!, ctx.ReleaseKeyHex);
            tag = t;
        }
        var expectedChips = UnitImage.BackupJedec(backup);
        var plan = InstallPlan.MakePlan(mode, original, target, tag, prof);
        ctx.Print(PyJson.Dumps(plan));
        if (args.Has("--plan-only"))
            return;
        _ = RamMonitor.Image;
        var expectedData = InstallPlan.CrcList(data);
        PyFile.MakeNewDir(outDir);
        var report = plan.Copy();
        report["started_utc"] = ctx.Clock.UtcStamp();
        foreach (var k in new[] { "writes_started", "installation_verified", "bootloader_unchanged", "codec_slot_verified",
                     "data_sector_crcs_unchanged", "restart_sent" })
            report[k] = false;
        var raw = ctx.Serial(args.Get("--port")!);
        var guard = new InstallPort(raw, original, target, skipOriginal: restore);

        void Receipt()
        {
            report["outgoing_operations"] = guard.Counts;
            report["planned_write_frames"] = guard.Plan.Count;
            report["sent_write_frames"] = guard.Index;
            PyFile.WriteText(Path.Combine(outDir, "install-receipt.json"), PyJson.Dumps(report) + "\n");
        }

        try
        {
            raw.Open();
            var radio = new MonitorRadio(guard, ctx.Print, ctx.Clock, retries: 3);
            ctx.Print($"READY on {args.Get("--port")}: turn the radio OFF, then ON normally, no buttons held. Waiting {wait}s.");
            radio.Start(wait);
            radio.Configure(MonitorRadio.Dm32);
            Steps.CheckMonitor(radio);
            radio.CheckWritable();
            Steps.CheckChips(radio, guard, expectedChips);
            if (restore)
            {
                guard.AuthorizeSavedBackup();
                report["writes_started"] = true;
                Receipt();
                ctx.Print($"RESTORING: {guard.Todo.Count} sectors from the backup. Boot and data writes blocked.");
                Steps.WritePlan(radio, guard, null);
                var after = Steps.ReadProgram(radio, Path.Combine(outDir, "application-after.bin"), "Verify restored application", ctx.Print, InstallPlan.Start);
                guard.AuthorizeResetApplication(after);
                report["installation_verified"] = true;
                report["codec_slot_verified"] = after.AsSpan(InstallPlan.Slot - InstallPlan.Start).SequenceEqual(target.AsSpan(InstallPlan.Slot));
                report["installed_application_sha256"] = Hash.Sha(after);
                Receipt();
                ctx.Print("VERIFIED: application and codec slot read back as in the backup. Restarting.");
                radio.Retries = 0;
                radio.ResetChip();
                report["restart_sent"] = true;
                Receipt();
                ctx.Print("PASS: restored. Check the boot screen on the radio.");
                return;
            }
            var before = Steps.ReadProgram(radio, Path.Combine(outDir, "program-before.bin"), "Read current firmware", ctx.Print);
            var beforeData = Steps.DeviceCrcs(radio, "q128", data.Length, "Check settings chip", ctx.Print);
            report["precheck_program_matches_backup"] = before.AsSpan().SequenceEqual(original);
            report["precheck_changed_data_sectors"] = beforeData.Zip(expectedData).Select((p, i) => (p, i))
                .Where(x => x.p.First != x.p.Second).Select(x => (long)x.i * InstallPlan.Sector).ToList();
            Receipt();
            guard.Authorize(before, beforeData, expectedData);
            report["writes_started"] = true;
            Receipt();
            var where = prof is not null ? "application and codec slot sectors" : "application sectors below the codec slot";
            ctx.Print($"INSTALLING: {guard.Todo.Count} {where}. Boot and data writes blocked.");
            var last = -1;
            void Progress(int n, int total)
            {
                var p = n * 100 / total;
                if (p / 5 != last)
                {
                    ctx.Print($"Written and sector-readback checked: {p}% ({n}/{total})");
                    last = p / 5;
                    Receipt();
                }
            }
            var started = ctx.Clock.Monotonic();
            Steps.WritePlan(radio, guard, Progress);
            report["write_seconds"] = ctx.Clock.Monotonic() - started;
            var afterProgram = Steps.ReadProgram(radio, Path.Combine(outDir, "program-after.bin"), "Read complete installed program chip", ctx.Print);
            var afterData = Steps.DeviceCrcs(radio, "q128", data.Length, "Recheck untouched settings chip", ctx.Print);
            if (prof is null)
            {
                if (!afterProgram.AsSpan(InstallPlan.Slot).SequenceEqual(original.AsSpan(InstallPlan.Slot)))
                    throw new ValueError("Codec slot changed; restart disabled");
            }
            else if (!afterProgram.AsSpan(InstallPlan.Slot).SequenceEqual(target.AsSpan(InstallPlan.Slot)) ||
                     InstallPlan.SlotCheck(afterProgram) != tag)
                throw new ValueError("Prepared codec slot did not read back; restart disabled");
            guard.AuthorizeReset(afterProgram, afterData, expectedData);
            foreach (var k in new[] { "installation_verified", "bootloader_unchanged", "codec_slot_verified", "data_sector_crcs_unchanged" })
                report[k] = true;
            report["installed_program_sha256"] = Hash.Sha(afterProgram);
            Receipt();
            ctx.Print("VERIFIED: application, untouched boot area, codec slot and settings chip all read back. Restarting.");
            radio.Retries = 0;
            radio.ResetChip();
            report["restart_sent"] = true;
            Receipt();
            ctx.Print("PASS: DDM32 Plus installed and verified. Check the boot screen on the radio. No RF command was sent.");
        }
        catch (Exception e)
        {
            report["error"] = BackupTool.Describe(e);
            throw;
        }
        finally
        {
            report["finished_utc"] = ctx.Clock.UtcStamp();
            raw.Close();
            Receipt();
        }
    }
}

/// <summary>pc/radiotools/install_firstboot.py --restore-stock --use-saved-backup: the proven stock restore.</summary>
public static class RestoreStockTool
{
    public static void Run(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var args = new Args(argv, ["--port", "--backup", "--out", "--wait"], ["--restore-stock", "--use-saved-backup", "--plan-only"],
            ["--port", "--backup", "--out"]);
        var wait = args.Int("--wait", 120);
        if (wait < 1 || wait > 600)
            throw new ToolExit("2");
        if (!args.Has("--restore-stock") || !args.Has("--use-saved-backup"))
            throw new ToolExit("2");
        var backup = args.Get("--backup")!;
        var outDir = args.Get("--out")!;
        var (original, target, _) = InstallPlan.RestoreInputs(backup, checkSlot: false);
        var expectedChips = UnitImage.BackupJedec(backup);
        var todo = InstallPlan.Todo(original, target);
        var plan = new PyDict
        {
            ["mode"] = "restore_stock", ["changed_sectors"] = todo.Count,
            ["first"] = todo.Count > 0 ? PyJson.Hex(todo[0]) : null, ["last"] = todo.Count > 0 ? PyJson.Hex(todo[^1]) : null,
            ["target_application_sha256"] = Hash.Sha(target.AsSpan(InstallPlan.Start)),
            ["boot_area_unchanged"] = original.AsSpan(0, InstallPlan.Start).SequenceEqual(target.AsSpan(0, InstallPlan.Start)),
            ["codec_slot_unchanged"] = false, ["codec_slot_rewritten_from_backup"] = true, ["data_chip_written"] = false,
        };
        ctx.Print(PyJson.Dumps(plan));
        if (args.Has("--plan-only"))
            return;
        _ = RamMonitor.Image;
        PyFile.MakeNewDir(outDir);
        var report = new PyDict
        {
            ["started_utc"] = ctx.Clock.UtcStamp(), ["mode"] = "restore_stock",
            ["target_application_sha256"] = Hash.Sha(target.AsSpan(InstallPlan.Start)), ["writes_started"] = false,
            ["installation_verified"] = false, ["bootloader_unchanged"] = false, ["data_sector_crcs_unchanged"] = false,
            ["restart_sent"] = false, ["preinstall_reads_requested"] = false, ["write_basis"] = "saved original stock backup",
        };
        var raw = ctx.Serial(args.Get("--port")!);
        var guard = new InstallPort(raw, original, target, skipOriginal: true);

        void Receipt()
        {
            report["outgoing_operations"] = guard.Counts;
            report["planned_write_frames"] = guard.Plan.Count;
            report["sent_write_frames"] = guard.Index;
            PyFile.WriteText(Path.Combine(outDir, "install-receipt.json"), PyJson.Dumps(report) + "\n");
        }

        report["changed_application_sectors"] = guard.Todo.Select(a => (long)a).ToList();
        try
        {
            raw.Open();
            var radio = new MonitorRadio(guard, ctx.Print, ctx.Clock, retries: 3);
            ctx.Print($"READY on {args.Get("--port")}: turn ON normally, no buttons held. Waiting {wait}s.");
            radio.Start(wait);
            radio.Configure(MonitorRadio.Dm32);
            Steps.CheckMonitor(radio);
            radio.CheckWritable();
            Steps.CheckChips(radio, guard, expectedChips);
            ctx.Print("Using the verified saved backup. No pre-write firmware or settings-chip reads.");
            guard.AuthorizeSavedBackup();
            report["writes_started"] = true;
            Receipt();
            ctx.Print($"INSTALLING: {guard.Todo.Count} changed application sectors. Boot/data writes blocked.");
            var last = -1;
            void Progress(int n, int total)
            {
                var p = n * 100 / total;
                if (p / 5 != last)
                {
                    ctx.Print($"Firmware written and sector-readback checked: {p}% ({n}/{total})");
                    last = p / 5;
                    Receipt();
                }
            }
            var started = ctx.Clock.Monotonic();
            Steps.WritePlan(radio, guard, Progress);
            report["write_seconds"] = ctx.Clock.Monotonic() - started;
            var after = Steps.ReadProgram(radio, Path.Combine(outDir, "application-after.bin"), "Verify newly installed application", ctx.Print, InstallPlan.Start);
            guard.AuthorizeResetApplication(after);
            report["installed_application_sha256"] = Hash.Sha(after);
            report["installation_verified"] = true;
            Receipt();
            ctx.Print("VERIFIED: installed application readback matches. Restarting.");
            radio.Retries = 0;
            radio.ResetChip();
            report["restart_sent"] = true;
            Receipt();
            ctx.Print("PASS: stock application restored and verified byte for byte. Confirm the normal stock screen on the radio.");
        }
        catch (Exception e)
        {
            report["error"] = BackupTool.Describe(e);
            throw;
        }
        finally
        {
            report["finished_utc"] = ctx.Clock.UtcStamp();
            raw.Close();
            Receipt();
        }
    }
}
