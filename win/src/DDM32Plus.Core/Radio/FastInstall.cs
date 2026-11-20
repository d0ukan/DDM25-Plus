// SPDX-License-Identifier: GPL-3.0-or-later
namespace DDM32Plus.Core.Radio;

/// <summary>
/// The two faster update paths (owner's decision 2026-11-20, AGENTS.md rule 4), only for an update over an installed
/// DDM32 Plus, never for a first install from stock (its codec slot is prepared from the backup):
///
/// (A) quick update, --quick --backup B: an earlier verified two-pass backup of this radio is reused. Before any write
///     the radio must be the same radio: its chip IDs, its boot area and its codec slot are read live (only those
///     ranges) and must equal the backup's. Every application sector below the slot is rewritten (what the radio
///     holds there now is not known); the boot area, the slot and the data chip are never written.
/// (B) install without backup, --no-backup: no stored backup. The 1 MiB program flash is read once, live, kept in
///     memory only (no file), and the plan and every check are made from it, as install_plus does from a backup.
///
/// Both keep all of install_plus's rules: only application sectors, in plan order, once; the data chip read for its
/// sector CRCs before and after and never written; the whole program read back and compared before the restart; a
/// failed write is never retried. Every run is one user-approved step.
/// </summary>
public static class FastInstallTool
{
    private const int Start = InstallPlan.Start, Size = InstallPlan.Size, Slot = InstallPlan.Slot;

    /// <summary>Over an installed DDM32 Plus only (the detection of a backup or of the live read).</summary>
    public static void RequireDdm32(PyDict detection, string what)
    {
        if (detection["layout"] as string == "stock")
            throw new ValueError($"A first install from stock firmware needs a new backup (its codec slot is prepared from it): {what} holds stock firmware");
        if (detection["layout"] as string != "classic" || detection["firmware"] as string != "ddm32")
            throw new ValueError($"This path is only for an update over an installed DDM32 Plus: {what} holds " +
                                 $"{detection["firmware"]} ({detection["layout"]}); take a new backup and use the normal install");
    }

    /// <summary>(A)'s images: target = backup boot + application + backup slot; the basis marks every application
    /// sector below the slot as changed (inverted), so all of them are rewritten whatever the radio holds there now.</summary>
    public static (byte[] Basis, byte[] Target, long Tag) QuickInputs(string backup, string plus, string? keyHex, Codec codec)
    {
        var program = UnitImage.SavedImages(backup)["progmem-pass1.bin"];
        RequireDdm32(Detect.Run(program, codec), "the backup");
        var tag = InstallPlan.SlotCheck(program);
        var app = InstallPlan.PlusApplication(plus, keyHex);
        byte[] target = [.. program.AsSpan(0, Start), .. app, .. Enumerable.Repeat((byte)0xFF, Slot - Start - app.Length), .. program.AsSpan(Slot)];
        var basis = (byte[])target.Clone();
        for (var i = Start; i < Slot; i++)
            basis[i] ^= 0xFF;
        return (basis, target, tag);
    }

    /// <summary>(A)'s images and the plan it prints (files only; also what the wizard's check step shows).</summary>
    public static (byte[] Basis, byte[] Target, PyDict Plan) QuickPlan(string backup, string plus, string? keyHex, Codec codec)
    {
        var (basis, target, tag) = QuickInputs(backup, plus, keyHex, codec);
        UnitImage.BackupJedec(backup);
        var plan = InstallPlan.MakePlan("install_plus_quick", basis, target, tag);
        plan["backup"] = "reused";
        plan["identity_checked_live"] = new List<object?> { "chip_ids", "boot_area", "codec_slot" };
        return (basis, target, plan);
    }

