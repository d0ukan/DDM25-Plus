// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;

namespace DDM32Plus.Core.Radio;

/// <summary>
/// Offline helpers on the user's own verified backup (pc/radiotools/prepare_unit_image.py): the saved images, the
/// recorded chip IDs, the stock profile of a program image and the first-install image from stock. Never opens a
/// port and never writes a file. The composed image holds the radio's own codec: it stays in memory.
/// </summary>
public static class UnitImage
{
    public const int Start = 0xC000, Size = 0x100000, Sector = 4096, Slot = 0xE8000;
    public static readonly HashSet<string> TestedStock = ["DM32.01.01.047.bin"];

    private static JsonElement Get(JsonElement o, string key) =>
        o.ValueKind == JsonValueKind.Object && o.TryGetProperty(key, out var v) ? v
            : throw new ToolError("KeyError", $"'{key}'");

    /// <summary>Both passes of both chips from a verified two-pass backup, checked against its manifest.</summary>
    public static Dictionary<string, byte[]> SavedImages(string folder)
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "backup-manifest.json")));
        var m = doc.RootElement;
        var names = new HashSet<string>();
        foreach (var mem in new[] { "progmem", "q128" })
            for (var n = 1; n <= 2; n++)
                names.Add($"{mem}-pass{n}.bin");
        var verified = m.TryGetProperty("backup_verified", out var bv) && Truthy(bv);
        var captures = m.TryGetProperty("captures", out var c) ? c : default;
        if (!verified || captures.ValueKind != JsonValueKind.Array ||
            !captures.EnumerateArray().Select(r => Get(r, "file").GetString()!).ToHashSet().SetEquals(names))
            throw new ValueError("A complete verified two-pass backup is required");
        var images = new Dictionary<string, byte[]>();
        foreach (var record in captures.EnumerateArray())
        {
            var name = Get(record, "file").GetString()!;
            var data = File.ReadAllBytes(Path.Combine(folder, name));
            if (data.Length != Get(record, "bytes").GetInt64() || Hash.Sha(data) != Get(record, "sha256").GetString())
                throw new ValueError("Saved image length/hash differs from its manifest");
            var sectors = Get(record, "sector_sha256").EnumerateArray().Select(x => x.GetString()).ToList();
            var mine = new List<string>();
            for (var i = 0; i < data.Length; i += Sector)
                mine.Add(Hash.Sha(data.AsSpan(i, Math.Min(Sector, data.Length - i))));
            if (!sectors.SequenceEqual(mine))
                throw new ValueError("Saved sector hashes differ");
            images[name] = data;
        }
        foreach (var mem in new[] { "progmem", "q128" })
            if (!images[mem + "-pass1.bin"].AsSpan().SequenceEqual(images[mem + "-pass2.bin"]))
                throw new ValueError("Saved passes differ");
        if (images["progmem-pass1.bin"].Length != 0x100000 || images["q128-pass1.bin"].Length != 0x1000000)
            throw new ValueError("Candidate board preset requires 1 MiB program and 16 MiB data flash");
        return images;
    }

    private static bool Truthy(JsonElement e) => e.ValueKind switch
    {
        JsonValueKind.True => true,
        JsonValueKind.String => e.GetString()!.Length > 0,
        JsonValueKind.Number => e.GetDouble() != 0,
        JsonValueKind.Array => e.GetArrayLength() > 0,
        JsonValueKind.Object => e.EnumerateObject().Any(),
        _ => false,
    };

    /// <summary>The JEDEC IDs the backup recorded ({"progmem": "c84014", "q128": "ef4018"}).</summary>
    public static Dictionary<string, string> BackupJedec(string folder)
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "backup-manifest.json")));
        var chips = doc.RootElement.TryGetProperty("chips", out var c) && c.ValueKind == JsonValueKind.Object ? c : default;
        var out_ = new Dictionary<string, string>();
        foreach (var memory in new[] { "progmem", "q128" })
        {
            var jedec = "";
            if (chips.ValueKind == JsonValueKind.Object && chips.TryGetProperty(memory, out var chip) &&
                chip.ValueKind == JsonValueKind.Object && chip.TryGetProperty("jedec", out var j))
                jedec = (j.ValueKind == JsonValueKind.String ? j.GetString()! : j.GetRawText()).ToLowerInvariant();
            if (jedec.Length != 6 || jedec.Any(ch => !"0123456789abcdef".Contains(ch)))
                throw new ValueError("The backup does not record its flash chip IDs; take a new backup with this program");
            out_[memory] = jedec;
        }
        return out_;
    }

    /// <summary>The codec tool after checking that its DM32 windows are the DDM32 Plus layout.</summary>
    public static Codec CheckCodec(Codec codec)
    {
        var b = codec.Board;
        if (b.SlotLo != 0x030E8000 || b.SlotHi != 0x03100000 || b.RunLo != 0x43A00 || b.RunHi != 0x58000 ||
            b.AppBase != 0x0300C000 || b.TablesLo != 0x18000E48 || b.TablesHi > 0x18003E38)
            throw new CodecError("The codec tool's DM32 windows differ from the DDM32 Plus layout");
        return codec;
    }

    public static bool HasSlot(Codec codec, byte[] program) => codec.ParseSlot(program.AsSpan(Slot, Size - Slot)) is not null;

    /// <summary>The stock profile of a program image (no slot, the codec where that stock build keeps it), or null.</summary>
    public static Codec.Profile? StockProfile(Codec codec, byte[] program)
    {
        if (program.Length != Size || HasSlot(codec, program))
            return null;
        Codec.Profile? prof;
        try
        {
            prof = codec.Identify(program, "dm32");
        }
        catch (Exception e) when (e is ToolError or ArgumentException or IndexOutOfRangeException)
        {
            return null;
        }
        if (prof is null || prof.Board != "dm32")
            return null;
        var codeAt = prof.Code.Addr - Codec.ProgmemBase;
        var tablesAt = prof.Tables.Addr - Codec.ProgmemBase;
        if (!(Start <= codeAt && codeAt < Slot && Start <= tablesAt && tablesAt < Slot))
            return null;
        return prof;
    }

    /// <summary>'047' from 'DM32.01.01.047.bin'; the profile name otherwise.</summary>
    public static string StockVersion(Codec.Profile prof)
    {
        var parts = prof.Name.Replace(".bin", "", StringComparison.Ordinal).Split('.');
        return parts.Length >= 4 && parts[^1].Length > 0 && parts[^1].All(char.IsAsciiDigit) ? parts[^1] : prof.Name;
    }

    /// <summary>(target image, stock profile) for a first install from stock: boot area unchanged, the application
    /// from 0xC000, the radio's own codec in the slot. CodecError for anything that is not a known stock layout.</summary>
    public static (byte[] Target, Codec.Profile Profile) ComposeFromStock(byte[] program, byte[] application, Codec codec)
    {
        CheckCodec(codec);
        if (program.Length != Size)
            throw new CodecError("Expected a 1 MiB program flash image");
        if (HasSlot(codec, program))
            throw new CodecError("A prepared codec slot is already present: this radio has the Classic layout, install over it instead");
        var prof = StockProfile(codec, program) ?? throw new CodecError(
            "Unknown stock firmware: its codec was not found where a supported stock version keeps it");
        if (application.Length == 0 || application.Length > Slot - Start)
            throw new CodecError("The application must end before the codec slot");
        byte[] raw, code, tables;
        try
        {
            (prof, code, tables) = codec.Extract(program, "dm32");
            raw = codec.Merge(application, prof, code, tables);
        }
        catch (ToolError e)
        {
            throw new CodecError($"Codec preparation failed: {e.Message}");
        }
        if (raw.Length > Size - Start || !raw.AsSpan(0, application.Length).SequenceEqual(application))
            throw new CodecError("Merged image does not fit the application area");
        var slot = codec.ParseSlot(raw.AsSpan(Slot - Start));
        if (slot is null || (long)slot["tag"]! != prof.Tag || (string)slot["code_sha256"]! != Hash.Sha(code) ||
            (string)slot["tables_sha256"]! != Hash.Sha(tables))
            throw new CodecError("Codec slot did not validate");
        byte[] target = [.. program.AsSpan(0, Start), .. raw, .. Enumerable.Repeat((byte)0xFF, Size - Start - raw.Length)];
        if (target.Length != Size || !target.AsSpan(0, Start).SequenceEqual(program.AsSpan(0, Start)))
            throw new CodecError("Composed image does not keep the boot area");
        return (target, prof);
    }
}

