// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The guards (the Python guards' frame table and each rule on its own), the fake radio against the Python one, the
//! quick update and the install without backup, the flows, the progress lines and the cable check (the C#
//! RadioGuardTests, FastInstallTests, FlowTests, ProgressTests). No serial port exists here.
mod common;
use common::*;
use ddm32_core::pyjson::{self, Py, PyDict};
use ddm32_core::radio::cable::{evaluate, CableState, PnpDevice};
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::fast_install::{fast_install_tool, quick_inputs};
use ddm32_core::radio::flow::{BackupPath, FileSteps, Installable, RestoreFlow, StepResult, StepState, Wizard};
use ddm32_core::radio::guards::{Authorization, InstallPort, ReadOnlyPort};
use ddm32_core::radio::monitor::{be32, be32_bytes, le32_bytes, ram_monitor as rm, sha};
use ddm32_core::radio::plan::crc_list;
use ddm32_core::radio::port::{no_port, RadioAccess, RawSerial, VirtualClock};
use ddm32_core::radio::progress::ProgressTracker;
use ddm32_core::radio::tools::{backup_tool, install_tool, restore_stock_tool, ToolContext};
use ddm32_core::radio::{ToolError, ToolResult};
use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::OnceLock;

fn null() -> Box<dyn RawSerial> {
    Box::new(NullPort::default())
}

fn verdict_ro(g: &mut ReadOnlyPort, f: &[u8]) -> String {
    match g.validate(f) {
        Ok(k) => k.to_string(),
        Err(e) => format!("refused: {}", e.message),
    }
}

fn verdict(g: &mut InstallPort, f: &[u8]) -> String {
    match g.validate(f) {
        Ok(k) => k.to_string(),
        Err(e) => format!("refused: {}", e.message),
    }
}

fn guards() -> &'static serde_json::Value {
    &index()["guards"]
}

#[test]
fn read_only_guard_before_the_monitor_runs() {
    let mut g = ReadOnlyPort::new(null()).unwrap();
    for c in guards()["readonly_before_monitor"].as_array().unwrap() {
        assert_eq!(
            c["verdict"].as_str().unwrap(),
            verdict_ro(&mut g, &hex::decode(c["frame"].as_str().unwrap()).unwrap()),
            "{}",
            c["name"]
        );
    }
}

#[test]
fn read_only_guard_answers_every_frame_as_python() {
    let cases = guards()["readonly"].as_array().unwrap();
    for c in cases {
        let mut g = ReadOnlyPort::new(null()).unwrap();
        g.monitor = true;
        g.limits.insert("progmem", c["limits"]["progmem"].as_u64().unwrap());
        g.limits.insert("q128", c["limits"]["q128"].as_u64().unwrap());
        let got = verdict_ro(&mut g, &hex::decode(c["frame"].as_str().unwrap()).unwrap());
        assert_eq!(c["verdict"].as_str().unwrap(), got, "{}", c["name"]);
    }
    assert!(cases.len() > 80);
}

fn rt_program() -> Vec<u8> {
    let mut p = [pattern(0xC000, 7), pattern(0x9000, 3), vec![0xFF; 0xE8000 - 0xC000 - 0x9000], slot()].concat();
    p.resize(0x100000, 0xFF);
    p
}

fn full_port(original: &[u8], target: &[u8], skip: bool) -> InstallPort {
    let mut g = InstallPort::new(null(), original, target, skip, Authorization::Full).unwrap();
    g.ro.monitor = true;
    g.ro.limits.insert("progmem", 0x100000);
    g.ro.limits.insert("q128", 0x1000000);
    g
}

#[test]
fn install_guard_follows_the_python_sequences() {
    let original = rt_program();
    let mut target = original.clone();
    target[0xC000..0xC300].copy_from_slice(&pattern(0x300, 11));
    target[0xC000 + 0x5000] = 0;
    target[0xE8000 + 0x20] ^= 1;
    let crcs = crc_list(&make_data());
    let mut other = crcs.clone();
    other[5] ^= 1;
    let bytes = |n: &str| -> Vec<u8> {
        match n {
            "original" => original.clone(),
            "target" => target.clone(),
            "original_app" => original[0xC000..].to_vec(),
            "target_app" => target[0xC000..].to_vec(),
            _ => panic!("{n}"),
        }
    };
    let list = |n: &str| -> Vec<u32> {
        match n {
            "crcs" => crcs.clone(),
            "other_crcs" => other.clone(),
            _ => panic!("{n}"),
        }
    };
    for seq in guards()["install_sequences"].as_array().unwrap() {
        let name = seq["name"].as_str().unwrap();
        let mut g = full_port(&original, &target, seq["skip_original"].as_bool().unwrap());
        let todo: Vec<usize> = seq["todo"].as_array().unwrap().iter().map(|x| x.as_u64().unwrap() as usize).collect();
        assert_eq!(todo, g.todo, "{name}");
        let plan: Vec<String> =
            seq["plan"].as_array().unwrap().iter().map(|x| x.as_str().unwrap().to_string()).collect();
        assert_eq!(plan, g.plan.iter().map(hex::encode).collect::<Vec<_>>(), "{name}");
        for (i, step) in seq["steps"].as_array().unwrap().iter().enumerate() {
            let kind = step["step"].as_str().unwrap();
            let arg = &step["arg"];
            let names: Vec<String> =
                arg.as_array().map(|a| a.iter().map(|x| x.as_str().unwrap().to_string()).collect()).unwrap_or_default();
            let r: ToolResult<Result<String, Vec<String>>> = (|| {
                Ok(Ok(match kind {
                    "frame" => g.validate(&hex::decode(arg.as_str().unwrap()).unwrap())?.to_string(),
                    "plan" => {
                        let f = g.plan[g.index].clone();
                        g.validate(&f)?.to_string()
                    }
                    "plan_all" => {
                        while g.index < g.plan.len() {
                            let f = g.plan[g.index].clone();
                            g.validate(&f)?;
                        }
                        "ok".into()
                    }
                    "authorize" => {
                        g.authorize(&bytes(&names[0]), &list(&names[1]), &list(&names[2]))?;
                        "ok".into()
                    }
                    "authorize_saved" => {
                        g.authorize_saved_backup()?;
                        "ok".into()
                    }
                    "finish" => {
                        g.finish_writes()?;
                        "ok".into()
                    }
                    "reset_ok" => {
                        g.authorize_reset(&bytes(&names[0]), &list(&names[1]), &list(&names[2]))?;
                        "ok".into()
                    }
                    "reset_app" => {
                        g.authorize_reset_application(&bytes(&names[0]))?;
                        "ok".into()
                    }
                    "reset_frames" => {
                        let frames = g.reset_plan.clone();
                        let mut out = vec![];
                        for f in frames {
                            out.push(g.validate(&f)?.to_string());
                        }
                        return Ok(Err(out));
                    }
                    k => panic!("{k}"),
                }))
            })();
            let w = format!("{name} step {i} ({kind})");
            match (r, &step["result"]) {
                (Ok(Err(list)), want) => assert_eq!(
                    want.as_array().unwrap().iter().map(|x| x.as_str().unwrap().to_string()).collect::<Vec<_>>(),
                    list,
                    "{w}"
                ),
                (Ok(Ok(s)), want) => assert_eq!(want.as_str().unwrap(), s, "{w}"),
                (Err(e), want) => assert_eq!(want.as_str().unwrap(), format!("refused: {}", e.message), "{w}"),
            }
            assert_eq!(step["index"].as_u64().unwrap() as usize, g.index, "{w}");
            assert_eq!(step["enabled"].as_bool().unwrap(), g.enabled, "{w}");
            assert_eq!(step["closed"].as_bool().unwrap(), g.closed_writes, "{w}");
        }
    }
}

