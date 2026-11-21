// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections;
using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using DDM32Plus.Core.Codeplug;
using DDM32Plus.Core.Storage;
using DDM32Plus.Localization;
using DDM32Plus.Services;
using CP = DDM32Plus.Core.Codeplug.Codeplug;

namespace DDM32Plus.Views.Pages;

/// <summary>
/// Kanallarım / My channels (pc/ddm32_app/codeplug_tab.py): the codeplug of a backup's data flash image
/// (q128-pass1.bin), read only. The newest verified backup loads by itself; "Başka bir yedek aç..." takes another
/// folder. Channels (FM, DMR, P25 with the NAC as the firmware shows it, NXDN, dPMR, YSF), zones (68, up to 80 channels
/// each), contacts and RX group lists; a search over every column, a zone and a mode filter for the channels, and the
/// rows shown saved as CSV. Nothing is ever written to a radio or a backup.
/// </summary>
public partial class ChannelsPage : UserControl, IPage
{
    private AppState? state;
    private byte[]? image;
    private CP? cp;
    private string? source;
    private bool verified;
    private string? error;
    private bool loading;
    private bool building;
    private bool chosen;                // a backup the user (or --show) chose: the newest one does not replace it

    public ChannelsPage() => InitializeComponent();

    public void Init(AppState state, MainWindow window)
    {
        this.state = state;
        _ = LoadNewestAsync();
    }

    private DataGrid[] Grids => [ChannelGrid, ZoneGrid, ContactGrid, ListGrid];

    private CP.Table CurrentTable => (CP.Table)Math.Max(0, Tabs.SelectedIndex);

    public void Refresh()
    {
        if (state is null)
            return;
        var text = CodeplugText.For(Loc.Instance.Language);
        if (image is not null && cp is not null && !ReferenceEquals(cp.Text, text))
            Decode(text);
        OpenButton.IsEnabled = !loading;
        ExportButton.IsEnabled = cp is not null;
        ShowInfo();
        ShowCount();
    }

    // --- loading ------------------------------------------------------------------------------------------------------
    private List<string> Roots()
    {
        var roots = new List<string> { state!.Paths.Backups };
        roots.AddRange(state.PythonData.Select(d => d.Backups));
        return roots;
    }

    /// <summary>The newest verified backup that has a data flash image loads by itself.</summary>
    private async Task LoadNewestAsync()
    {
        if (state is null)
            return;
        var roots = Roots();
        var newest = await Task.Run(() => AppPaths.VerifiedBackups(roots, "q128-pass1.bin").FirstOrDefault());
        if (newest is not null && cp is null && !chosen)
            await LoadFolderAsync(newest);
    }

    /// <summary>A backup folder (or a 16 MiB image file): read, decode, show. Files only.</summary>
    public async Task LoadFolderAsync(string folder)
    {
        loading = true;
        Refresh();
        try
        {
            var text = CodeplugText.For(Loc.Instance.Language);
            var (img, decoded) = await Task.Run(() =>
            {
                var b = CP.LoadImage(folder);
                return (b, CP.Decode(b, text));
            });
            (image, cp, source, error) = (img, decoded, folder, null);
            verified = Directory.Exists(folder) && AppPaths.IsVerifiedBackup(folder);
            Fill();
        }
        catch (Exception ex) when (ex is CodeplugError or IOException or UnauthorizedAccessException)
        {
            error = Loc.F("Ch_Failed", ex.Message);
        }
        finally
        {
            loading = false;
            Refresh();
        }
    }

    private void Decode(CodeplugText text)
    {
        var zoneSlot = (ZoneBox.SelectedItem as ComboBoxItem)?.Tag is ZoneRow z ? z.Slot : (int?)null;
        var mode = (ModeBox.SelectedItem as ComboBoxItem)?.Tag as string;
        cp = CP.Decode(image!, text);
        Fill(zoneSlot, mode);
    }

