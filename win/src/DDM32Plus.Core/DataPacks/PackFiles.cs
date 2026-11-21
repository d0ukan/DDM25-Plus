// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net;
using System.Net.Http.Headers;
using DDM32Plus.Core.Releases;

namespace DDM32Plus.Core.DataPacks;

/// <summary>An air band pack's source: one aerodrome's page(s), its code and the frequencies found.</summary>
public sealed record AirSource(string Path, string? Code, List<(int Type, int Khz)> Freqs);

/// <summary>A built pack: the slot image (sequence 0, as datapack.py writes .dpk files) and what it holds.</summary>
public sealed record BuiltPack(int Type, byte[] Image, PackHeader Header, int Count, RadioIdSkipped? Skipped);

/// <summary>Building packs from the user's files, reading what a verified backup holds (files only, no port).</summary>
public static class PackFiles
{
    public static string Today() => DateTime.Now.ToString("yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>A RadioID pack from RadioID.net's user.csv the user downloaded.</summary>
    public static BuiltPack RadioId(string csvPath, IEnumerable<string> countries, string name, string? date = null)
    {
        var (payload, count, skipped) = DataPack.RadioIdFromCsv(File.ReadAllBytes(csvPath), countries);
        if (count == 0)
            throw new PackError("no ID of the chosen countries in the list");
        var image = DataPack.Image(DataPack.RadioId, payload, date ?? Today(), name);
        return new BuiltPack(DataPack.RadioId, image, DataPack.Parse(image).Header!, count, skipped);
    }

    /// <summary>One AIP PDF (or a .txt of its text): its aerodrome code and the AD 2.18 frequencies.</summary>
    public static AirSource ReadAirSource(string path)
    {
        var text = path.EndsWith(".txt", StringComparison.OrdinalIgnoreCase)
            ? File.ReadAllText(path) : PdfText.Read(path);
        return new AirSource(path, AirText.CodeOf(path, text), AirText.ParseAd218(text));
    }

    /// <summary>An air band pack from the sources (later pages of the same code replace earlier ones, as datapack.py's
    /// dictionary does).</summary>
    public static BuiltPack Air(IEnumerable<AirSource> sources, string name, string? date = null)
    {
        var fields = new Dictionary<string, List<(int Type, int Khz)>>(StringComparer.Ordinal);
        foreach (var s in sources)
        {
            if (s.Code is null)
                throw new PackError($"{Path.GetFileName(s.Path)}: no aerodrome code (LTxx)");
            fields[s.Code] = s.Freqs;
        }
        if (fields.Count == 0)
            throw new PackError("no aerodrome page given");
        var payload = DataPack.AirPayload(fields);
        var image = DataPack.Image(DataPack.Air, payload, date ?? Today(), name);
        return new BuiltPack(DataPack.Air, image, DataPack.Parse(image).Header!, fields.Count, null);
    }

    /// <summary>A .dpk file (this program's or datapack.py's): whole and checked, or PackError.</summary>
    public static BuiltPack Load(string path)
    {
        var image = File.ReadAllBytes(path);
        var (h, payload) = DataPack.Parse(image);
        if (h is not { CrcOk: true } || h.Format != 1 || h.Type is not (DataPack.RadioId or DataPack.Air) ||
            image.Length != DataPack.HeaderSize + h.Length || image.Length > DataPack.SlotSize(h.Type))
            throw new PackError($"{Path.GetFileName(path)}: not a whole data pack");
        return new BuiltPack(h.Type, image, h, DataPack.Count(h, payload) ?? 0, null);
    }

    /// <summary>Both slots of a type as a verified backup's data chip image holds them (q128-pass1.bin, files only).</summary>
    public static byte[][] SlotsFromBackup(string folder, int type)
    {
        var path = Path.Combine(folder, "q128-pass1.bin");
        using var f = File.OpenRead(path);
        if (f.Length < DataPack.RegionEnd)
            throw new PackError("q128-pass1.bin: too short for the data pack region");
        var out_ = new byte[2][];
        for (var slot = 0; slot < 2; slot++)
        {
            var buf = new byte[DataPack.SlotSize(type)];
            f.Seek(DataPack.SlotAddress(type, slot), SeekOrigin.Begin);
            f.ReadExactly(buf);
            out_[slot] = buf;
        }
        return out_;
    }
}

/// <summary>
/// RadioID.net's user list, downloaded only on the user's click: https://radioid.net only (every redirect checked
/// again), at most 200 MB, into the file the user's program folder names. The list is the user's own copy; this
/// program never redistributes it.
/// </summary>
public sealed class RadioIdDownloader : IDisposable
{
    public static readonly IReadOnlySet<string> AllowedHosts = new HashSet<string> { "radioid.net", "www.radioid.net" };
    public const long Limit = 200L << 20;
    private readonly HttpClient client;

    public RadioIdDownloader(HttpMessageHandler? handler = null)
    {
        handler ??= new SocketsHttpHandler { AllowAutoRedirect = false, ConnectTimeout = TimeSpan.FromSeconds(30) };
        client = new HttpClient(handler, disposeHandler: true) { Timeout = TimeSpan.FromMinutes(10) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue(AppInfo.ExeName, AppInfo.Version));
    }

    public void Dispose() => client.Dispose();

    public static void CheckUrl(Uri u)
    {
        if (u.Scheme != Uri.UriSchemeHttps || !AllowedHosts.Contains(u.IdnHost.ToLowerInvariant()) || !string.IsNullOrEmpty(u.UserInfo))
            throw new NetException($"Refusing a download outside radioid.net over HTTPS: {u.Scheme}://{u.Host}");
    }

    public async Task DownloadAsync(string dest, Action<long>? progress = null, CancellationToken ct = default)
    {
        var current = new Uri(RadioIdCsv.DownloadUrl);
        for (var hop = 0; hop <= 5; hop++)
        {
            CheckUrl(current);
            using var req = new HttpRequestMessage(HttpMethod.Get, current);
            HttpResponseMessage resp;
            try
            {
                resp = await client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException)
            {
                throw new NetException($"{e.GetType().Name}: {e.Message}");
            }
            using (resp)
            {
                var code = (int)resp.StatusCode;
                if (code is >= 300 and < 400 && resp.Headers.Location is { } loc)
                {
                    current = loc.IsAbsoluteUri ? loc : new Uri(current, loc);
                    continue;
                }
                if (resp.StatusCode != HttpStatusCode.OK)
                    throw new NetException($"HTTP Error {code}: {resp.ReasonPhrase}");
                var part = dest + ".partial";
                await using (var s = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false))
                await using (var outp = File.Create(part))
                {
                    var block = new byte[1 << 16];
                    long done = 0;
                    int n;
                    while ((n = await s.ReadAsync(block, ct).ConfigureAwait(false)) > 0)
                    {
                        done += n;
                        if (done > Limit)
                        {
                            outp.Close();
                            File.Delete(part);
                            throw new NetException("The list is larger than 200 MB");
                        }
                        await outp.WriteAsync(block.AsMemory(0, n), ct).ConfigureAwait(false);
                        progress?.Invoke(done);
                    }
                }
                File.Move(part, dest, overwrite: true);
                return;
            }
        }
        throw new NetException("Too many redirects");
    }
}