#[test]
fn never_below_the_application_area_or_on_the_data_chip() {
    let original = rt_program();
    let mut boot = original.clone();
    boot[0x100] ^= 1;
    assert!(InstallPort::new(null(), &original, &boot, false, Authorization::Full).is_err());
    let mut target = original.clone();
    target[0xD000] ^= 1;
    let mut g = full_port(&original, &target, false);
    assert!(g.plan.iter().all(|f| hex::encode_upper(&f[6..12]) == "040000000100"));
    assert!(g.todo.iter().all(|&a| a >= 0xC000));
    g.authorize(&original, &[1, 2], &[1, 2]).unwrap();
    let q128 = [0x14, 0x0a, 0x00, 0x00, 0x03, 0x12];
    assert!(verdict(&mut g, &rm::packet(5, &[q128.to_vec(), vec![0, 1, 0, 0, 0x06]].concat())).starts_with("refused"));
    assert!(verdict(&mut g, &rm::spi_packet(&[0x20, 0x00, 0x80, 0x00], 0)).starts_with("refused"));
    assert!(verdict(&mut g, &rm::spi_packet(&[0xC7], 0)).starts_with("refused"));
    let p1 = g.plan[1].clone();
    let p0 = g.plan[0].clone();
    assert!(verdict(&mut g, &p1).starts_with("refused"));
    assert_eq!("program_write_enable", verdict(&mut g, &p0));
    assert!(verdict(&mut g, &p0).starts_with("refused"));
}

#[test]
fn no_restart_before_the_readback_matched() {
    let original = rt_program();
    let mut target = original.clone();
    target[0xD000] ^= 1;
    let mut g = full_port(&original, &target, false);
    let poke = rm::packet(3, &[be32_bytes(0x18007EF4).to_vec(), le32_bytes(0x54464F53).to_vec()].concat());
    assert!(verdict(&mut g, &poke).starts_with("refused"));
    g.authorize(&original, &[7], &[7]).unwrap();
    while g.index < g.plan.len() {
        let f = g.plan[g.index].clone();
        g.validate(&f).unwrap();
    }
    g.finish_writes().unwrap();
    assert!(g.authorize_reset(&original, &[7], &[7]).is_err());
    assert!(g.authorize_reset(&target, &[8], &[7]).is_err());
    assert!(verdict(&mut g, &poke).starts_with("refused"));
    g.authorize_reset(&target, &[7], &[7]).unwrap();
    assert_eq!("verified_restart_register", verdict(&mut g, &poke));
    assert!(g.authorize_reset(&target, &[7], &[7]).is_err());
}

#[test]
fn only_the_exact_pinned_monitor_image_is_loaded() {
    let mut g = ReadOnlyPort::new(null()).unwrap();
    let mut image = rm::image().unwrap();
    image[10] ^= 1;
    let changed =
        rm::rom_cmd(0x18, &[be32_bytes(0x10000).to_vec(), be32_bytes(64).to_vec(), image[..256].to_vec()].concat());
    assert!(verdict_ro(&mut g, &changed).starts_with("refused"));
    assert!(verdict_ro(&mut g, &rm::rom_cmd(0x1b, &be32_bytes(0x10000))).starts_with("refused"));
    use ddm32_core::radio::port::RadioPort;
    assert!(g.set_baud_rate(921600).is_err());
}

#[test]
fn radio_access_follows_the_build_feature() {
    // Without the `radio` feature (the default, and how the tests run) no port can be opened at all.
    if !RadioAccess::ENABLED {
        assert!(!RadioAccess::WRITES_ENABLED);
        assert_eq!("RadioDisabled", RadioAccess::factory("COM14").err().unwrap().py_type);
        assert_eq!("RadioDisabled", RadioAccess::write_factory("COM14").err().unwrap().py_type);
    }
    assert!(!ddm32_core::app_info::DEVELOPER);
}

#[test]
fn a_tool_without_a_given_port_opens_nothing() {
    let t = tmp();
    let lines = Lines::default();
    let print = lines.print();
    let clock = VirtualClock::default();
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &no_port, release_key_hex: None, codec: Codec::dm32() };
    let out = t.path().join("b").to_string_lossy().into_owned();
    let e = backup_tool(&argv(&["--port", "COM14", "--out", &out, "--installed-firmware", "x", "--wait", "5"]), &ctx)
        .unwrap_err();
    assert_eq!("NoPortGiven", e.py_type);
    let e = ddm32_core::radio::cps::data_pack_tool(&argv(&["read", "--port", "COM14"]), &ctx).unwrap_err();
    assert_eq!("NoPortGiven", e.py_type);
    assert!(lines.get().is_empty());
}

fn argv(a: &[&str]) -> Vec<String> {
    a.iter().map(|s| s.to_string()).collect()
}

#[test]
fn failed_writes_are_never_retried() {
    for name in ["install_readback_corrupt", "from_stock_slot_corrupt", "restore_readback_corrupt"] {
        let sc = scenario(name);
        let mut erases = std::collections::HashSet::new();
        for e in sc["events"].as_array().unwrap().iter().filter(|e| e[0] == "w") {
            let f = b64(e[1].as_str().unwrap());
            if f.len() > 16 && f[1] == 5 && f[16] == 0x20 {
                assert!(erases.insert(f.clone()), "{name}: a sector was erased twice");
            }
            assert!(!(f.len() > 1 && f[1] == 3), "{name}: a restart register");
        }
        assert!(!erases.is_empty());
    }
}

