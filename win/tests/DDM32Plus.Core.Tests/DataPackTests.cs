// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text.Json;
using DDM32Plus.Core.DataPacks;
using UglyToad.PdfPig.Content;
using UglyToad.PdfPig.Core;
using UglyToad.PdfPig.Fonts.Standard14Fonts;
using UglyToad.PdfPig.Writer;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The C# pack builder against tools/datapack.py (with iddb.py and airfields.py) on synthetic inputs only:
/// Fixtures/datapack-results.json is written by make_datapack_fixtures.py. Every payload, header and slot image must
/// be byte-identical; the same refusals, the same slot choice and sequence, the same CPS order.
/// </summary>
public class DataPackTests
{
    private static readonly Lazy<JsonDocument> Doc = new(() =>
        JsonDocument.Parse(File.ReadAllBytes(TestData.FixturePath("datapack-results.json"))));

    private static JsonElement R => Doc.Value.RootElement;

    private static byte[] B(JsonElement e) => Convert.FromBase64String(e.GetString()!);

    private static void SameBytes(byte[] want, byte[] got, string what)
    {
        Assert.True(want.AsSpan().SequenceEqual(got), $"{what}: {want.Length} bytes wanted, {got.Length} built; first difference at " +
            $"{Enumerable.Range(0, Math.Min(want.Length, got.Length)).FirstOrDefault(i => want[i] != got[i], Math.Min(want.Length, got.Length))}");
    }

    [Fact]
    public void RadioIdPacksEqualDatapackPy()
    {
        var n = 0;
        foreach (var c in R.GetProperty("radioid").EnumerateArray())
        {
            var name = c.GetProperty("name").GetString()!;
            var csv = B(c.GetProperty("csv"));
            var countries = c.GetProperty("countries").EnumerateArray().Select(x => x.GetString()!).ToList();
            if (c.TryGetProperty("error", out var err))
            {
                var e = Assert.Throws<PackError>(() => RadioIdCsv.Read(csv, countries));
                Assert.EndsWith(err.GetString()!.Replace("user.csv: ", "", StringComparison.Ordinal), e.Message);
                continue;
            }
            var (rows, skipped) = RadioIdCsv.Read(csv, countries);
            var want = c.GetProperty("rows").EnumerateArray().Select(r => (r[0].GetInt32(), r[1].GetString(), r[2].GetInt64(), r[3].GetString())).ToList();
            Assert.Equal(want, rows.Select(r => (r.Id, (string?)r.Call, r.Packed, (string?)r.Name)).ToList());
            var sk = c.GetProperty("skipped");
            Assert.Equal((sk.GetProperty("callsign").GetInt32(), sk.GetProperty("id").GetInt32(), sk.GetProperty("duplicate").GetInt32()),
                (skipped.Callsign, skipped.Id, skipped.Duplicate));
            var (payload, count, _) = DataPack.RadioIdFromCsv(csv, countries);
            Assert.Equal(c.GetProperty("count").GetInt32(), count);
            SameBytes(B(c.GetProperty("payload")), payload, name + " payload");
            SameBytes(B(c.GetProperty("image")),
                DataPack.Image(DataPack.RadioId, payload, "20261121", "RadioID TR", 0, DataPack.PythonTag), name + " image");
            SameBytes(B(c.GetProperty("image_cs")), DataPack.Image(DataPack.RadioId, payload, "20261121", "RadioID TR", 7),
                name + " image with this program's tag");
            n++;
        }
        Assert.Equal(6, n);
    }

    [Fact]
    public void TextRulesEqualIddb()
    {
        var t = R.GetProperty("text");
        foreach (var x in t.GetProperty("fold").EnumerateArray())
            Assert.Equal(x[1].GetString(), RadioIdCsv.Fold(x[0].GetString()!));
        foreach (var x in t.GetProperty("country").EnumerateArray())
            Assert.Equal(x[1].GetString(), RadioIdCsv.CountryKey(x[0].GetString()!));
        foreach (var x in t.GetProperty("short_name").EnumerateArray())
            Assert.Equal(x[2].GetString(), RadioIdCsv.ShortName(x[0].GetString()!, x[1].GetString()!));
        foreach (var x in t.GetProperty("pack_call").EnumerateArray())
            Assert.Equal(x[1].ValueKind == JsonValueKind.Null ? null : x[1].GetInt64(), RadioIdCsv.PackCall(x[0].GetString()!));
    }

