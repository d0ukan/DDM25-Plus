// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net;
using System.Text;
using DDM32Plus.Core.DataPacks;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.Tests;

/// <summary>Building packs from files, reading a backup's slots, the RadioID download's address rules (a fake
/// network: no request leaves the test).</summary>
public class PackFilesTests
{
    private const string Csv = "RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\n" +
                               "2860105,TA2XYZ,Ayşe,Kaya,Izmir,,Turkey\n2860001,ta1abc,Ahmet,Yilmaz,Ankara,,Turkey\n" +
                               "3109999,W1AW,Hiram,Maxim,Newington,CT,United States\n";

    [Fact]
    public void PacksBuildFromTheUsersFiles()
    {
        using var tmp = new TempDir();
        File.WriteAllText(tmp.Sub("user.csv"), Csv, new UTF8Encoding(false));
        var rid = PackFiles.RadioId(tmp.Sub("user.csv"), [], "RadioID TR", "20261121");
        Assert.Equal(2, rid.Count);
        Assert.Equal("RadioID TR", rid.Header.Name);
        Assert.Equal(DataPack.Tag, rid.Header.Tag);
        Assert.Equal(0u, rid.Header.Seq);
        Assert.Throws<PackError>(() => PackFiles.RadioId(tmp.Sub("user.csv"), ["Atlantis"], "x"));

        File.WriteAllText(tmp.Sub("LTAC.txt"), "LTAC AD 2.18\nTWR 118.100\nGND 121.900\n");
        File.WriteAllText(tmp.Sub("other.txt"), "TWR 119.000\n");
        var src = PackFiles.ReadAirSource(tmp.Sub("LTAC.txt"));
        Assert.Equal("LTAC", src.Code);
        Assert.Equal(2, src.Freqs.Count);
        var air = PackFiles.Air([src], "TR aerodromes", "20261121");
        Assert.Equal(1, air.Count);
        Assert.Throws<PackError>(() => PackFiles.Air([PackFiles.ReadAirSource(tmp.Sub("other.txt"))], "x"));
        Assert.Throws<PackError>(() => PackFiles.Air([], "x"));

        File.WriteAllBytes(tmp.Sub("a.dpk"), air.Image);
        Assert.Equal(air.Image, PackFiles.Load(tmp.Sub("a.dpk")).Image);
        var bad = (byte[])air.Image.Clone();
        bad[^1] ^= 1;
        File.WriteAllBytes(tmp.Sub("b.dpk"), bad);
        Assert.Throws<PackError>(() => PackFiles.Load(tmp.Sub("b.dpk")));
    }

    [Fact]
    public void ABackupShowsItsSlots()
    {
        using var tmp = new TempDir();
        var q = new byte[1 << 24];
        Array.Fill(q, (byte)0xFF);
        var img = DataPack.Image(DataPack.Air, DataPack.AirPayload(new Dictionary<string, List<(int, int)>> { ["LTAC"] = [(4, 118100)] }),
            "20261121", "Air", 2);
        img.CopyTo(q, 0xEF4000);
        File.WriteAllBytes(tmp.Sub("q128-pass1.bin"), q);
        var slots = PackFiles.SlotsFromBackup(tmp.Path, DataPack.Air);
        Assert.Equal(1, DataPack.ActiveSlot(DataPack.Air, slots));
        Assert.Null(DataPack.ActiveSlot(DataPack.RadioId, PackFiles.SlotsFromBackup(tmp.Path, DataPack.RadioId)));
        File.WriteAllBytes(tmp.Sub("q128-pass1.bin"), new byte[100]);
        Assert.Throws<PackError>(() => PackFiles.SlotsFromBackup(tmp.Path, DataPack.Air));
    }

    private sealed class FakeHandler(Func<HttpRequestMessage, HttpResponseMessage> answer) : HttpMessageHandler
    {
        public List<Uri> Asked { get; } = [];

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
        {
            Asked.Add(request.RequestUri!);
            return Task.FromResult(answer(request));
        }
    }

    [Fact]
    public async Task TheRadioIdListComesOnlyFromRadioidNet()
    {
        using var tmp = new TempDir();
        var ok = new FakeHandler(r => r.RequestUri!.AbsolutePath == "/static/user.csv"
            ? new HttpResponseMessage(HttpStatusCode.Found) { Headers = { Location = new Uri("https://www.radioid.net/static/u2.csv") } }
            : new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(Csv) });
        using (var d = new RadioIdDownloader(ok))
            await d.DownloadAsync(tmp.Sub("user.csv"));
        Assert.Equal(Csv, File.ReadAllText(tmp.Sub("user.csv")));
        Assert.Equal(["https://radioid.net/static/user.csv", "https://www.radioid.net/static/u2.csv"], ok.Asked.Select(u => u.AbsoluteUri));

        var evil = new FakeHandler(_ => new HttpResponseMessage(HttpStatusCode.MovedPermanently)
            { Headers = { Location = new Uri("https://example.com/user.csv") } });
        using (var d = new RadioIdDownloader(evil))
            await Assert.ThrowsAsync<NetException>(() => d.DownloadAsync(tmp.Sub("x.csv")));
        Assert.Single(evil.Asked);
        var plain = new FakeHandler(_ => new HttpResponseMessage(HttpStatusCode.Found) { Headers = { Location = new Uri("http://radioid.net/x") } });
        using (var d = new RadioIdDownloader(plain))
            await Assert.ThrowsAsync<NetException>(() => d.DownloadAsync(tmp.Sub("x.csv")));
        Assert.False(File.Exists(tmp.Sub("x.csv")));
        Assert.Throws<NetException>(() => RadioIdDownloader.CheckUrl(new Uri("https://user@radioid.net/static/user.csv")));
    }
}