// --- the fake radio and the faster paths ----------------------------------------------------------------------------

fn inputs() -> &'static (PathBuf, Vec<u8>, Vec<u8>, Vec<u8>, Vec<u8>) {
    static I: OnceLock<(PathBuf, Vec<u8>, Vec<u8>, Vec<u8>, Vec<u8>)> = OnceLock::new();
    I.get_or_init(|| {
        let r = RadioInputs::new();
        (r.dir.keep(), r.program, r.data, r.stock, r.target)
    })
}

fn backup() -> PathBuf {
    inputs().0.join("backup")
}
fn stock_backup() -> PathBuf {
    inputs().0.join("stock-backup")
}
fn plus() -> PathBuf {
    inputs().0.join("plus")
}

fn out(name: &str) -> PathBuf {
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    inputs().0.join(format!("fast-{name}-{}", N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)))
}

type Tool = fn(&[String], &ToolContext<'_>) -> ToolResult<()>;

fn run(
    tool: Tool,
    program: &[u8],
    args: Vec<String>,
    setup: impl FnOnce(&mut FakeRadio),
    stock: bool,
) -> (Rc<RefCell<FakeRadio>>, Vec<String>, Option<ToolError>) {
    let vclock = Rc::new(VirtualClock::default());
    let clock = SharedClock(vclock.clone());
    let radio = Rc::new(RefCell::new(FakeRadio::new(program, &inputs().2, Rc::new(SharedClock(vclock)))));
    setup(&mut radio.borrow_mut());
    let lines = Lines::default();
    let print = lines.print();
    let r2 = radio.clone();
    let opened = Rc::new(RefCell::new(false));
    let o2 = opened.clone();
    let serial = move |_: &str| -> ToolResult<Box<dyn RawSerial>> {
        *o2.borrow_mut() = true;
        Ok(Box::new(Shared(r2.clone())))
    };
    let syn = synthetic_codec_tables();
    let codec: &Codec = if stock { &syn } else { Codec::dm32() };
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: Some(test_public_hex()), codec };
    let mut a = argv(&["--port", "SIMULATED"]);
    a.extend(args);
    let err = tool(&a, &ctx).err();
    if !*opened.borrow() {
        assert!(radio.borrow().sent.is_empty());
    }
    (radio, lines.get(), err)
}

fn python_writes(name: &str) -> Vec<Vec<u8>> {
    scenario(name)["events"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|e| e[0] == "w")
        .map(|e| b64(e[1].as_str().unwrap()))
        .collect()
}

fn changed() -> Vec<u8> {
    let mut c = inputs().1.clone();
    c[START + 0x20000] ^= 0xff;
    c
}
fn broken() -> Vec<u8> {
    let mut b = inputs().4.clone();
    b[START + 3 * SECTOR..START + 5 * SECTOR].fill(0xFF);
    b[SLOT + 0x200..SLOT + 0x210].fill(0);
    b
}
fn stock_installed() -> Vec<u8> {
    [inputs().3[..START].to_vec(), inputs().1[START..].to_vec()].concat()
}

#[test]
fn the_fake_radio_draws_the_same_bytes_as_the_python_one() {
    for name in [
        "backup_ok",
        "backup_radio_off",
        "install_ok",
        "install_live_program_changed",
        "install_readback_corrupt",
        "install_slot_changed",
        "install_wrong_flash_id",
        "restore_ok",
        "restore_readback_corrupt",
        "from_stock_ok",
        "restore_stock_ok",
    ] {
        let sc = scenario(name);
        let r = &sc["radio"];
        let h = r["image"].as_str().unwrap();
        let (_, p, _, s, t) = inputs();
        let program = [p.clone(), t.clone(), s.clone(), changed(), broken(), stock_installed()]
            .into_iter()
            .find(|x| sha(x) == h)
            .unwrap_or_else(|| panic!("unknown image {h}"));
        let o = out(name);
        let args: Vec<String> = sc["args"]
            .as_array()
            .unwrap()
            .iter()
            .map(|a| {
                a.as_str()
                    .unwrap()
                    .replace("{out}", &o.to_string_lossy())
                    .replace("{backup}", &backup().to_string_lossy())
                    .replace("{stock_backup}", &stock_backup().to_string_lossy())
                    .replace("{plus}", &plus().to_string_lossy())
            })
            .collect();
        let tool: Tool = match sc["tool"].as_str().unwrap() {
            "backup" => backup_tool,
            "install_plus" => install_tool,
            _ => restore_stock_tool,
        };
        let r2 = r.clone();
        let (radio, _, err) = run(
            tool,
            &program,
            args,
            move |f| {
                if let Some(j) = r2.get("data_jedec") {
                    f.data_jedec = j.as_str().unwrap().into();
                }
                if let Some(c) = r2.get("corrupt_after_writes") {
                    f.corrupt_after_writes = Some(c.as_u64().unwrap() as usize);
                }
                if r2["setup"].is_string() {
                    f.silent = true;
                }
            },
            sc["stock_codec"].as_bool().unwrap(),
        );
        let want = python_writes(name);
        let radio = radio.borrow();
        assert_eq!(want.len(), radio.sent.len(), "{name}");
        for (i, w) in want.iter().enumerate() {
            assert_eq!(w, &radio.sent[i], "{name}: write {i} differs");
        }
        assert_eq!(sc["error"].is_null(), err.is_none(), "{name}");
        assert!(radio.errors.is_empty(), "{name}: {:?}", radio.errors);
    }
}

fn radio_with_other_build() -> Vec<u8> {
    program(&[pattern(0x900, 21), rep(0x9000), b"DDM32 Plus 1.1".to_vec()].concat(), None)
}

fn quick(o: &Path) -> Vec<String> {
    argv(&[
        "--quick",
        "--backup",
        &backup().to_string_lossy(),
        "--plus",
        &plus().to_string_lossy(),
        "--out",
        &o.to_string_lossy(),
        "--wait",
        "5",
    ])
}

fn reads(frames: &[Vec<u8>]) -> Vec<(u8, u64, usize, &'static str)> {
    let mut r = vec![];
    for f in frames.iter().filter(|f| f.len() > 6 && f[0] == 2) {
        let mem = if f[6] == 0x04 { "progmem" } else { "q128" };
        if f[1] == 9 && f.len() == 18 {
            r.push((9, be32(&f[12..]) as u64, f[16] as usize, mem));
        }
        if f[1] == 7 && f.len() == 18 {
            r.push((7, be32(&f[12..]) as u64, (f[16] as usize) << 8 | f[17] as usize, mem));
        }
    }
    r
}

