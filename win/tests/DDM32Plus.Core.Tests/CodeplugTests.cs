// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Text.Json.Nodes;
using DDM32Plus.Core.Codeplug;
using CP = DDM32Plus.Core.Codeplug.Codeplug;

namespace DDM32Plus.Core.Tests;

/// <summary>
/// The codeplug viewer against the Python viewer (pc/ddm32_app/codeplug.py) on synthetic images
/// (Fixtures/codeplug-results.json.gz, make_codeplug_fixtures.py): every row, zone, contact, list, count and CSV equal,
/// with the NAC in the firmware's decimal-first form.
/// </summary>
public class CodeplugTests
{
    private static readonly JsonNode Fixture = Load();

    private static JsonNode Load()
    {
        using var f = File.OpenRead(Path.Combine(AppContext.BaseDirectory, "Fixtures", "codeplug-results.json.gz"));
        using var gz = new GZipStream(f, CompressionMode.Decompress);
        return JsonNode.Parse(gz)!;
    }

    public static TheoryData<string> Images() => [.. Fixture["images"]!.AsObject().Select(kv => kv.Key)];

    internal static byte[] Image(string name)
    {
        var img = new byte[CP.ImageSize];
        Array.Fill(img, (byte)0xFF);
        foreach (var run in Fixture["images"]![name]!["sparse"]!.AsArray())
        {
            var data = Convert.FromBase64String((string)run![1]!);
            data.CopyTo(img, (int)run[0]!);
        }
        return img;
    }

    private static string Text(JsonNode? v) => v switch
    {
        null => "",
        JsonValue x when x.TryGetValue<string>(out var s) => s,
        _ => v.ToJsonString(),
    };

    [Theory]
    [MemberData(nameof(Images))]
    public void DecodesAsThePythonViewer(string name)
    {
        var e = Fixture["images"]![name]!["expected"]!;
        var cp = CP.Decode(Image(name), CodeplugText.Tr);
        var counts = e["counts"]!;
        Assert.Equal((int)counts["channels"]!, cp.Channels.Count);
        Assert.Equal((int)counts["zones"]!, cp.Zones.Count);
        Assert.Equal((int)counts["contacts"]!, cp.Contacts.Count);
        Assert.Equal((int)counts["rx_group_lists"]!, cp.Lists.Count);
        Assert.Equal(e["mode_counts"]!.AsObject().ToDictionary(kv => kv.Key, kv => (int)kv.Value!), cp.ModeCounts());
        Assert.Equal((int)e["bad_records"]!, cp.BadRecords);
        Assert.Equal((int)e["rfkv_entries"]!, cp.RfkvEntries);
        Assert.Equal((int)e["zone_capacity"]!, cp.ZoneCapacity);
        Assert.Equal((string)e["radio_name"]!, cp.RadioName);
        Assert.Equal((long)e["dmr_id"]!, cp.DmrId);

        var rows = e["channels"]!.AsArray();
        for (var i = 0; i < rows.Count; i++)
        {
            var want = rows[i]!.AsObject();
            var got = cp.Channels[i];
            Assert.Equal((int)want["number"]!, got.Number);
            Assert.Equal((string)want["mode"]!, got.Mode);
            foreach (var (key, _) in CodeplugText.Tr.ChannelColumns)
                Assert.True(Text(want[key]) == got[key], $"{name} channel {got.Number} {key}: '{Text(want[key])}' != '{got[key]}'");
        }
        var zones = e["zones"]!.AsArray();
        for (var i = 0; i < zones.Count; i++)
        {
            var z = zones[i]!;
            var g = cp.Zones[i];
            Assert.Equal(((int)z["index"]!, (int)z["slot"]!, (string)z["name"]!, (int)z["count"]!), (g.Index, g.Slot, g.Name, g.Count));
            Assert.Equal(z["channels"]!.AsArray().Select(x => (int)x!), g.Channels);
            Assert.Equal((string)z["channel_names"]!, g.ChannelNames);
        }
        var contacts = e["contacts"]!.AsArray();
        for (var i = 0; i < contacts.Count; i++)
        {
            var c = contacts[i]!;
            Assert.Equal(new ContactRow((int)c["index"]!, (string)c["name"]!, (long)c["id"]!, (string)c["type"]!, (string)c["ts"]!), cp.Contacts[i]);
        }
        var lists = e["lists"]!.AsArray();
        for (var i = 0; i < lists.Count; i++)
        {
            var l = lists[i]!;
            var g = cp.Lists[i];
            Assert.Equal(((int)l["index"]!, (string)l["name"]!, (int)l["count"]!, (string)l["contact_names"]!), (g.Index, g.Name, g.Count, g.ContactNames));
            Assert.Equal(l["contacts"]!.AsArray().Select(x => (int)x!), g.Contacts);
        }
        foreach (var t in Enum.GetValues<CP.Table>())
        {
            var key = t switch { CP.Table.Channels => "channels", CP.Table.Zones => "zones", CP.Table.Contacts => "contacts", _ => "lists" };
            Assert.Equal((string)e["csv"]![key]!, CP.ToCsv(cp.Columns(t), cp.Rows(t)));
        }
    }