    [Fact]
    public void RadioIdPayloadWidthsAndRefusals()
    {
        var p = R.GetProperty("payloads");
        var entries = p.GetProperty("entries").EnumerateArray()
            .Select(e => (e[0].GetInt32(), e[1].GetString()!, e[2].GetString()!)).ToList();
        foreach (var w in p.GetProperty("widths").EnumerateArray())
        {
            var chars = w.GetProperty("chars").GetInt32();
            if (w.TryGetProperty("error", out var err))
                Assert.Equal(err.GetString(), Assert.Throws<PackError>(() => DataPack.RadioIdPayload(entries, chars)).Message);
            else
                SameBytes(B(w.GetProperty("payload")), DataPack.RadioIdPayload(entries, chars), $"width {chars}");
        }
        var refused = p.GetProperty("refused").EnumerateArray().Select(x => x.GetString()).ToList();
        Assert.Equal(refused[0], Assert.Throws<PackError>(() => DataPack.RadioIdPayload([(1, "A", ""), (1, "B", "")])).Message);
        Assert.Equal(refused[1], Assert.Throws<PackError>(() => DataPack.RadioIdPayload([(0, "A", "")])).Message);
        Assert.Equal(refused[2], Assert.Throws<PackError>(() => DataPack.RadioIdPayload([(16777216, "A", "")])).Message);
    }

    [Fact]
    public void AirPacksEqualDatapackPy()
    {
        var a = R.GetProperty("air");
        foreach (var t in a.GetProperty("texts").EnumerateObject())
        {
            var want = a.GetProperty("parsed").GetProperty(t.Name).EnumerateArray().Select(x => (x[0].GetInt32(), x[1].GetInt32())).ToList();
            Assert.Equal(want, AirText.ParseAd218(t.Value.GetString()!));
        }
        foreach (var c in a.GetProperty("channel").EnumerateArray())
        {
            var (khz, why) = AirText.ChannelKhz(c[0].GetString()!);
            Assert.Equal(c[1].ValueKind == JsonValueKind.Null ? null : c[1].GetInt32(), khz);
            Assert.Equal(c[2].ValueKind == JsonValueKind.Null ? null : c[2].GetString(), why);
        }
        var fields = a.GetProperty("fields").EnumerateObject().ToDictionary(f => f.Name,
            f => f.Value.EnumerateArray().Select(x => (x[0].GetInt32(), x[1].GetInt32())).ToList());
        var payload = DataPack.AirPayload(fields);
        SameBytes(B(a.GetProperty("payload")), payload, "air payload");
        SameBytes(B(a.GetProperty("image")), DataPack.Image(DataPack.Air, payload, "20261121", "TR aerodromes", 0, DataPack.PythonTag), "air image");
        SameBytes(B(a.GetProperty("image_cs")), DataPack.Image(DataPack.Air, payload, "20261121", "TR aerodromes", 3), "air image, this tag");
        var refused = a.GetProperty("refused").EnumerateArray().Select(x => x.GetString()).ToList();
        Assert.Equal(refused[0], Assert.Throws<PackError>(() => DataPack.AirPayload(new Dictionary<string, List<(int, int)>> { [""] = [(0, 118000)] })).Message);
        Assert.Equal(refused[1], Assert.Throws<PackError>(() => DataPack.AirPayload(new Dictionary<string, List<(int, int)>> { ["TOOLONGXX"] = [(0, 118000)] })).Message);
    }

    [Fact]
    public void HeadersEqualDatapackPy()
    {
        foreach (var h in R.GetProperty("headers").EnumerateArray())
        {
            var type = h.GetProperty("type").GetInt32();
            var payload = B(h.GetProperty("payload"));
            var date = h.GetProperty("date").GetString()!;
            var name = h.GetProperty("name").GetString()!;
            var seq = h.GetProperty("seq").GetUInt32();
            var tag = h.GetProperty("tag").GetString()!;
            if (h.TryGetProperty("error", out var err))
            {
                var e = Assert.Throws<PackError>(() => DataPack.Header(type, payload, date, name, seq, tag));
                Assert.Equal(err.GetString()!.Replace("'", "", StringComparison.Ordinal), e.Message.Replace("'", "", StringComparison.Ordinal));
            }
            else
                SameBytes(B(h.GetProperty("header")), DataPack.Header(type, payload, date, name, seq, tag), name);
        }
    }