fn first_write(radio: &FakeRadio) -> usize {
    radio.sent.iter().position(|f| f.len() > 16 && f[1] == 5 && (f[16] == 0x20 || f[16] == 0x02)).unwrap()
}

fn receipt(o: &Path) -> serde_json::Value {
    serde_json::from_slice(&std::fs::read(o.join("install-receipt.json")).unwrap()).unwrap()
}

fn files(o: &Path) -> Vec<String> {
    std::fs::read_dir(o).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect()
}

#[test]
fn quick_update_rewrites_the_application_and_nothing_else() {
    let o = out("quick-ok");
    let (radio, lines, err) = run(fast_install_tool, &radio_with_other_build(), quick(&o), |_| {}, false);
    assert!(err.is_none(), "{err:?}");
    let radio = radio.borrow();
    assert!(lines.iter().any(|l| l.starts_with("PASS:")));
    assert_eq!(inputs().4, radio.images["progmem"]);
    assert_eq!(inputs().2, radio.images["q128"]);
    assert!(radio.writes.iter().all(|w| w.0 == "progmem" && w.2 >= START && w.2 < SLOT));
    let erased: Vec<usize> = radio.writes.iter().filter(|w| w.1 == 0x20).map(|w| w.2).collect();
    assert_eq!((0..(SLOT - START) / SECTOR).map(|i| START + i * SECTOR).collect::<Vec<_>>(), erased);
    assert_eq!(7, radio.pokes.len());
    let first = first_write(&radio);
    let before: Vec<_> = reads(&radio.sent[..first]).into_iter().filter(|r| r.0 == 9 && r.3 == "progmem").collect();
    assert!(!before.is_empty());
    assert!(before.iter().all(|r| r.1 as usize + r.2 * SECTOR <= START || r.1 as usize >= SLOT));
    let rc = receipt(&o);
    for k in [
        "installation_verified",
        "codec_slot_verified",
        "data_sector_crcs_unchanged",
        "restart_sent",
        "live_boot_area_matches_backup",
        "live_codec_slot_matches_backup",
    ] {
        assert_eq!(rc[k], true, "{k}");
    }
    assert_eq!(rc["backup"], "reused");
    assert_eq!(vec!["install-receipt.json"], files(&o));
}

#[test]
fn quick_update_refuses_another_radio() {
    for (what, at) in [("boot", 0x100usize), ("slot", SLOT + 0x90)] {
        let mut p = radio_with_other_build();
        p[at] ^= 1;
        let o = out(&format!("quick-{what}"));
        let (radio, _, err) = run(fast_install_tool, &p, quick(&o), |_| {}, false);
        assert!(err.unwrap().message.contains("does not match the backup"));
        assert!(radio.borrow().writes.is_empty() && radio.borrow().pokes.is_empty());
        assert_eq!(receipt(&o)["writes_started"], false);
    }
}

#[test]
fn quick_update_refuses_other_flash_chips() {
    let (radio, _, err) = run(
        fast_install_tool,
        &radio_with_other_build(),
        quick(&out("quick-jedec")),
        |f| f.data_jedec = "ef4017".into(),
        false,
    );
    assert_eq!("Attached flash IDs differ from the saved unit", err.unwrap().message);
    assert!(radio.borrow().writes.is_empty());
    assert!(!radio.borrow().ops.contains(&9));
}

#[test]
fn quick_update_never_from_stock_or_over_another_firmware() {
    let o = out("quick-stock");
    let a = argv(&[
        "--quick",
        "--backup",
        &stock_backup().to_string_lossy(),
        "--plus",
        &plus().to_string_lossy(),
        "--out",
        &o.to_string_lossy(),
    ]);
    let (radio, _, err) = run(fast_install_tool, &inputs().3, a, |_| {}, true);
    assert!(err.unwrap().message.contains("first install from stock firmware needs a new backup"));
    assert!(radio.borrow().sent.is_empty());
    assert!(!o.exists());
    let open25 = out("open25-backup");
    write_backup(
        &open25,
        &program(&[b"open25-classic-r13".to_vec(), pattern(0x900, 3)].concat(), None),
        &inputs().2,
        true,
    );
    let a = argv(&[
        "--quick",
        "--backup",
        &open25.to_string_lossy(),
        "--plus",
        &plus().to_string_lossy(),
        "--out",
        &out("q25").to_string_lossy(),
    ]);
    let (radio, _, err) = run(fast_install_tool, &inputs().1, a, |_| {}, false);
    assert!(err.unwrap().message.contains("only for an update over an installed DDM32 Plus"));
    assert!(radio.borrow().sent.is_empty());
}

#[test]
fn quick_update_plan_only_opens_no_port_and_keeps_the_slot() {
    let mut a = quick(&out("quick-plan"));
    a.push("--plan-only".into());
    let (radio, lines, err) = run(fast_install_tool, &inputs().1, a, |_| {}, false);
    assert!(err.is_none());
    assert!(radio.borrow().sent.is_empty());
    let plan: serde_json::Value = serde_json::from_str(&lines.join("\n")).unwrap();
    assert_eq!(plan["mode"], "install_plus_quick");
    assert_eq!(plan["changed_sectors"], (SLOT - START) / SECTOR);
    assert_eq!(plan["last"], "0xe7000");
    assert_eq!(plan["codec_slot_unchanged"], true);
    assert_eq!(plan["data_chip_written"], false);
}

#[test]
fn quick_update_readback_failure_blocks_the_restart_without_retry() {
    let (radio, _, err) = run(
        fast_install_tool,
        &radio_with_other_build(),
        quick(&out("quick-bad")),
        |f| f.corrupt_after_writes = Some(START + 0x10),
        false,
    );
    assert_eq!("Post-write verification failed; reset remains disabled", err.unwrap().message);
    let radio = radio.borrow();
    assert!(radio.pokes.is_empty());
    let erased: Vec<usize> = radio.writes.iter().filter(|w| w.1 == 0x20).map(|w| w.2).collect();
    let mut d = erased.clone();
    d.dedup();
    assert_eq!(erased.len(), d.len());
}

fn no_backup(o: &Path) -> Vec<String> {
    argv(&["--no-backup", "--plus", &plus().to_string_lossy(), "--out", &o.to_string_lossy(), "--wait", "5"])
}