    /// <summary>--port P --plus R --out NEWDIR (--quick --backup B | --no-backup) [--wait 1..600] [--plan-only]</summary>
    public static void Run(IReadOnlyList<string> argv, ToolContext ctx)
    {
        var args = new Args(argv, ["--port", "--backup", "--plus", "--out", "--wait"], ["--quick", "--no-backup", "--plan-only"],
            ["--port", "--plus", "--out"]);
        var quick = args.Has("--quick");
        var noBackup = args.Has("--no-backup");
        if (quick == noBackup || (quick && args.Get("--backup") is null) || (noBackup && args.Has("--backup")) ||
            (noBackup && args.Has("--plan-only")))
            throw new ToolExit("2");
        var wait = args.Int("--wait", 600);
        if (wait < 1 || wait > 600)
            throw new ToolExit("2");
        var outDir = args.Get("--out")!;
        var plus = args.Get("--plus")!;
        byte[] basis = [], target = [];
        PyDict? plan = null;
        Dictionary<string, string>? expectedChips = null;
        byte[] app = [];
        if (quick)
        {
            var backup = args.Get("--backup")!;
            (basis, target, plan) = QuickPlan(backup, plus, ctx.ReleaseKeyHex, ctx.Codec);
            expectedChips = UnitImage.BackupJedec(backup);
            ctx.Print(PyJson.Dumps(plan));
            if (args.Has("--plan-only"))
                return;
        }
        else
            app = InstallPlan.PlusApplication(plus, ctx.ReleaseKeyHex);
        _ = RamMonitor.Image;
        PyFile.MakeNewDir(outDir);
        var report = new PyDict
        {
            ["mode"] = quick ? "install_plus_quick" : "install_plus_no_backup",
            ["backup"] = quick ? "reused" : "none", ["started_utc"] = ctx.Clock.UtcStamp(),
        };
        if (plan is not null)
            report.Update(plan);
        foreach (var k in new[] { "writes_started", "installation_verified", "bootloader_unchanged", "codec_slot_verified",
                     "data_sector_crcs_unchanged", "restart_sent" })
            report[k] = false;
        var raw = ctx.Serial(args.Get("--port")!);
        var guard = quick ? new InstallPort(raw, basis, target, kind: InstallPort.Authorization.Identity) : new InstallPort(raw);

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
            List<uint> beforeData;
            long? tagLive = null;
            if (quick)
            {
                // the same radio: chip IDs, boot area and codec slot as in the backup (only those ranges are read)
                Steps.CheckChips(radio, guard, expectedChips!);
                var boot = ReadRange(radio, 0, Start);
                var slot = ReadRange(radio, Slot, Size);
                report["live_boot_area_matches_backup"] = boot.AsSpan().SequenceEqual(target.AsSpan(0, Start));
                report["live_codec_slot_matches_backup"] = slot.AsSpan().SequenceEqual(target.AsSpan(Slot));
                Receipt();
                if (!(bool)report["live_boot_area_matches_backup"]! || !(bool)report["live_codec_slot_matches_backup"]!)
                    throw new ValueError("This radio does not match the backup (boot area or codec slot differ): take a new backup");
                ctx.Print("Same radio: chip IDs, boot area and codec slot equal the backup's.");
                beforeData = Steps.DeviceCrcs(radio, "q128", (int)guard.Limits["q128"], "Check settings chip", ctx.Print);
                guard.AuthorizeIdentity(boot, slot);
            }
            else
            {
                foreach (var memory in new[] { "progmem", "q128" })
                {
                    var jedec = radio.Jedec(memory);
                    guard.Limits[memory] = BackupTool.Capacity(jedec, memory);
                    report[$"{memory}_jedec"] = Convert.ToHexStringLower(jedec);
                }
                if (guard.Limits["progmem"] != Size)
                    throw new ValueError("Expected a 1 MiB program flash");
                // the single live read: kept in memory, never saved
                var live = ReadRange(radio, 0, Size, "Read current firmware (once, kept in memory)", ctx.Print);
                report["live_program_sha256"] = Hash.Sha(live);
                var detection = Detect.Run(live, ctx.Codec);
                report["detection"] = detection;
                RequireDdm32(detection, "the radio");
                tagLive = InstallPlan.SlotCheck(live);
                target = [.. live.AsSpan(0, Start), .. app, .. Enumerable.Repeat((byte)0xFF, Slot - Start - app.Length), .. live.AsSpan(Slot)];
                plan = InstallPlan.MakePlan("install_plus_no_backup", live, target, tagLive);
                plan["backup"] = "none";
                plan["live_read_once"] = true;
                ctx.Print(PyJson.Dumps(plan));
                report.Update(plan);
                guard.SetImages(live, target);
                beforeData = Steps.DeviceCrcs(radio, "q128", (int)guard.Limits["q128"], "Check settings chip", ctx.Print);
                Receipt();
                guard.Authorize(live, beforeData, beforeData);
            }
            report["writes_started"] = true;
            Receipt();
            ctx.Print($"INSTALLING: {guard.Todo.Count} application sectors below the codec slot. Boot, slot and data writes blocked.");
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
            Steps.WritePlan(radio, guard, Progress);
            var after = ReadRange(radio, 0, Size, "Read complete installed program chip", ctx.Print);
            var afterData = Steps.DeviceCrcs(radio, "q128", beforeData.Count * 4096, "Recheck untouched settings chip", ctx.Print);
            if (!after.AsSpan(Slot).SequenceEqual(target.AsSpan(Slot)) || InstallPlan.SlotCheck(after) != InstallPlan.SlotCheck(target))
                throw new ValueError("Codec slot changed; restart disabled");
            guard.AuthorizeReset(after, afterData, beforeData);
            foreach (var k in new[] { "installation_verified", "bootloader_unchanged", "codec_slot_verified", "data_sector_crcs_unchanged" })
                report[k] = true;
            report["installed_program_sha256"] = Hash.Sha(after);
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

    /// <summary>[start, end) of the program flash into memory (every sector checked against the radio's CRC).</summary>
    private static byte[] ReadRange(MonitorRadio radio, int start, int end, string? label = null, Action<string>? print = null)
    {
        using var ms = new MemoryStream();
        var last = -1;
        radio.Dump(ms, "progmem", start, end, label is null ? null : (done, total) =>
        {
            var p = (int)((done - start) * 100 / (total - start));
            if (p / 10 != last)
            {
                print!($"{label}: {p}%");
                last = p / 10;
            }
        });
        var data = ms.ToArray();
        if (data.Length != end - start)
            throw new ValueError("Incomplete program readback");
        return data;
    }
}