    [Fact]
    public void ParseAndInfoEqualDatapackPy()
    {
        foreach (var c in R.GetProperty("parse").EnumerateObject())
        {
            var img = B(c.Value.GetProperty("image"));
            var (h, payload) = DataPack.Parse(img);
            var want = c.Value.GetProperty("header");
            Assert.Equal(c.Value.GetProperty("info").GetString(), DataPack.Info(img));
            if (want.ValueKind == JsonValueKind.Null)
            {
                Assert.Null(h);
                Assert.Empty(payload);
                continue;
            }
            Assert.NotNull(h);
            Assert.Equal(want.GetProperty("type").GetInt32(), h.Type);
            Assert.Equal(want.GetProperty("fmt").GetInt32(), h.Format);
            Assert.Equal(want.GetProperty("seq").GetUInt32(), h.Seq);
            Assert.Equal(want.GetProperty("len").GetUInt32(), h.Length);
            Assert.Equal(want.GetProperty("crc").GetUInt32(), h.Crc);
            Assert.Equal(want.GetProperty("date").GetString(), h.Date);
            Assert.Equal(want.GetProperty("name").GetString(), h.Name);
            Assert.Equal(want.GetProperty("tag").GetString(), h.Tag);
            Assert.Equal(want.GetProperty("crc_ok").GetBoolean(), h.CrcOk);
            SameBytes(B(c.Value.GetProperty("payload")), payload, c.Name);
        }
    }

    [Fact]
    public void SlotChoiceAndSequenceEqualDatapackPy()
    {
        foreach (var s in R.GetProperty("slots").EnumerateArray())
        {
            var imgs = s.GetProperty("images").EnumerateArray().Select(B).ToList();
            Assert.Equal(s.GetProperty("inactive_radioid").GetInt32(), DataPack.InactiveSlot(DataPack.RadioId, imgs));
            Assert.Equal(s.GetProperty("inactive_air").GetInt32(), DataPack.InactiveSlot(DataPack.Air, imgs));
            Assert.Equal(s.GetProperty("next_seq").GetUInt32(), DataPack.NextSeq(imgs));
        }
    }

    [Fact]
    public void CpsPlanEqualsDatapackPy()
    {
        foreach (var p in R.GetProperty("plans").EnumerateArray())
        {
            var plan = DataPack.CpsPlan(p.GetProperty("address").GetInt64(), B(p.GetProperty("image")));
            var want = p.GetProperty("plan").EnumerateArray().ToList();
            Assert.Equal(want.Count, plan.Count);
            for (var i = 0; i < want.Count; i++)
            {
                Assert.Equal(want[i][0].GetInt64(), plan[i].Address);
                SameBytes(B(want[i][1]), plan[i].Data, $"sector {i}");
            }
        }
    }

    [Fact]
    public void TheRegionGuardRefusesEverySectorOutside()
    {
        // 0xE70000-0xEFFFFF only: one byte below, at the end, a slot image that would run past the region
        Assert.True(DataPack.InRegion(0xE70000, 4096));
        Assert.True(DataPack.InRegion(0xEFF000, 4096));
        Assert.False(DataPack.InRegion(0xE6F000, 4096));
        Assert.False(DataPack.InRegion(0xE6FFFF, 2));
        Assert.False(DataPack.InRegion(0xF00000, 1));
        Assert.False(DataPack.InRegion(0xEFF001, 4096));
        Assert.False(DataPack.InRegion(0xE70000, 0));
        Assert.False(DataPack.InRegion(0x720000, 4096));             // the codeplug
        Assert.Throws<PackError>(() => DataPack.CpsPlan(0xE6F000, new byte[100]));
        Assert.Throws<PackError>(() => DataPack.CpsPlan(0xEFF000, new byte[4097]));
        Assert.Throws<PackError>(() => DataPack.CpsPlan(0xF00000, new byte[1]));
        Assert.Throws<PackError>(() => DataPack.CpsPlan(0x000000, new byte[1]));
        // a pack bigger than its slot never reaches a plan (it would run into the other slot)
        Assert.Throws<PackError>(() => DataPack.PlanForSlot(DataPack.Air, 1, new byte[0x4001]));
        Assert.Throws<PackError>(() => DataPack.SlotAddress(DataPack.Air, 2));
        Assert.Throws<PackError>(() => DataPack.SlotAddress(3, 0));
        var full = DataPack.PlanForSlot(DataPack.Air, 1, new byte[0x4000]);
        Assert.Equal([0xEF5000L, 0xEF6000, 0xEF7000, 0xEF4000], full.Select(s => s.Address));
        var id = DataPack.PlanForSlot(DataPack.RadioId, 1, new byte[0x40000]);
        Assert.Equal(64, id.Count);
        Assert.All(id, s => Assert.True(s.Address is >= 0xEB0000 and < 0xEF0000));
    }