#[test]
fn no_backup_reads_the_program_once_and_keeps_every_check() {
    let now = radio_with_other_build();
    let o = out("nb-ok");
    let (radio, lines, err) = run(fast_install_tool, &now, no_backup(&o), |_| {}, false);
    assert!(err.is_none(), "{err:?}");
    let radio = radio.borrow();
    assert!(lines.iter().any(|l| l.starts_with("PASS:")));
    let mut target = now[..START].to_vec();
    target.extend(plus_app());
    target.resize(SLOT, 0xFF);
    target.extend_from_slice(&now[SLOT..]);
    assert_eq!(target, radio.images["progmem"]);
    assert_eq!(inputs().2, radio.images["q128"]);
    assert!(radio.writes.iter().all(|w| w.0 == "progmem" && w.2 >= START && w.2 < SLOT));
    let first = first_write(&radio);
    let sectors: Vec<u64> = reads(&radio.sent[..first])
        .into_iter()
        .filter(|r| r.0 == 9 && r.3 == "progmem")
        .flat_map(|r| (0..r.2).map(move |i| r.1 + (i * SECTOR) as u64))
        .collect();
    assert_eq!((0..SIZE / SECTOR).map(|i| (i * SECTOR) as u64).collect::<Vec<_>>(), sectors);
    assert_eq!(vec!["install-receipt.json"], files(&o));
    let rc = receipt(&o);
    assert_eq!(rc["backup"], "none");
    assert_eq!(rc["live_program_sha256"], sha(&now).as_str());
    assert_eq!(rc["restart_sent"], true);
    assert_eq!(rc["data_sector_crcs_unchanged"], true);
}

#[test]
fn no_backup_never_from_stock() {
    let o = out("nb-stock");
    let (radio, _, err) = run(fast_install_tool, &inputs().3, no_backup(&o), |_| {}, true);
    assert!(err.unwrap().message.contains("first install from stock firmware needs a new backup"));
    assert!(radio.borrow().writes.is_empty() && radio.borrow().pokes.is_empty());
    assert_eq!(receipt(&o)["writes_started"], false);
    let (radio, _, err) = run(
        fast_install_tool,
        &program(&[b"open25-classic-r13".to_vec(), pattern(0x900, 3)].concat(), None),
        no_backup(&out("nb-25")),
        |_| {},
        false,
    );
    assert!(err.unwrap().message.contains("only for an update over an installed DDM32 Plus"));
    assert!(radio.borrow().writes.is_empty());
}

#[test]
fn no_backup_readback_failure_blocks_the_restart() {
    let (radio, _, err) = run(
        fast_install_tool,
        &radio_with_other_build(),
        no_backup(&out("nb-bad")),
        |f| f.corrupt_after_writes = Some(SLOT + 0x90),
        false,
    );
    assert_eq!("Codec slot changed; restart disabled", err.unwrap().message);
    assert!(radio.borrow().pokes.is_empty());
}

#[test]
fn fast_install_argument_refusals() {
    for flags in [
        vec!["--no-backup", "--quick"],
        vec!["--no-backup", "--plan-only"],
        vec!["--quick"],
        vec!["--no-backup", "--backup"],
    ] {
        let mut a = argv(&["--plus", &plus().to_string_lossy(), "--out", &out("args").to_string_lossy()]);
        for f in flags {
            a.push(f.into());
            if f == "--backup" {
                a.push(backup().to_string_lossy().into_owned());
            }
        }
        let (radio, _, err) = run(fast_install_tool, &inputs().1, a, |_| {}, false);
        assert_eq!("SystemExit", err.unwrap().py_type);
        assert!(radio.borrow().sent.is_empty());
    }
}

#[test]
fn identity_guard_rules() {
    let (basis, target, _) = quick_inputs(&backup(), &plus(), Some(&test_public_hex()), Codec::dm32()).unwrap();
    let mut g = InstallPort::new(null(), &basis, &target, false, Authorization::Identity).unwrap();
    g.ro.monitor = true;
    assert!(g.todo.iter().all(|&a| a < SLOT));
    assert!(g.authorize(&basis, &[1], &[1]).is_err());
    let mut other_boot = target[..START].to_vec();
    other_boot[5] ^= 1;
    assert!(g.authorize_identity(&other_boot, &target[SLOT..]).is_err());
    let p0 = g.plan[0].clone();
    assert!(verdict(&mut g, &p0).starts_with("refused"));
    g.authorize_identity(&target[..START], &target[SLOT..]).unwrap();
    assert_eq!("program_write_enable", g.validate(&p0).unwrap());
    let mut slot_changed = target.clone();
    slot_changed[SLOT + 4] ^= 1;
    assert!(InstallPort::new(null(), &basis, &slot_changed, false, Authorization::Identity).is_err());
    let mut full = InstallPort::new(null(), &basis, &target, false, Authorization::Full).unwrap();
    assert!(full.authorize_identity(&target[..START], &target[SLOT..]).is_err());
}

#[test]
fn deferred_plan_is_read_only_until_set_once_and_authorized() {
    let mut g = InstallPort::deferred(null()).unwrap();
    g.ro.monitor = true;
    g.ro.limits.insert("progmem", SIZE as u64);
    g.ro.limits.insert("q128", 0x1000000);
    assert!(g.authorize(&inputs().1, &[1], &[1]).is_err());
    assert!(verdict(&mut g, &rm::spi_packet(&[0x06], 0)).starts_with("refused"));
    assert_eq!("sector_read", g.validate(&rm::packet(9, &[0x04, 0, 0, 0, 1, 0, 0, 0, 0, 0, 16, 1])).unwrap());
    g.set_images(&inputs().1, &inputs().4).unwrap();
    assert!(g.set_images(&inputs().1, &inputs().1).is_err());
    g.authorize(&inputs().1, &[1], &[1]).unwrap();
    let p0 = g.plan[0].clone();
    assert_eq!("program_write_enable", g.validate(&p0).unwrap());
}

// --- the flows ----------------------------------------------------------------------------------------------------

