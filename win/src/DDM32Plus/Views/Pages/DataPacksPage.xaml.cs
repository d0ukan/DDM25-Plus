// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using DDM32Plus.Core;
using DDM32Plus.Core.DataPacks;
using DDM32Plus.Core.Radio;
using DDM32Plus.Core.Releases;
using DDM32Plus.Localization;
using DDM32Plus.Services;

namespace DDM32Plus.Views.Pages;

/// <summary>
/// Veri paketleri / Data packs (docs/plus.md "User data packs"): what the radio holds (a live read over the CPS
/// protocol, or a verified backup's data chip image), a RadioID pack from the radioid.net list the user downloads, an
/// air band pack from the AIP pages the user downloads, and the write of one pack (the slot not in force, payload
/// first, header last). Each radio step starts only from its own button; the write asks first.
/// </summary>
public partial class DataPacksPage : UserControl, IPage
{
    public const string AipPage = "https://aip.dhmi.gov.tr/";
    private AppState? state;
    private ToolRunner? runner;
    private string? csvPath;
    private readonly List<AirSource> airSources = [];
    private BuiltPack? radioIdPack, airPack;
    private string? radioIdFile, airFile;
    private bool busy, downloading;
    private string radioText = "";
    private string? radioIdError, airError;
    private int sectorsDone, sectorsAll;

    private static readonly System.Windows.Media.FontFamily Mono = new("Consolas");

    public DataPacksPage() => InitializeComponent();

