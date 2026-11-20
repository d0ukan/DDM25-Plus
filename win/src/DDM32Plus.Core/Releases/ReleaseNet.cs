// SPDX-License-Identifier: GPL-3.0-or-later
using System.Net;
using System.Net.Http.Headers;

namespace DDM32Plus.Core.Releases;

/// <summary>A network failure or a refused address (releases.NetError).</summary>
public sealed class NetException(string message) : Exception(message);

/// <summary>Network access for the release list and downloads; tests pass a fake.</summary>
public interface IReleaseNet
{
    /// <summary>The body of url, at most limit bytes.</summary>
    Task<byte[]> FetchAsync(string url, int limit, CancellationToken ct = default);

    /// <summary>url into dest (via dest.partial), refusing more than size bytes; progress(done, size).</summary>
    Task DownloadAsync(string url, string dest, long size, Action<long, long>? progress, CancellationToken ct = default);
}

/// <summary>
/// GitHub over HTTPS only, with every redirect checked against the allowed hosts. TLS is Windows' own (SChannel):
/// it trusts the Windows certificate store and lets Windows fetch a missing root, so no extra CA bundle is needed
/// (the Python program needed certifi for that, pc/ddm32_app/tls.py). TLS is not what makes a release trusted: every
/// file is checked against the Ed25519-signed manifest before it is used.
/// </summary>
public sealed class HttpReleaseNet : IReleaseNet, IDisposable
{
    private const int MaxRedirects = 10;
    private readonly HttpClient client;

    public HttpReleaseNet(HttpMessageHandler? handler = null, TimeSpan? timeout = null)
    {
        handler ??= new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            AutomaticDecompression = DecompressionMethods.None,
            ConnectTimeout = TimeSpan.FromSeconds(30),
        };
        client = new HttpClient(handler, disposeHandler: true) { Timeout = timeout ?? TimeSpan.FromSeconds(60) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue(AppInfo.ExeName, AppInfo.Version));
    }

    public void Dispose() => client.Dispose();

    /// <summary>Refuses anything but https:// on the GitHub hosts (releases._check_url).</summary>
    public static void CheckUrl(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u) || u.Scheme != Uri.UriSchemeHttps ||
            !ReleaseConstants.AllowedHosts.Contains(u.IdnHost.ToLowerInvariant()) || !string.IsNullOrEmpty(u.UserInfo))
        {
            var shown = u is null ? url : $"{u.Scheme}://{u.Host}";
            throw new NetException($"Refusing a download outside GitHub over HTTPS: {shown}");
        }
    }

    private async Task<HttpResponseMessage> OpenAsync(string url, CancellationToken ct)
    {
        CheckUrl(url);
        var current = new Uri(url);
        for (var hop = 0; hop <= MaxRedirects; hop++)
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, current);
            req.Headers.Accept.ParseAdd(current.Host == "api.github.com"
                ? "application/vnd.github+json" : "application/octet-stream, */*");
            HttpResponseMessage resp;
            try
            {
                resp = await client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException)
            {
                throw new NetException($"{e.GetType().Name}: {e.Message}");
            }
            var code = (int)resp.StatusCode;
            if (code is >= 300 and < 400 && resp.Headers.Location is { } loc)
            {
                resp.Dispose();
                var next = loc.IsAbsoluteUri ? loc : new Uri(current, loc);
                CheckUrl(next.AbsoluteUri);
                current = next;
                continue;
            }
            if (!resp.IsSuccessStatusCode)
            {
                resp.Dispose();
                throw new NetException($"HTTP Error {code}: {resp.ReasonPhrase}");
            }
            return resp;
        }
        throw new NetException("Too many redirects");
    }

    public async Task<byte[]> FetchAsync(string url, int limit, CancellationToken ct = default)
    {
        using var resp = await OpenAsync(url, ct).ConfigureAwait(false);
        await using var s = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
        var buf = new MemoryStream();
        var block = new byte[1 << 16];
        int n;
        while ((n = await s.ReadAsync(block, ct).ConfigureAwait(false)) > 0)
        {
            buf.Write(block, 0, n);
            if (buf.Length > limit)
                throw new NetException($"Answer larger than {limit} bytes: {url}");
        }
        return buf.ToArray();
    }

    public async Task DownloadAsync(string url, string dest, long size, Action<long, long>? progress,
        CancellationToken ct = default)
    {
        var part = dest + ".partial";
        using var resp = await OpenAsync(url, ct).ConfigureAwait(false);
        await using (var s = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false))
        await using (var outp = File.Create(part))
        {
            var block = new byte[1 << 16];
            long done = 0;
            int n;
            while ((n = await s.ReadAsync(block, ct).ConfigureAwait(false)) > 0)
            {
                done += n;
                if (done > size)
                {
                    outp.Close();
                    File.Delete(part);
                    throw new NetException("Download larger than the signed size");
                }
                await outp.WriteAsync(block.AsMemory(0, n), ct).ConfigureAwait(false);
                progress?.Invoke(done, size);
            }
        }
        File.Move(part, dest, overwrite: true);
    }
}