fn det(layout: &str, mode: Option<&str>, supported: bool, reason: &str) -> PyDict {
    PyDict::new().with("layout", layout).with("install_mode", mode).with("supported", supported).with("reason", reason)
}
fn classic() -> PyDict {
    det("classic", Some("over_classic"), true, "")
}
fn stock_d() -> PyDict {
    det("stock", Some("from_stock"), true, "")
}
fn unknown() -> PyDict {
    det("unknown", None, false, "Unknown firmware: no codec slot")
}
fn plan_ok() -> PyDict {
    PyDict::new().with("boot_area_unchanged", true).with("data_chip_written", false)
}
fn pkg(sha: &str, ok: bool) -> Option<Installable> {
    Some(Installable { sha256: sha.into(), installable: ok })
}
fn backup_ok() -> StepResult {
    StepResult::new(true, false, false, "")
}
fn install_pass() -> StepResult {
    StepResult::new(true, true, false, "")
}
fn install_timeout() -> StepResult {
    StepResult::new(false, false, true, "DumpError: the radio did not start its serial boot ROM within 600 s")
}
fn install_mid_write() -> StepResult {
    StepResult::new(false, true, false, "OSError: cable")
}

fn ready(d: PyDict, p: Option<Installable>) -> Wizard {
    let mut w = Wizard::new();
    w.set_port(Some("COM7")).unwrap();
    w.set_package(p.or(pkg("aaaa", true))).unwrap();
    w.start_backup(Path::new("b1")).unwrap();
    w.backup_finished(&backup_ok());
    w.start_detect().unwrap();
    w.detect_finished(Some(d), "");
    w
}

#[test]
fn flow_order_backup_detect_plan_install() {
    let mut w = Wizard::new();
    assert!(!(w.can_backup() || w.can_plan() || w.can_install()));
    w.set_port(Some("COM7")).unwrap();
    w.set_package(pkg("aaaa", true)).unwrap();
    assert!(!w.can_detect());
    w.start_backup(Path::new("b1")).unwrap();
    assert!(!w.can_backup());
    w.backup_finished(&backup_ok());
    assert!(w.can_detect() && !w.can_plan());
    w.start_detect().unwrap();
    w.detect_finished(Some(classic()), "");
    assert_eq!(Some("install_plus"), w.mode());
    assert!(w.can_plan() && !w.can_install());
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", true);
    assert!(w.can_install());
    w.start_install().unwrap();
    assert!(!w.can_install());
    w.install_finished(&install_pass());
    assert_eq!(StepState::Ok, w.install);
    assert_eq!(StepState::Stale, w.backup);
    assert!(!w.can_plan());
}

#[test]
fn flow_stock_mode_and_plan_key() {
    let mut w = ready(stock_d(), None);
    assert_eq!(Some("install_plus_from_stock"), w.mode());
    w.start_plan().unwrap();
    assert_eq!(Some((PathBuf::from("b1"), "aaaa".to_string(), "install_plus_from_stock".to_string())), w.plan_key);
    w.plan_finished(Some(plan_ok()), "", true);
    assert!(w.can_install());
    w.set_package(pkg("aaaa", true)).unwrap();
    assert!(w.can_install());
    w.set_package(pkg("bbbb", true)).unwrap();
    assert!(!w.can_install());
}

#[test]
fn flow_unsupported_radio_blocks_plan() {
    let w = ready(unknown(), None);
    assert_eq!(None, w.mode());
    assert!(!w.can_plan());
    assert_eq!("Wiz_Unsupported", w.message.0);
    let mut w2 = Wizard::new();
    w2.set_port(Some("COM7")).unwrap();
    w2.set_package(pkg("aaaa", true)).unwrap();
    w2.start_backup(Path::new("b")).unwrap();
    w2.backup_finished(&backup_ok());
    w2.start_detect().unwrap();
    w2.detect_finished(None, "boom");
    assert_eq!(StepState::Failed, w2.detect_state);
    assert!(!w2.can_plan());
}

#[test]
fn flow_steps_refuse_out_of_order_and_nothing_starts_by_itself() {
    let mut w = Wizard::new();
    assert!(w.start_detect().is_err());
    assert!(w.start_plan().is_err());
    assert!(w.start_install().is_err());
    assert!(w.start_backup(Path::new("b")).is_err());
    let mut w = ready(classic(), None);
    assert!(w.running.is_none());
    assert_eq!(StepState::Idle, w.plan);
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", true);
    assert_eq!(StepState::Idle, w.install);
}

#[test]
fn flow_unsafe_plan_and_unsigned_package() {
    let mut w = ready(classic(), None);
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", false);
    assert_eq!(StepState::Failed, w.plan);
    assert!(!w.can_install());
    assert!(!ready(classic(), pkg("aaaa", false)).can_plan());
}

#[test]
fn flow_one_plan_one_install_and_no_automatic_retry() {
    let mut w = ready(classic(), None);
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", true);
    w.start_install().unwrap();
    w.install_finished(&install_timeout());
    assert_eq!("Wiz_InstallNotStartedTimeout", w.message.0);
    assert!(!w.can_install());
    assert!(w.can_plan());
}

#[test]
fn flow_failed_install_after_writes_is_locked_until_a_new_backup() {
    let mut w = ready(classic(), None);
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", true);
    w.start_install().unwrap();
    w.install_finished(&install_mid_write());
    assert!(w.locked);
    assert!(!(w.can_plan() || w.can_install() || w.can_detect()));
    w.start_backup(Path::new("b2")).unwrap();
    w.backup_finished(&backup_ok());
    assert!(!w.locked);
}

#[test]
fn flow_port_change_refused_while_a_radio_step_runs() {
    let mut w = Wizard::new();
    w.set_port(Some("COM7")).unwrap();
    w.start_backup(Path::new("b")).unwrap();
    assert!(w.set_port(Some("COM8")).is_err());
}

fn ddm32_d() -> PyDict {
    PyDict::new()
        .with("layout", "classic")
        .with("firmware", "ddm32")
        .with("install_mode", "over_classic")
        .with("supported", true)
        .with("reason", "")
}
fn open25_d() -> PyDict {
    PyDict::new()
        .with("layout", "classic")
        .with("firmware", "classic")
        .with("install_mode", "over_classic")
        .with("supported", true)
        .with("reason", "")
}

fn quick_ready(d: PyDict) -> Wizard {
    let mut w = Wizard::new();
    w.set_port(Some("COM7")).unwrap();
    w.set_package(pkg("aaaa", true)).unwrap();
    w.use_quick_backup(Path::new("old-backup")).unwrap();
    w.start_detect().unwrap();
    w.detect_finished(Some(d), "");
    w
}