/// <summary>What a DM-32UV runs, from its backup only (pc/radiotools/radio_detect.py). No port, nothing written.</summary>
public static class Detect
{
    private static readonly byte[][] Ddm32Markers = ["DDM32 Plus"u8.ToArray(), "DDM32"u8.ToArray(), "DDM25 Plus"u8.ToArray(), "DDM25"u8.ToArray()];
    private static readonly byte[][] ClassicMarkers = ["open25-classic"u8.ToArray(), "Open25"u8.ToArray(), "open25-"u8.ToArray()];

    public static int ApplicationEnd(byte[] program)
    {
        var app = program.AsSpan(UnitImage.Start, UnitImage.Slot - UnitImage.Start);
        var n = app.Length;
        while (n > 0 && app[n - 1] == 0xFF)
            n--;
        return UnitImage.Start + n;
    }

    private static bool Any(ReadOnlySpan<byte> app, IEnumerable<byte[]> markers)
    {
        foreach (var m in markers)
            if (app.IndexOf(m) >= 0)
                return true;
        return false;
    }

    public static PyDict Run(byte[] program, Codec codec)
    {
        UnitImage.CheckCodec(codec);
        if (program.Length != UnitImage.Size)
            throw new ValueError("Expected a 1 MiB program flash image");
        var app = program.AsSpan(UnitImage.Start, UnitImage.Slot - UnitImage.Start);
        var out_ = new PyDict
        {
            ["layout"] = "unknown", ["firmware"] = "unknown", ["codec_tag"] = null, ["stock_profile"] = null,
            ["stock_version"] = null, ["stock_tested"] = false, ["install_mode"] = null, ["supported"] = false,
            ["reason"] = "", ["application_end"] = PyJson.Hex(ApplicationEnd(program)),
        };
        var slot = codec.ParseSlot(program.AsSpan(UnitImage.Slot, UnitImage.Size - UnitImage.Slot));
        if (slot is not null)
        {
            out_["layout"] = "classic";
            out_["codec_tag"] = slot["tag"];
            out_["install_mode"] = "over_classic";
            out_["supported"] = true;
            out_["firmware"] = Any(app, Ddm32Markers) ? "ddm32" : Any(app, ClassicMarkers) ? "classic" : "unknown";
            return out_;
        }
        var prof = UnitImage.StockProfile(codec, program);
        if (prof is not null)
        {
            out_["layout"] = "stock";
            out_["firmware"] = "stock";
            out_["codec_tag"] = prof.Tag;
            out_["stock_profile"] = prof.Name;
            out_["stock_version"] = UnitImage.StockVersion(prof);
            out_["stock_tested"] = UnitImage.TestedStock.Contains(prof.Name);
            out_["install_mode"] = "from_stock";
            out_["supported"] = true;
            return out_;
        }
        if (Any(app, Ddm32Markers.Concat(ClassicMarkers)))
        {
            out_["firmware"] = "classic";
            out_["reason"] = "Classic-type firmware without a valid codec slot (damaged slot?)";
        }
        else
            out_["reason"] = "Unknown firmware: no codec slot and no known stock codec layout (for example stock .050 " +
                             "or newer). Install Classic with the upstream Open25-DM32 Flasher first, then DDM32 Plus " +
                             "over it.";
        return out_;
    }

    /// <summary>radio_detect.py --backup FOLDER: the printed JSON.</summary>
    public static string ToolOutput(string backup, Codec codec) =>
        PyJson.Dumps(Run(UnitImage.SavedImages(backup)["progmem-pass1.bin"], codec)) + "\n";
}