    public bool Busy => busy;

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        runner = new ToolRunner(Dispatcher);
        Countries.Text = state.Settings.GetString("pack_countries") ?? "Turkey";
        RadioIdName.Text = state.Settings.GetString("pack_radioid_name") ?? "RadioID TR";
        AirName.Text = state.Settings.GetString("pack_air_name") ?? "TR aerodromes";
        var csv = Path.Combine(state.Paths.Packs, "user.csv");
        if (File.Exists(csv))
            csvPath = csv;
    }

    private string PacksDir()
    {
        var d = state!.Paths.Packs;
        Directory.CreateDirectory(d);
        return d;
    }

    private static string Kb(long n) => (n / 1024.0).ToString("0.#", Loc.Instance.Culture) + " KB";

    private static string Date(string yyyymmdd) =>
        yyyymmdd.Length == 8 ? $"{yyyymmdd[..4]}-{yyyymmdd[4..6]}-{yyyymmdd[6..]}" : yyyymmdd;

    private static string Describe(BuiltPack p) =>
        Loc.F(p.Type == DataPack.RadioId ? "Pack_DescRadioId" : "Pack_DescAir", p.Header.Name, Date(p.Header.Date), p.Count,
            Kb(p.Image.Length));

    public void Refresh()
    {
        if (state is null)
            return;
        CableText.Text = CableScanner.Text(state.Cable);
        var note = AccessNotes.Current();
        AccessNote.Visibility = note is null ? Visibility.Collapsed : Visibility.Visible;
        AccessTitle.Text = note is null ? "" : Loc.T(note.Value.Title);
        AccessText.Text = note is null ? "" : Loc.T(note.Value.Text);
        var port = state.Port is not null;
        var ok = state.DisclaimerAccepted;
        ReadButton.IsEnabled = !busy && port && ok;
        FromBackupButton.IsEnabled = !busy;
        DownloadButton.IsEnabled = !busy && !downloading;
        BuildRadioIdButton.IsEnabled = !busy && csvPath is not null;
        BuildAirButton.IsEnabled = !busy && airSources.Count > 0;
        WriteRadioIdButton.IsEnabled = !busy && port && ok && radioIdPack is not null;
        WriteAirButton.IsEnabled = !busy && port && ok && airPack is not null;
        LoadButton.IsEnabled = !busy;
        CsvText.Text = csvPath is null ? Loc.T("Pack_NoCsv") : Loc.F("Pack_Csv", csvPath, Kb(SafeLength(csvPath)));
        AirList.Text = airSources.Count == 0 ? Loc.T("Pack_NoPdf")
            : string.Join("\n", airSources.Select(s => Loc.F("Pack_AirSource", s.Code ?? "????", s.Freqs.Count, Path.GetFileName(s.Path),
                string.Join(" ", s.Freqs.Take(6).Select(f => $"{AirText.TypeName(f.Type)} {AirText.Mhz(f.Khz)}")) + (s.Freqs.Count > 6 ? " ..." : ""))));
        RadioIdResult.Text = radioIdError ?? (radioIdPack is null ? "" : Describe(radioIdPack) + (radioIdPack.Skipped is { } sk
            ? "\n" + Loc.F("Pack_Skipped", sk.Callsign, sk.Id, sk.Duplicate) : "") + (radioIdFile is null ? "" : "\n" + radioIdFile));
        AirResult.Text = airError ?? (airPack is null ? "" : Describe(airPack) + (airFile is null ? "" : "\n" + airFile));
        var chosen = new List<string>();
        if (radioIdPack is not null)
            chosen.Add(Describe(radioIdPack));
        if (airPack is not null)
            chosen.Add(Describe(airPack));
        ChosenText.Text = chosen.Count == 0 ? Loc.T("Pack_NothingBuilt") : string.Join("\n", chosen);
        RadioText.Text = radioText.Length == 0 ? Loc.T("Pack_NotRead") : radioText;
        RadioText.FontFamily = radioText.Length == 0 ? FontFamily : Mono;
        AirList.FontFamily = airSources.Count == 0 ? FontFamily : Mono;
    }

    private static long SafeLength(string p)
    {
        try
        {
            return new FileInfo(p).Length;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return 0;
        }
    }

    // --- what the radio holds --------------------------------------------------------------------------------------
    private static string SlotLine(JsonElement s)
    {
        var type = Loc.T(s.GetProperty("type").GetString() == "radioid" ? "Pack_TypeRadioId" : "Pack_TypeAir");
        var slot = s.GetProperty("slot").GetInt64();
        var addr = s.GetProperty("address").GetString();
        if (!s.GetProperty("valid").GetBoolean())
            return Loc.F("Pack_SlotEmpty", type, slot, addr);
        if (!s.GetProperty("crc_ok").GetBoolean())
            return Loc.F("Pack_SlotDamaged", type, slot, addr, s.GetProperty("name").GetString());
        return Loc.F(s.GetProperty("in_force").GetBoolean() ? "Pack_SlotInForce" : "Pack_SlotOld", type, slot, addr,
            s.GetProperty("name").GetString(), Date(s.GetProperty("date").GetString() ?? ""),
            s.GetProperty("count").ValueKind == JsonValueKind.Number ? s.GetProperty("count").GetInt64() : 0,
            Kb(s.GetProperty("bytes").GetInt64() + DataPack.HeaderSize), s.GetProperty("seq").GetInt64());
    }

    private static string SlotLine(DataPackTool.SlotState s) =>
        SlotLine(JsonDocument.Parse(PyJson.Dumps(DataPackTool.SlotJson(s), null)).RootElement);

    private void OnLine(string line)
    {
        LogText.AppendText(line + "\n");
        LogText.ScrollToEnd();
        if (line.StartsWith("{\"slot\"", StringComparison.Ordinal))
        {
            using var doc = JsonDocument.Parse(line);
            radioText += (radioText.Length > 0 ? "\n" : "") + SlotLine(doc.RootElement.GetProperty("slot"));
        }
        else if (line.StartsWith("{\"radio\"", StringComparison.Ordinal))
        {
            using var doc = JsonDocument.Parse(line);
            var r = doc.RootElement.GetProperty("radio");
            radioText = Loc.F("Pack_RadioInfo", r.GetProperty("git").GetString(), r.GetProperty("build").GetString());
        }
        else if (line.StartsWith("{\"plan\"", StringComparison.Ordinal))
        {
            using var doc = JsonDocument.Parse(line);
            var p = doc.RootElement.GetProperty("plan");
            sectorsAll = p.GetProperty("sectors").GetArrayLength();
            Result.Text = Loc.F("Pack_Plan", p.GetProperty("slot").GetInt64(), p.GetProperty("address").GetString(), sectorsAll,
                p.GetProperty("seq").GetInt64());
        }
        else if (line.StartsWith("pack: sector ", StringComparison.Ordinal))
        {
            sectorsDone++;
            Progress.Value = sectorsAll == 0 ? 0 : 100.0 * sectorsDone / sectorsAll;
        }
        Refresh();
    }

    private string LogPath(string what) =>
        Path.Combine(PacksDir(), $"{what}-{DateTime.Now:yyyyMMdd-HHmmss}.log");

    private async void OnRead(object sender, RoutedEventArgs e)
    {
        if (state?.Port is not { } port || runner is null || busy || !state.DisclaimerAccepted)
            return;
        busy = true;
        radioText = "";
        Result.Text = Loc.T("Pack_Reading");
        LogText.Clear();
        Refresh();
        var end = await runner.RunAsync(DataPackTool.Run, ["read", "--port", port], LogPath("datapack-read"), OnLine, writes: false);
        busy = false;
        Result.Text = end.AccessOff ? Loc.T("Radio_Disabled") : end.Error is null ? Loc.T("Pack_ReadDone") : Loc.F("Pack_Failed", end.Error.Record);
        Refresh();
    }

    private void OnFromBackup(object sender, RoutedEventArgs e)
    {
        if (state is null)
            return;
        var dlg = new Microsoft.Win32.OpenFolderDialog { InitialDirectory = state.Paths.Backups, Title = Loc.T("Pack_FromBackup") };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true)
            return;
        try
        {
            if (!Core.Storage.AppPaths.IsVerifiedBackup(dlg.FolderName))
                throw new PackError(Loc.T("Pack_NotVerified"));
            var lines = new List<string> { Loc.F("Pack_FromBackupOf", Path.GetFileName(dlg.FolderName)) };
            foreach (var type in new[] { DataPack.RadioId, DataPack.Air })
                lines.AddRange(DataPackTool.Describe(type, PackFiles.SlotsFromBackup(dlg.FolderName, type)).Select(SlotLine));
            radioText = string.Join("\n", lines);
        }
        catch (Exception ex) when (ex is PackError or IOException or UnauthorizedAccessException)
        {
            radioText = Loc.F("Pack_Failed", ex.Message);
        }
        Refresh();
    }

    // --- RadioID -------------------------------------------------------------------------------------------------------
    private static void OpenBrowser(string url)
    {
        try
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
        }
    }

    private void OnOpenRadioId(object sender, RoutedEventArgs e) => OpenBrowser(RadioIdCsv.DownloadPage);

    private void OnOpenAip(object sender, RoutedEventArgs e) => OpenBrowser(AipPage);

    private async void OnDownload(object sender, RoutedEventArgs e)
    {
        if (state is null || downloading)
            return;
        if (MessageBox.Show(Window.GetWindow(this)!, Loc.F("Pack_DownloadConfirm", RadioIdCsv.DownloadUrl), Loc.T("Pack_Download"),
                MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        downloading = true;
        var dest = Path.Combine(PacksDir(), "user.csv");
        Refresh();
        try
        {
            using var d = new RadioIdDownloader();
            await d.DownloadAsync(dest, n => Dispatcher.BeginInvoke(() => CsvText.Text = Loc.F("Pack_Downloading", Kb(n))));
            csvPath = dest;
        }
        catch (Exception ex) when (ex is NetException or IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(Window.GetWindow(this)!, Loc.F("Pack_Failed", ex.Message), AppInfo.Name, MessageBoxButton.OK, MessageBoxImage.Warning);
        }
        finally
        {
            downloading = false;
            Refresh();
        }
    }

    private void OnChooseCsv(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Filter = "user.csv|*.csv|*.*|*.*" };
        if (dlg.ShowDialog(Window.GetWindow(this)) == true)
            csvPath = dlg.FileName;
        Refresh();
    }

    private async void OnBuildRadioId(object sender, RoutedEventArgs e)
    {
        if (state is null || csvPath is not { } csv || busy)
            return;
        var countries = Countries.Text.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
        var name = RadioIdName.Text.Trim().Length > 0 ? RadioIdName.Text.Trim() : "RadioID";
        state.Settings.Save("pack_countries", Countries.Text.Trim());
        state.Settings.Save("pack_radioid_name", name);
        busy = true;
        Refresh();
        try
        {
            radioIdPack = await Task.Run(() => PackFiles.RadioId(csv, countries, name));
            radioIdFile = Path.Combine(PacksDir(), $"radioid-{radioIdPack.Header.Date}.dpk");
            await File.WriteAllBytesAsync(radioIdFile, radioIdPack.Image);
        }
        catch (Exception ex) when (ex is PackError or IOException or UnauthorizedAccessException)
        {
            radioIdPack = null;
            radioIdFile = null;
            radioIdError = Loc.F("Pack_Failed", ex.Message);
            busy = false;
            Refresh();
            return;
        }
        radioIdError = null;
        busy = false;
        Refresh();
    }

    // --- air band ------------------------------------------------------------------------------------------------------
    private async void OnAddPdf(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Filter = "AIP AD 2 (PDF)|*.pdf|*.txt|*.txt", Multiselect = true };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true || busy)
            return;
        busy = true;
        Refresh();
        var failed = new List<string>();
        foreach (var f in dlg.FileNames)
        {
            try
            {
                var s = await Task.Run(() => PackFiles.ReadAirSource(f));
                airSources.RemoveAll(x => string.Equals(x.Path, f, StringComparison.OrdinalIgnoreCase));
                airSources.Add(s);
            }
            catch (Exception ex) when (ex is PackError or IOException or UnauthorizedAccessException)
            {
                failed.Add(ex.Message);
            }
        }
        busy = false;
        airError = failed.Count > 0 ? string.Join("\n", failed.Select(m => Loc.F("Pack_Failed", m))) : null;
        Refresh();
    }

    private void OnClearPdf(object sender, RoutedEventArgs e)
    {
        airSources.Clear();
        airError = null;
        Refresh();
    }

    private void OnBuildAir(object sender, RoutedEventArgs e)
    {
        if (state is null || airSources.Count == 0 || busy)
            return;
        var name = AirName.Text.Trim().Length > 0 ? AirName.Text.Trim() : "Aerodromes";
        state.Settings.Save("pack_air_name", name);
        try
        {
            airPack = PackFiles.Air(airSources, name);
            airFile = Path.Combine(PacksDir(), $"air-{airPack.Header.Date}.dpk");
            File.WriteAllBytes(airFile, airPack.Image);
        }
        catch (Exception ex) when (ex is PackError or IOException or UnauthorizedAccessException)
        {
            airPack = null;
            airFile = null;
            airError = Loc.F("Pack_Failed", ex.Message);
            Refresh();
            return;
        }
        airError = null;
        Refresh();
    }

    // --- write ---------------------------------------------------------------------------------------------------------
    private void OnLoadDpk(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Filter = "DDM32 Plus (*.dpk)|*.dpk", InitialDirectory = state is null ? null : PacksDir() };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true)
            return;
        try
        {
            var p = PackFiles.Load(dlg.FileName);
            if (p.Type == DataPack.RadioId)
                (radioIdPack, radioIdFile) = (p, dlg.FileName);
            else
                (airPack, airFile) = (p, dlg.FileName);
        }
        catch (Exception ex) when (ex is PackError or IOException or UnauthorizedAccessException)
        {
            Result.Text = Loc.F("Pack_Failed", ex.Message);
            return;
        }
        Refresh();
    }

    private void OnWriteRadioId(object sender, RoutedEventArgs e) => Write(radioIdPack, radioIdFile);

    private void OnWriteAir(object sender, RoutedEventArgs e) => Write(airPack, airFile);

    private async void Write(BuiltPack? pack, string? file)
    {
        if (state?.Port is not { } port || runner is null || busy || pack is null || file is null || !state.DisclaimerAccepted)
            return;
        var slots = $"0x{DataPack.SlotAddress(pack.Type, 0):X} / 0x{DataPack.SlotAddress(pack.Type, 1):X}";
        var sectors = (pack.Image.Length + DataPack.Sector - 1) / DataPack.Sector;
        if (MessageBox.Show(Window.GetWindow(this)!, Loc.F("Pack_Confirm", Describe(pack), port, slots, sectors),
                Loc.T("Pack_ConfirmTitle"), MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes)
            return;
        busy = true;
        sectorsDone = sectorsAll = 0;
        Progress.Value = 0;
        radioText = "";
        LogText.Clear();
        Result.Text = Loc.T("Pack_Writing");
        Refresh();
        var end = await runner.RunAsync(DataPackTool.Run,
            ["write", "--port", port, "--pack", file, "--line", RadioIdCsv.Fold(Loc.T("Pack_ScreenLine"))],
            LogPath("datapack-write"), OnLine, writes: true);
        busy = false;
        if (end.AccessOff)
            Result.Text = Loc.T(RadioAccess.IsOn ? "Radio_WritesDisabled" : "Radio_Disabled");
        else if (end.Error is null)
        {
            Progress.Value = 100;
            Result.Text = Loc.T("Pack_WriteDone");
        }
        else
            Result.Text = Loc.F("Pack_WriteFailed", end.Error.Record);
        Refresh();
    }
}