    [Fact]
    public void ColumnTitlesAndHelpersEqualPython()
    {
        var cols = Fixture["columns"]!;
        Assert.Equal(cols["channels"]!.AsArray().Select(x => ((string)x![0]!, (string)x[1]!)), CodeplugText.Tr.ChannelColumns);
        Assert.Equal(cols["zones"]!.AsArray().Select(x => ((string)x![0]!, (string)x[1]!)), CodeplugText.Tr.ZoneColumns);
        Assert.Equal(cols["contacts"]!.AsArray().Select(x => ((string)x![0]!, (string)x[1]!)), CodeplugText.Tr.ContactColumns);
        Assert.Equal(cols["lists"]!.AsArray().Select(x => ((string)x![0]!, (string)x[1]!)), CodeplugText.Tr.ListColumns);
        Assert.Equal(CodeplugText.Tr.ChannelColumns.Select(c => c.Key), CodeplugText.En.ChannelColumns.Select(c => c.Key));
        foreach (var x in Fixture["name_check"]!.AsArray())
        {
            var raw = new byte[16];
            Array.Fill(raw, (byte)0xFF);
            var b = System.Text.Encoding.Latin1.GetBytes((string)x![0]!);
            b.AsSpan(0, Math.Min(16, b.Length)).CopyTo(raw);
            Assert.Equal((int)x[1]!, CP.NameCheck(raw));
        }
        foreach (var x in Fixture["tones"]!.AsArray())
            Assert.Equal((string)x![1]!, CP.Tone((int)x[0]!));
    }

    [Fact]
    public void NacInTheFirmwaresDecimalForm()
    {
        Assert.Equal("659 ($293)", CodeplugText.Tr.Nac(0x293));
        Assert.Equal("1 ($001)", CodeplugText.En.Nac(1));
        Assert.Equal("4095 ($FFF)", CodeplugText.En.Nac(0xFFF));
        Assert.Equal("Herhangi 3966 ($F7E)", CodeplugText.Tr.Nac(0xF7E));
        Assert.Equal("Any 3966 ($F7E)", CodeplugText.En.Nac(0xF7E));
    }

    [Fact]
    public void EnglishWords()
    {
        var cp = CP.Decode(Image("sample"), CodeplugText.En);
        var ch = cp.Channels.ToDictionary(c => c.Number);
        Assert.Equal("NAC 659 ($293) · TG 101", ch[4]["summary"]);
        Assert.Equal("Same as RX", ch[4]["tx_nac"]);
        Assert.StartsWith("Any 3966", ch[5]["nac"]);
        Assert.Equal("Yes", ch[5]["rx_only"]);
        Assert.Equal("CC Any · TS Any · all TGs · RX only", ch[3]["summary"]);
        Assert.Equal("Tone RX 67.0 / TX D023I", ch[1]["summary"]);
        Assert.Equal("Default", ch[3]["power"]);
        Assert.Equal("RAN any", ch[7]["code"]);
        Assert.Equal(["Group", "Private", "All call"], cp.Contacts.Select(c => c.Type));
        var p25 = CP.Decode(Image("p25"), CodeplugText.En).Channels.ToDictionary(c => c.Number);
        Assert.Equal("101 (all groups)", p25[13]["tg"]);
        Assert.Equal("contact 3", p25[15]["tg"]);
        Assert.Equal("Unknown (RFKV kind 3)", p25[18].Mode);
        Assert.Equal("Unknown (0x03)", p25[38].Mode);
        Assert.EndsWith("(CH_MODE 2 form)", p25[32]["nac"]);
    }