#[test]
fn flow_quick_update_only_over_ddm32() {
    let mut w = quick_ready(ddm32_d());
    assert_eq!(BackupPath::Quick, w.path);
    assert_eq!(Some("install_plus_quick"), w.mode());
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", true);
    assert!(w.can_install());
    let (tool, args) = w.install_command("plus", "out").unwrap();
    assert_eq!("fast_install", tool);
    assert_eq!(
        argv(&[
            "--port",
            "COM7",
            "--quick",
            "--backup",
            "old-backup",
            "--plus",
            "plus",
            "--out",
            "out",
            "--wait",
            "600"
        ]),
        args
    );
    for d in [stock_d(), open25_d(), unknown()] {
        let q = quick_ready(d);
        assert_eq!(None, q.mode());
        assert!(!q.can_plan());
    }
    assert_eq!("Wiz_QuickNeedsDdm32", quick_ready(stock_d()).message.0);
    let mut w = quick_ready(ddm32_d());
    w.use_existing_backup(Path::new("b")).unwrap();
    assert_eq!(BackupPath::Normal, w.path);
}

#[test]
fn flow_install_without_backup_needs_the_risk_accepted() {
    let mut w = Wizard::new();
    w.set_port(Some("COM7")).unwrap();
    w.set_package(pkg("aaaa", true)).unwrap();
    assert!(w.choose_no_backup(false).is_err());
    assert_eq!(BackupPath::Normal, w.path);
    assert!(!w.can_install());
    w.choose_no_backup(true).unwrap();
    assert_eq!((BackupPath::None, true, StepState::Skipped), (w.path, w.risk_accepted, w.backup));
    assert!(!w.can_detect());
    assert_eq!(Some("install_plus_no_backup"), w.mode());
    assert!(w.can_install());
    assert_eq!(
        argv(&["--port", "COM7", "--no-backup", "--plus", "p", "--out", "o", "--wait", "600"]),
        w.install_command("p", "o").unwrap().1
    );
    w.set_port(None).unwrap();
    assert!(!w.can_install());
    w.set_port(Some("COM7")).unwrap();
    w.start_install().unwrap();
    w.install_finished(&install_mid_write());
    assert!(w.locked);
    assert_eq!("Wiz_NoBackupStoppedAfterWrites", w.message.0);
    assert!(!w.can_install());
    let mut w = Wizard::new();
    w.set_port(Some("COM7")).unwrap();
    w.set_package(pkg("aaaa", true)).unwrap();
    w.choose_no_backup(true).unwrap();
    w.start_install().unwrap();
    w.install_finished(&StepResult::new(
        false,
        false,
        false,
        "ValueError: first install from stock firmware needs a new backup",
    ));
    assert!(!w.locked && w.running.is_none() && w.can_install());
    w.start_backup(Path::new("b")).unwrap();
    assert_eq!((BackupPath::Normal, false), (w.path, w.risk_accepted));
}

#[test]
fn flow_the_backup_step_keeps_its_own_result() {
    let mut w = Wizard::new();
    w.set_port(Some("COM7")).unwrap();
    w.set_package(pkg("aaaa", true)).unwrap();
    w.start_backup(Path::new("backups/dm32-20261120")).unwrap();
    assert_eq!("Wiz_BackupRunning", w.backup_message.0);
    w.backup_finished(&backup_ok());
    assert_eq!("Wiz_BackupDone", w.backup_message.0);
    assert_eq!("backups/dm32-20261120", w.backup_message.1[0]);
    w.start_detect().unwrap();
    w.detect_finished(Some(classic()), "");
    w.start_plan().unwrap();
    w.plan_finished(Some(plan_ok()), "", true);
    assert_eq!("Wiz_PlanReady", w.message.0);
    assert_eq!("Wiz_BackupDone", w.backup_message.0);
    w.start_backup(Path::new("b2")).unwrap();
    w.backup_finished(&StepResult::new(
        false,
        false,
        true,
        "DumpError: the radio did not start its serial boot ROM within 120 s",
    ));
    assert_eq!("Wiz_BackupTimeout", w.backup_message.0);
}

#[test]
fn flow_normal_path_is_the_default() {
    let w = Wizard::new();
    assert_eq!(BackupPath::Normal, w.path);
    assert!(!w.risk_accepted);
    assert_eq!("install_plus", ready(classic(), None).install_command("p", "o").unwrap().0);
    assert_eq!("--from-stock", ready(stock_d(), None).install_command("p", "o").unwrap().1.last().unwrap());
}

fn restore_flow(kind: &str, d: Option<PyDict>) -> RestoreFlow {
    let mut f = RestoreFlow::new();
    f.port = Some("COM7".into());
    f.choose(kind, Path::new("bk")).unwrap();
    f.start_detect().unwrap();
    f.detect_finished(d, "");
    f
}

#[test]
fn restore_tool_follows_the_backup() {
    assert_eq!("install_firstboot", restore_flow("stock", Some(stock_d())).tool().unwrap().0);
    assert_eq!(
        vec!["--restore-stock", "--use-saved-backup"],
        restore_flow("previous", Some(stock_d())).tool().unwrap().1
    );
    assert_eq!("restore", restore_flow("previous", Some(classic())).tool().unwrap().2);
    let f = restore_flow("stock", Some(classic()));
    assert!(f.tool().is_none());
    assert_eq!("Res_NotStock", f.message.0);
    assert!(restore_flow("previous", Some(unknown())).tool().is_none());
    assert!(restore_flow("previous", None).tool().is_none());
}

#[test]
fn restore_steps() {
    let mut f = restore_flow("stock", Some(stock_d()));
    assert!(!f.can_restore());
    f.start_plan().unwrap();
    f.plan_finished(Some(plan_ok()), "", true);
    assert!(f.can_restore());
    f.port = None;
    assert!(!f.can_restore());
    f.port = Some("COM7".into());
    f.start_restore().unwrap();
    assert!(!f.can_restore());
    f.restore_finished(&install_mid_write());
    assert_eq!(StepState::Failed, f.restore);
    assert!(!f.can_restore() && f.can_plan());
    f.start_plan().unwrap();
    f.plan_finished(Some(plan_ok()), "", true);
    f.start_restore().unwrap();
    f.restore_finished(&install_pass());
    assert_eq!(StepState::Ok, f.restore);
}

fn printed_plan(name: &str) -> String {
    let lines: Vec<String> =
        scenario(name)["lines"].as_array().unwrap().iter().map(|l| l.as_str().unwrap().to_string()).collect();
    let end = lines.iter().position(|l| l == "}").unwrap();
    lines[..=end].join("\n")
}