    [Fact]
    public void AerodromeCodesComeFromTheFileNameOrTheText()
    {
        Assert.Equal("LTAC", AirText.CodeOf("LT_AD_2_LTAC_en.pdf"));
        Assert.Equal("LTBA", AirText.CodeOf(@"C:\x\LTBA.pdf"));
        Assert.Equal("LTFM", AirText.CodeOf("LT-AD-2-LTFM-en.pdf"));
        Assert.Equal("LTAD", AirText.CodeOf("LTAD AD 2.pdf"));
        Assert.Equal("LTAC", AirText.CodeOf("page.pdf", "LTAC AD 2.17 x\nLTAC AD 2.18 ATS COMMUNICATION FACILITIES\nLTBA 118.1"));
        Assert.Null(AirText.CodeOf("page.pdf", "no code here"));
    }

    [Fact]
    public void ASyntheticAipPdfGivesItsTableRows()
    {
        // a made-up AD 2.18 page: table cells side by side on one row, a frequency alone on the next row
        var b = new PdfDocumentBuilder();
        var font = b.AddStandard14Font(Standard14Font.Helvetica);
        var page = b.AddPage(PageSize.A4);
        void Row(double y, params (double X, string Text)[] cells)
        {
            foreach (var (x, t) in cells)
                page.AddText(t, 9, new PdfPoint(x, y), font);
        }
        Row(800, (40, "LTXX AD 2.17 AIR TRAFFIC SERVICES AIRSPACE"));
        Row(780, (40, "LTXX AD 2.18 ATS COMMUNICATION FACILITIES"));
        Row(760, (40, "Service designation"), (160, "Call sign"), (300, "Frequency"), (400, "Hours"));
        Row(740, (40, "ATIS"), (160, "TESTFIELD INFORMATION"), (300, "123.600 MHZ"), (400, "H24"));
        Row(720, (40, "TWR"), (160, "TESTFIELD TOWER"), (300, "118.100 MHZ"), (400, "H24"));
        Row(708, (300, "121.500 MHZ"), (400, "EMERG"));
        Row(690, (40, "GND"), (160, "TESTFIELD GROUND"), (300, "121.900 MHZ"));
        Row(670, (40, "LTXX AD 2.19 RADIO NAVIGATION AND LANDING AIDS"));
        Row(650, (40, "VOR"), (300, "112.500 MHZ"));
        using var tmp = new TempDir();
        var path = tmp.Sub("LT_AD_2_LTXX_en.pdf");
        File.WriteAllBytes(path, b.Build());
        Assert.Equal(1, PdfText.Pages(path));
        var text = PdfText.Read(path);
        int T(string n) => Array.IndexOf(AirText.Types, n);
        Assert.Equal([(T("ATIS"), 123600), (T("TWR"), 118100), (T("EMR"), 121500), (T("GND"), 121900)], AirText.ParseAd218(text));
        Assert.Equal("LTXX", AirText.CodeOf(path, text));
        var bad = tmp.Sub("bad.pdf");
        File.WriteAllText(bad, "not a pdf");
        Assert.Throws<PackError>(() => PdfText.Read(bad));
        Assert.Equal(0, PdfText.Pages(bad));
    }
}