    private void OnOpen(object sender, RoutedEventArgs e)
    {
        if (state is null)
            return;
        var dlg = new Microsoft.Win32.OpenFolderDialog { InitialDirectory = state.Paths.Backups, Title = Loc.T("Ch_OpenTip") };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true)
            return;
        chosen = true;
        _ = LoadFolderAsync(dlg.FolderName);
    }

    // --- tables -------------------------------------------------------------------------------------------------------
    private static readonly string[] ModeOrder = ["FM", "DMR", "P25", "NXDN", "dPMR", "YSF"];

    private void Fill(int? zoneSlot = null, string? mode = null)
    {
        if (cp is null)
            return;
        building = true;
        try
        {
            ZoneBox.Items.Clear();
            ZoneBox.Items.Add(new ComboBoxItem { Content = Loc.T("Ch_AllZones") });
            foreach (var z in cp.Zones)
                ZoneBox.Items.Add(new ComboBoxItem { Content = $"{z.Index} · {z.Name} ({z.Count})", Tag = z });
            ZoneBox.SelectedIndex = Math.Max(0, cp.Zones.FindIndex(z => z.Slot == zoneSlot) + 1);
            ModeBox.Items.Clear();
            ModeBox.Items.Add(new ComboBoxItem { Content = Loc.T("Ch_AllModes") });
            var counts = cp.ModeCounts();
            foreach (var m in counts.Keys.OrderBy(m => Array.IndexOf(ModeOrder, m) is var i and >= 0 ? i : 99).ThenBy(m => m, StringComparer.Ordinal))
                ModeBox.Items.Add(new ComboBoxItem { Content = $"{m} ({counts[m]})", Tag = m });
            ModeBox.SelectedIndex = 0;
            for (var i = 1; i < ModeBox.Items.Count; i++)
                if ((ModeBox.Items[i] as ComboBoxItem)?.Tag as string == mode)
                    ModeBox.SelectedIndex = i;
            foreach (CP.Table t in Enum.GetValues<CP.Table>())
                BuildColumns(Grids[(int)t], cp.Columns(t));
            ZoneGrid.ItemsSource = Rows(CP.Table.Zones);
            ContactGrid.ItemsSource = Rows(CP.Table.Contacts);
            ListGrid.ItemsSource = Rows(CP.Table.Lists);
            FillChannels();
            ApplyColumns();
        }
        finally
        {
            building = false;
        }
        ApplyFilter();
    }

    private List<Dictionary<string, string>> Rows(CP.Table t) =>
        [.. cp!.Rows(t).Select(r => new Dictionary<string, string>(r, StringComparer.Ordinal))];

    private void FillChannels()
    {
        if (cp is null)
            return;
        var zone = (ZoneBox.SelectedItem as ComboBoxItem)?.Tag as ZoneRow;
        var rows = zone is null ? cp.Channels : cp.ZoneChannels(zone);
        ChannelGrid.ItemsSource = rows.Select(c => c.Values).ToList();
    }

    private static void BuildColumns(DataGrid grid, (string Key, string Title)[] columns)
    {
        grid.Columns.Clear();
        foreach (var (key, title) in columns)
            grid.Columns.Add(new DataGridTextColumn
            {
                Header = title, Binding = new Binding($"[{key}]"), SortMemberPath = key,
                MaxWidth = key is "channel_names" or "contact_names" ? 900 : 420,
            });
    }

    private void ApplyColumns()
    {
        var all = AllColumnsBox.IsChecked == true;
        for (var i = 0; i < ChannelGrid.Columns.Count && cp is not null; i++)
        {
            var key = cp.Text.ChannelColumns[i].Key;
            var hidden = all ? CP.DerivedKeys.Contains(key) : !CP.SimpleChannelKeys.Contains(key);
            ChannelGrid.Columns[i].Visibility = hidden ? Visibility.Collapsed : Visibility.Visible;
        }
    }

    private void OnColumns(object sender, RoutedEventArgs e) => ApplyColumns();

    // --- search and filters -------------------------------------------------------------------------------------------
    private void ApplyFilter()
    {
        if (cp is null || building)
            return;
        var search = SearchBox.Text;
        var mode = (ModeBox.SelectedItem as ComboBoxItem)?.Tag as string;
        foreach (var g in Grids)
        {
            if (g.ItemsSource is null)
                continue;
            var view = CollectionViewSource.GetDefaultView(g.ItemsSource);
            var isChannels = ReferenceEquals(g, ChannelGrid);
            view.Filter = o => o is IReadOnlyDictionary<string, string> r && CP.Matches(r, search) &&
                               (!isChannels || mode is null || r.TryGetValue("mode", out var m) && m == mode);
        }
        ShowCount();
    }

    private void OnSearch(object sender, TextChangedEventArgs e) => ApplyFilter();

    private void OnFilter(object sender, SelectionChangedEventArgs e)
    {
        if (building || cp is null)
            return;
        if (ReferenceEquals(sender, ZoneBox))
            FillChannels();
        ApplyFilter();
    }

    private void OnTab(object sender, SelectionChangedEventArgs e)
    {
        if (ReferenceEquals(e.OriginalSource, Tabs))
            ShowCount();
    }

    private void OnZoneDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (cp is null || ZoneGrid.SelectedItem is not IReadOnlyDictionary<string, string> r ||
            !int.TryParse(r["index"], NumberStyles.Integer, CultureInfo.InvariantCulture, out var index))
            return;
        ZoneBox.SelectedIndex = index;                      // item 0 is "all channels"
        Tabs.SelectedIndex = 0;
    }

    /// <summary>Numbers sort as numbers ("10" after "9"), texts by the culture, ignoring case.</summary>
    private sealed class RowComparer(string key, ListSortDirection dir) : IComparer
    {
        public int Compare(object? x, object? y)
        {
            var a = (x as IReadOnlyDictionary<string, string>)?.GetValueOrDefault(key) ?? "";
            var b = (y as IReadOnlyDictionary<string, string>)?.GetValueOrDefault(key) ?? "";
            int c;
            if (double.TryParse(a, NumberStyles.Float, CultureInfo.InvariantCulture, out var da) &&
                double.TryParse(b, NumberStyles.Float, CultureInfo.InvariantCulture, out var db))
                c = da.CompareTo(db);
            else
                c = string.Compare(a, b, CultureInfo.CurrentCulture, CompareOptions.IgnoreCase);
            return dir == ListSortDirection.Ascending ? c : -c;
        }
    }

    private void OnSorting(object sender, DataGridSortingEventArgs e)
    {
        e.Handled = true;
        var grid = (DataGrid)sender;
        var dir = e.Column.SortDirection == ListSortDirection.Ascending ? ListSortDirection.Descending : ListSortDirection.Ascending;
        foreach (var c in grid.Columns)
            if (!ReferenceEquals(c, e.Column))
                c.SortDirection = null;
        e.Column.SortDirection = dir;
        if (CollectionViewSource.GetDefaultView(grid.ItemsSource) is ListCollectionView view)
            view.CustomSort = new RowComparer(e.Column.SortMemberPath, dir);
    }

    // --- texts --------------------------------------------------------------------------------------------------------
    private void ShowInfo()
    {
        if (loading && cp is null)
        {
            InfoText.Text = "...";
            RadioText.Text = "";
            return;
        }
        if (cp is null)
        {
            InfoText.Text = error ?? Loc.T("Ch_NoBackup");
            RadioText.Text = "";
            return;
        }
        var zones = cp.Zones.Count > 0 ? Loc.F("Ch_ZoneCount", cp.Zones.Count, cp.ZoneCapacity) : Loc.T("Ch_NoZones");
        var name = source is null ? "" : Path.GetFileName(Path.TrimEndingDirectorySeparator(source));
        InfoText.Text = Loc.F("Ch_Info", cp.Channels.Count, zones, cp.Contacts.Count, cp.Lists.Count, name,
            verified ? "" : Loc.T("Ch_Unverified")) + (error is null ? "" : "\n" + error);
        var modes = string.Join(", ", cp.ModeCounts().OrderBy(kv => Array.IndexOf(ModeOrder, kv.Key) is var i and >= 0 ? i : 99)
            .Select(kv => $"{kv.Key} {kv.Value}"));
        var line = Loc.F("Ch_Radio", cp.RadioName.Length > 0 ? cp.RadioName : "-", cp.DmrId != 0 ? cp.DmrId.ToString(CultureInfo.InvariantCulture) : "-");
        if (modes.Length > 0)
            line += "  ·  " + Loc.F("Ch_Modes", modes);
        if (cp.BadRecords > 0)
            line += "  ·  " + Loc.F("Ch_BadRecords", cp.BadRecords);
        RadioText.Text = line;
    }

    private void ShowCount()
    {
        if (cp is null || Grids[(int)CurrentTable].ItemsSource is not IList all)
        {
            ShownText.Text = "";
            return;
        }
        var view = CollectionViewSource.GetDefaultView(all);
        ShownText.Text = Loc.F("Ch_Shown", view.Cast<object>().Count(), all.Count);
    }

    // --- export -------------------------------------------------------------------------------------------------------
    private void OnExport(object sender, RoutedEventArgs e)
    {
        if (cp is null)
            return;
        var t = CurrentTable;
        var grid = Grids[(int)t];
        var rows = CollectionViewSource.GetDefaultView(grid.ItemsSource).Cast<IReadOnlyDictionary<string, string>>().ToList();
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            FileName = $"ddm32-{t.ToString().ToLowerInvariant()}.csv", Filter = "CSV (*.csv)|*.csv", DefaultExt = ".csv",
        };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true)
            return;
        try
        {
            CP.WriteCsv(dlg.FileName, CP.ToCsv(cp.Columns(t), rows));
            ShownText.Text = Loc.F("Ch_Saved", dlg.FileName);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(Window.GetWindow(this)!, Loc.F("Ch_Failed", ex.Message), Core.AppInfo.Name, MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    /// <summary>--show channels:FOLDER (screenshots, demos): that backup's codeplug, files only.</summary>
    public void ShowBackup(string folder)
    {
        chosen = true;
        _ = LoadFolderAsync(folder);
    }
}