    [Fact]
    public void FullCodeplugHas68ZonesOf80()
    {
        var cp = CP.Decode(Image("full"));
        Assert.Equal(CP.MaxChannels, cp.Channels.Count);
        Assert.Equal(CP.ZoneSlots, cp.Zones.Count);
        Assert.All(cp.Zones, z => Assert.Equal(80, z.Count));
        Assert.Equal(80, cp.ZoneCapacity);
        var z68 = cp.Zones[^1];
        Assert.Equal("Zone 68", z68.Name);
        var chans = cp.ZoneChannels(z68).ToList();
        Assert.Equal(z68.Channels, chans.Select(c => c.Number));
        Assert.Equal(6, cp.ModeCounts().Count);
    }

    [Fact]
    public void SearchMatchesAnyColumnIgnoringCase()
    {
        var cp = CP.Decode(Image("sample"));
        var rows = cp.Rows(CP.Table.Channels).ToList();
        Assert.Equal(rows.Count, rows.Count(r => CP.Matches(r, "")));
        Assert.Equal(rows.Count, rows.Count(r => CP.Matches(r, "   ")));
        Assert.Equal([4, 5], rows.Where(r => CP.Matches(r, "p25")).Select(r => int.Parse(r["number"], System.Globalization.CultureInfo.InvariantCulture)));
        Assert.Single(rows, r => CP.Matches(r, "659"));
        Assert.Single(rows, r => CP.Matches(r, "$293"));
        Assert.Single(rows, r => CP.Matches(r, "d023i"));
        Assert.Single(rows, r => CP.Matches(r, "430.9"));
        Assert.DoesNotContain(rows, r => CP.Matches(r, "no such text"));
        Assert.Single(cp.Rows(CP.Table.Zones), r => CP.Matches(r, "bank1"));
    }

    [Fact]
    public void CsvQuotesOnlyWhenNeededAndKeepsABom()
    {
        (string, string)[] cols = [("a", "A"), ("b", "B,1")];
        var rows = new[] { new Dictionary<string, string> { ["a"] = "x\"y", ["b"] = "1, 2" }, new() { ["a"] = "plain" } };
        Assert.Equal("A,\"B,1\"\n\"x\"\"y\",\"1, 2\"\nplain,\n", CP.ToCsv(cols, rows));
        var dir = Directory.CreateTempSubdirectory("ddm32-cp-");
        try
        {
            var p = Path.Combine(dir.FullName, "t.csv");
            CP.WriteCsv(p, "No,Ad\n1,Kanal ğüşİ\n");
            var b = File.ReadAllBytes(p);
            Assert.Equal([0xEF, 0xBB, 0xBF], b[..3]);
            Assert.Equal("No,Ad\n1,Kanal ğüşİ\n", System.Text.Encoding.UTF8.GetString(b[3..]));
        }
        finally
        {
            dir.Delete(true);
        }
    }

    [Fact]
    public void WrongSizeIsRefused()
    {
        Assert.Throws<CodeplugError>(() => CP.Decode(new byte[1024]));
        var dir = Directory.CreateTempSubdirectory("ddm32-cp-");
        try
        {
            File.WriteAllBytes(Path.Combine(dir.FullName, "q128-pass1.bin"), new byte[4096]);
            Assert.Throws<CodeplugError>(() => CP.LoadImage(dir.FullName));
            File.WriteAllBytes(Path.Combine(dir.FullName, "q128-pass1.bin"), Image("zone16"));
            Assert.Equal(16, CP.Decode(CP.LoadImage(dir.FullName)).ZoneCapacity);
        }
        finally
        {
            dir.Delete(true);
        }
    }
}