#[test]
fn file_steps_print_the_tools_plans() {
    let k = test_public_hex();
    let dump = |p: PyDict| pyjson::dumps2(&Py::Dict(p));
    assert_eq!(
        printed_plan("install_plan_only"),
        dump(FileSteps::plan("install_plus", &backup(), &plus(), Some(&k), Codec::dm32()).unwrap())
    );
    assert_eq!(
        printed_plan("from_stock_plan_only"),
        dump(
            FileSteps::plan("install_plus_from_stock", &stock_backup(), &plus(), Some(&k), &synthetic_codec_tables())
                .unwrap()
        )
    );
    assert_eq!(printed_plan("restore_plan_only"), dump(FileSteps::restore_plan("restore", &backup()).unwrap()));
    assert_eq!(
        printed_plan("restore_stock_plan_only"),
        dump(FileSteps::restore_plan("restore_stock", &stock_backup()).unwrap())
    );
    let d = FileSteps::detect_backup(&backup(), Codec::dm32()).unwrap();
    assert_eq!(Some("over_classic"), d.str("install_mode"));
    assert!(FileSteps::safe(&FileSteps::restore_plan("restore", &backup()).unwrap()));
    assert!(FileSteps::plan("install_plus", &backup(), &plus(), None, Codec::dm32()).unwrap_err().is_value());
}

#[test]
fn step_results_from_what_the_tools_left() {
    let t = tmp();
    assert!(!StepResult::install(t.path(), Some(&ToolError::dump("x"))).writes_started);
    std::fs::write(t.path().join("install-receipt.json"), "{\"writes_started\": true, \"restart_sent\": false}")
        .unwrap();
    let r = StepResult::install(
        t.path(),
        Some(&ToolError::value("Post-write verification failed; reset remains disabled")),
    );
    assert!(r.writes_started && !r.passed);
    std::fs::write(t.path().join("install-receipt.json"), "{\"writes_started\": true, \"restart_sent\": true}")
        .unwrap();
    assert!(StepResult::install(t.path(), None).passed);
    assert!(
        StepResult::backup(
            t.path(),
            Some(&ToolError::dump("the radio did not start its serial boot ROM within 120 s: x"))
        )
        .timeout
    );
    std::fs::write(t.path().join("backup-manifest.json"), "{\"backup_verified\": true}").unwrap();
    assert!(StepResult::backup(t.path(), None).passed);
}

#[test]
fn cable_check_finds_a_missing_driver_without_a_port() {
    let dev = |id: &str, name: &str, code: i32| PnpDevice { device_id: id.into(), name: name.into(), error_code: code };
    let ok = dev(r"USB\VID_1A86&PID_7523\5&2A1B&0&3", "USB-SERIAL CH340 (COM14)", 0);
    let no_driver = dev(r"USB\VID_1A86&PID_7523\5&2A1B&0&4", "USB Serial", 28);
    let other = dev(r"USB\VID_0403&PID_6001\A1", "USB Serial Port (COM3)", 0);
    let virt = dev(r"COM0COM\PORT\CNCA0", "com0com - serial port emulator (COM12)", 0);
    assert_eq!(CableState::NoCable, evaluate(&[other.clone(), virt.clone()]).state);
    let s = evaluate(&[other, no_driver.clone()]);
    assert_eq!((CableState::NoDriver, 28), (s.state, s.error_code));
    let s = evaluate(&[no_driver, ok.clone(), virt]);
    assert_eq!((CableState::Ok, Some("COM14".to_string())), (s.state, s.port));
    assert_eq!(
        CableState::Problem,
        evaluate(&[PnpDevice { error_code: 10, name: "USB-SERIAL CH340".into(), ..ok.clone() }]).state
    );
    assert_eq!(CableState::NoPort, evaluate(&[PnpDevice { name: "USB-SERIAL CH340".into(), ..ok.clone() }]).state);
    let two = evaluate(&[PnpDevice { name: "USB-SERIAL CH340 (COM15)".into(), ..ok.clone() }, ok]);
    assert_eq!(vec!["COM14", "COM15"], two.ports);
}

// --- progress lines ---

fn track(lines: &[String]) -> Vec<(&'static str, i64)> {
    let mut t = ProgressTracker::new();
    lines.iter().filter_map(|l| t.feed(l)).collect()
}

fn scenario_lines(name: &str) -> Vec<String> {
    scenario(name)["lines"].as_array().unwrap().iter().map(|l| l.as_str().unwrap().to_string()).collect()
}

#[test]
fn progress_install_phases_in_order() {
    let p = track(&scenario_lines("install_ok"));
    let mut keys: Vec<&str> = vec![];
    for (k, _) in &p {
        if !keys.contains(k) {
            keys.push(k);
        }
    }
    assert_eq!(vec!["Prog_Compare", "Prog_Write", "Prog_Verify"], keys);
    let compare: Vec<i64> = p.iter().filter(|x| x.0 == "Prog_Compare").map(|x| x.1).collect();
    let mut sorted = compare.clone();
    sorted.sort();
    assert_eq!(sorted, compare);
    assert_eq!(100, *compare.last().unwrap());
    assert!(compare.contains(&50));
    assert_eq!(100, p.iter().filter(|x| x.0 == "Prog_Write").map(|x| x.1).max().unwrap());
    assert_eq!(100, p.last().unwrap().1);
}

#[test]
fn progress_the_owners_lines() {
    let mut t = ProgressTracker::new();
    assert_eq!(Some(("Prog_Compare", 20)), t.feed("Read current firmware: 40%"));
    assert_eq!(Some(("Prog_Compare", 75)), t.feed("Check settings chip: 50%"));
    assert_eq!(Some(("Prog_Write", 25)), t.feed("Written and sector-readback checked: 25% (1/4)"));
    assert_eq!(Some(("Prog_Verify", 60)), t.feed("Recheck untouched settings chip: 20%"));
    assert_eq!(None, t.feed("program flash JEDEC ID c84014"));
    assert_eq!(None, t.feed("progmem pass 1: 25% (262144/1048576)"));
    assert_eq!(None, t.feed("READY on COM14: turn the radio OFF, then ON normally, no buttons held. Waiting 600s."));
    let mut nb = ProgressTracker::new();
    assert_eq!(Some(("Prog_Read", 50)), nb.feed("Read current firmware (once, kept in memory): 100%"));
    assert_eq!(Some(("Prog_Read", 100)), nb.feed("Check settings chip: 100%"));
}

#[test]
fn progress_restore_phases() {
    let p = track(&scenario_lines("restore_stock_ok"));
    let mut keys: Vec<&str> = vec![];
    for (k, _) in &p {
        if !keys.contains(k) {
            keys.push(k);
        }
    }
    assert_eq!(vec!["Prog_Write", "Prog_Verify"], keys);
    assert!(track(&scenario_lines("restore_ok")).iter().any(|x| x.0 == "Prog_Verify"));
}
