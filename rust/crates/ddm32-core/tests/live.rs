// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The debug-link readers against the recorded conversations (Fixtures/live-results.json, make_live_fixtures.py; the
//! C# LiveTests): the same frames in the same order, the same printed text, the same summaries; the guard refuses
//! everything outside its list; the live reader stops on cancel. No serial port exists here.
mod common;
use common::*;
use ddm32_core::radio::codec::Codec;
use ddm32_core::radio::debug::{
    self, build, crc16, health_tool, live_tool, DebugLinkPort, HEALTH_ALLOWED, LIVE_ALLOWED,
};
use ddm32_core::radio::live::{self, HealthSummary, LiveSummary};
use ddm32_core::radio::port::{RadioPort, RawSerial, VirtualClock};
use ddm32_core::radio::tools::ToolContext;
use ddm32_core::radio::{ToolError, ToolResult};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::VecDeque;
use std::rc::Rc;
use std::sync::atomic::AtomicBool;
use std::sync::OnceLock;

fn fx() -> &'static Value {
    static V: OnceLock<Value> = OnceLock::new();
    V.get_or_init(|| serde_json::from_slice(&std::fs::read(fixture(&["live-results.json"])).unwrap()).unwrap())
}

/// A recorded conversation: each written frame must be the next recorded one; its answer is queued for reading.
struct Transcript {
    steps: Vec<Value>,
    next: usize,
    pending: VecDeque<u8>,
    opened: bool,
    closes: usize,
    baud: u32,
    failure: Option<String>,
}

impl Transcript {
    fn new(steps: &Value) -> Self {
        Transcript {
            steps: steps.as_array().unwrap().clone(),
            next: 0,
            pending: VecDeque::new(),
            opened: false,
            closes: 0,
            baud: 0,
            failure: None,
        }
    }
}

impl RadioPort for Transcript {
    fn timeout(&self) -> f64 {
        0.0
    }
    fn set_timeout(&mut self, _t: f64) {}
    fn baud_rate(&self) -> u32 {
        self.baud
    }
    fn set_baud_rate(&mut self, b: u32) -> ToolResult<()> {
        self.baud = b;
        Ok(())
    }
    fn write(&mut self, data: &[u8]) -> ToolResult<Option<usize>> {
        let got = hex::encode(data);
        if self.next >= self.steps.len() || self.steps[self.next]["write"] != got.as_str() {
            self.failure = Some(format!("step {}: unexpected frame {got}", self.next));
            return Err(ToolError::new("ReplayMismatch", self.failure.clone().unwrap()));
        }
        self.pending.extend(hex::decode(self.steps[self.next]["reply"].as_str().unwrap()).unwrap());
        self.next += 1;
        Ok(Some(data.len()))
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        let k = n.min(self.pending.len());
        Ok(self.pending.drain(..k).collect())
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        self.pending.clear();
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for Transcript {
    fn open(&mut self) -> ToolResult<()> {
        self.opened = true;
        Ok(())
    }
    fn close(&mut self) -> ToolResult<()> {
        self.closes += 1;
        Ok(())
    }
}

fn case(name: &str) -> &'static Value {
    fx()["cases"].as_array().unwrap().iter().find(|c| c["name"] == name).unwrap()
}

fn names(tool: &str) -> Vec<String> {
    fx()["cases"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|c| c["tool"] == tool)
        .map(|c| c["name"].as_str().unwrap().to_string())
        .collect()
}

fn ctx_run(
    port: Rc<RefCell<Transcript>>,
    f: impl FnOnce(&ToolContext<'_>) -> ToolResult<()>,
) -> (Vec<String>, ToolResult<()>) {
    let lines = Lines::default();
    let print = lines.print();
    let clock = VirtualClock::default();
    let serial = move |_: &str| -> ToolResult<Box<dyn RawSerial>> { Ok(Box::new(Shared(port.clone()))) };
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
    let r = f(&ctx);
    (lines.get(), r)
}

#[test]
fn frames_and_crc_equal_the_pinned_protocol() {
    for f in fx()["frames"].as_array().unwrap() {
        let frame = build(
            f["cmd"].as_u64().unwrap() as u8,
            f["seq"].as_u64().unwrap() as u8,
            &hex::decode(f["payload"].as_str().unwrap()).unwrap(),
        );
        assert_eq!(f["frame"].as_str().unwrap(), hex::encode(frame));
    }
    for c in fx()["crc16"].as_array().unwrap() {
        assert_eq!(c["crc"].as_u64().unwrap() as u16, crc16(&hex::decode(c["data"].as_str().unwrap()).unwrap()));
    }
}

#[test]
fn names_equal_the_python_tables() {
    let strs =
        |k: &str| fx()[k].as_array().unwrap().iter().map(|x| x.as_str().unwrap().to_string()).collect::<Vec<_>>();
    assert_eq!(strs("native_fields"), debug::native_fields().to_vec());
    assert_eq!(strs("native_states"), debug::NATIVE_STATES.to_vec());
    assert_eq!(strs("native_reasons"), debug::NATIVE_REASONS.to_vec());
    assert_eq!(strs("scan_fields"), debug::SCAN_FIELDS.to_vec());
    let mut allowed: Vec<(u8, u8)> = fx()["live_allowed"]
        .as_array()
        .unwrap()
        .iter()
        .map(|x| (x[0].as_u64().unwrap() as u8, hex::decode(x[1].as_str().unwrap()).unwrap()[0]))
        .collect();
    allowed.sort();
    let mut mine = LIVE_ALLOWED.to_vec();
    mine.sort();
    assert_eq!(allowed, mine);
    assert!(HEALTH_ALLOWED.iter().all(|h| LIVE_ALLOWED.contains(h)));
}

fn health_json(h: &HealthSummary) -> Value {
    let mut tasks = serde_json::Map::new();
    for (n, f) in &h.tasks {
        tasks.insert(n.clone(), json!(f));
    }
    json!({"uptime_ms": h.uptime_ms, "heap_free": h.heap_free, "heap_min_free": h.heap_min_free, "min_task": h.min_task,
        "min_free": h.min_free, "crash_valid": h.crash_valid, "crash_built": h.crash_built, "crash_task": h.crash_task,
        "crash_pc": h.crash_pc, "crash_uptime_ms": h.crash_uptime_ms, "reset_cause": h.reset_cause, "tasks": tasks, "ok": h.ok})
}

fn summary_json(s: &LiveSummary) -> Value {
    let g = &s.signal;
    let n = &s.native;
    let scan = s.scan.as_ref().map(|sc| {
        let mut m = serde_json::Map::new();
        for (k, v) in sc {
            m.insert(k.clone(), json!(v));
        }
        Value::Object(m)
    });
    json!({
        "signal": {"ok": g.ok, "rssi_dbm": g.rssi_dbm, "noise": g.noise, "state": g.state, "mode": g.mode, "zone": g.zone,
            "channel": g.channel, "channel_number": g.channel_number, "vfo": g.vfo, "rx_hz": g.rx_hz, "tx_hz": g.tx_hz,
            "squelch": g.squelch, "monitor": g.monitor, "uptime_ms": g.uptime_ms},
        "native": {"ok": n.ok, "status": n.status, "state": n.state, "reason": n.reason, "frequency_hz": n.frequency_hz,
            "rx_count": n.rx_count, "rx_seen": n.rx_seen, "p25_sync": n.p25_sync, "faults": n.faults},
        "scan": scan, "scan_status": s.scan_status,
        "health": s.health.as_ref().map(health_json),
    })
}

/// JSON equality (numbers by value) and the tasks' order.
fn same(expected: &Value, actual: &Value, what: &str) {
    let norm = |v: &Value| serde_json::from_str::<serde_json::Value>(&v.to_string()).unwrap();
    assert_eq!(norm(expected).to_string().len(), norm(actual).to_string().len(), "{what}: {expected}\n{actual}");
    assert_eq!(canon(expected), canon(actual), "{what}");
    for path in [&["health", "tasks"][..], &["tasks"][..]] {
        let e = path.iter().try_fold(expected, |v, k| v.get(*k));
        let a = path.iter().try_fold(actual, |v, k| v.get(*k));
        if let (Some(Value::Object(e)), Some(Value::Object(a))) = (e, a) {
            assert_eq!(e.keys().collect::<Vec<_>>(), a.keys().collect::<Vec<_>>(), "{what}: tasks order");
        }
    }
}

/// A canonical text of a JSON value (object keys sorted, numbers as i64 text).
fn canon(v: &Value) -> String {
    match v {
        Value::Object(m) => {
            let mut keys: Vec<&String> = m.keys().collect();
            keys.sort();
            format!("{{{}}}", keys.iter().map(|k| format!("{k:?}:{}", canon(&m[*k]))).collect::<Vec<_>>().join(","))
        }
        Value::Array(a) => format!("[{}]", a.iter().map(canon).collect::<Vec<_>>().join(",")),
        Value::Number(n) => n.as_i64().map_or(n.to_string(), |i| i.to_string()),
        other => other.to_string(),
    }
}

fn split_polls(lines: &[String]) -> (Vec<String>, Vec<Vec<String>>) {
    let mut once = vec![];
    let mut blocks = vec![];
    let mut block: Option<Vec<String>> = None;
    for l in lines {
        match live::poll_marker(l) {
            Some((k, _)) if k == "poll" => block = Some(vec![]),
            Some(_) => {
                if let Some(b) = block.take() {
                    blocks.push(b);
                }
            }
            None => match &mut block {
                Some(b) => b.push(l.trim().to_string()),
                None => once.push(l.trim().to_string()),
            },
        }
    }
    (once, blocks)
}

#[test]
fn the_live_reader_prints_what_plus_live_prints() {
    let all = names("live");
    assert!(all.len() >= 6);
    for name in all {
        let c = case(&name);
        let port = Rc::new(RefCell::new(Transcript::new(&c["steps"])));
        let count = c["count"].as_i64().unwrap().to_string();
        let cancel = AtomicBool::new(false);
        let (lines, r) = ctx_run(port.clone(), |ctx| {
            live_tool(&argv(&["--port", "COM9", "--interval", "2", "--count", &count]), ctx, &cancel)
        });
        let exit = match r {
            Ok(()) => 0,
            Err(e) => {
                assert_eq!("SystemExit", e.py_type, "{name}: {}", e.record());
                e.message.parse().unwrap()
            }
        };
        let p = port.borrow();
        assert!(p.failure.is_none(), "{name}: {:?}", p.failure);
        assert_eq!(c["exit"].as_i64().unwrap(), exit, "{name}");
        assert_eq!("LIVE: COM9 open, read only, every 2 s", lines[0]);
        assert_eq!(c["output"].as_str().unwrap(), lines[1..].join("\n") + "\n", "{name}");
        assert_eq!(p.steps.len(), p.next, "{name}: frames left");
        assert!(p.opened && p.closes == 1 && p.baud == 115200);
        let (once, blocks) = split_polls(&lines[1..]);
        let expected = c["summaries"].as_array().unwrap();
        assert_eq!(expected.len(), blocks.len(), "{name}");
        for (i, e) in expected.iter().enumerate() {
            let s = live::live_summary(&live::parse_blocks(&blocks[i]), &live::parse_blocks(&once));
            same(e, &summary_json(&s), &format!("{name} poll {i}"));
        }
    }
}

#[test]
fn the_health_reader_prints_what_plus_txdiag_prints() {
    let all = names("health");
    assert!(all.len() >= 5);
    for name in all {
        let c = case(&name);
        let port = Rc::new(RefCell::new(Transcript::new(&c["steps"])));
        let (lines, r) = ctx_run(port.clone(), |ctx| health_tool(&argv(&["--port", "COM9", "--health"]), ctx));
        assert_eq!(c["error"].as_str().map(str::to_string), r.err().map(|e| e.record()), "{name}");
        let text = if lines.is_empty() { String::new() } else { lines.join("\n") + "\n" };
        assert_eq!(c["output"].as_str().unwrap(), text, "{name}");
        let p = port.borrow();
        assert_eq!(p.steps.len(), p.next, "{name}");
        assert_eq!(1, p.closes);
        let s = live::health_summary(&lines);
        same(&c["summary"], &health_json(&s), &name);
        if let Some(v) = c["verdict"].as_str() {
            assert_eq!(v, format!("{:?}", live::verdict(&s)).to_lowercase(), "{name}");
        }
    }
}

/// A fake radio that answers every request with a small view, counting what reaches it.
#[derive(Default)]
struct FakeDebug {
    frames: Vec<Vec<u8>>,
    pending: VecDeque<u8>,
    closes: usize,
}

impl RadioPort for FakeDebug {
    fn timeout(&self) -> f64 {
        0.0
    }
    fn set_timeout(&mut self, _t: f64) {}
    fn baud_rate(&self) -> u32 {
        115200
    }
    fn set_baud_rate(&mut self, _b: u32) -> ToolResult<()> {
        Ok(())
    }
    fn write(&mut self, d: &[u8]) -> ToolResult<Option<usize>> {
        self.frames.push(d.to_vec());
        let mut body = vec![0, 0x50, 0x40, 0x01, 0x00, 5];
        body.extend_from_slice(b"state=rx\nrssi_dbm=-90\n");
        self.pending.extend(build(d[2] | 0x80, d[3], &body));
        Ok(Some(d.len()))
    }
    fn read(&mut self, n: usize) -> ToolResult<Vec<u8>> {
        let k = n.min(self.pending.len());
        Ok(self.pending.drain(..k).collect())
    }
    fn reset_input_buffer(&mut self) -> ToolResult<()> {
        Ok(())
    }
    fn flush(&mut self) -> ToolResult<()> {
        Ok(())
    }
}

impl RawSerial for FakeDebug {
    fn open(&mut self) -> ToolResult<()> {
        Ok(())
    }
    fn close(&mut self) -> ToolResult<()> {
        self.closes += 1;
        Ok(())
    }
}

#[test]
fn the_guard_refuses_everything_outside_its_list() {
    let radio = Rc::new(RefCell::new(FakeDebug::default()));
    let mut guard = DebugLinkPort::new(Box::new(Shared(radio.clone())), &LIVE_ALLOWED, "plus_live");
    let mut cut = build(0x09, 1, &[0x00]);
    cut.pop();
    let refused: Vec<Vec<u8>> = vec![
        build(0x06, 1, &[]),
        build(0x07, 1, &[1]),
        build(0x0A, 1, &[1]),
        build(0x21, 1, &[1]),
        build(0x21, 1, &[0x0A, 1]),
        build(0x09, 1, &[0x09]),
        build(0x0F, 1, &[2]),
        build(0x0D, 1, &[]),
        build(0x20, 1, &[0x0D]),
        build(0x09, 1, &[0, 0]),
        vec![0xA5, 0x5A, 0x09, 1, 1, 0, 0x00, 0, 0],
        vec![0x09, 0x00],
        vec![],
        cut,
    ];
    for f in &refused {
        assert!(guard.write(f).is_err(), "{f:02x?}");
    }
    assert!(radio.borrow().frames.is_empty());
    assert_eq!(refused.len(), guard.refusals.len());
    assert!(guard.set_baud_rate(9600).is_err());
    assert!(guard.write(&build(0x09, 1, &[0x14, 0x00])).is_err());
    guard.write(&build(0x09, 1, &[0x00])).unwrap();
    assert_eq!(1, radio.borrow().frames.len());
    let mut health = DebugLinkPort::new(Box::new(Shared(radio.clone())), &HEALTH_ALLOWED, "plus_txdiag");
    assert!(health.write(&build(0x09, 2, &[0x00])).is_err());
    assert!(health.write(&build(0x21, 2, &[0x00])).is_err());
    health.write(&build(0x09, 2, &[0x14])).unwrap();
}

#[test]
fn live_stops_on_cancel_and_closes_the_port() {
    let radio = Rc::new(RefCell::new(FakeDebug::default()));
    let cancel = AtomicBool::new(false);
    let polls = RefCell::new(0);
    let print = |s: &str| {
        if s.starts_with("=== end ") {
            *polls.borrow_mut() += 1;
            if *polls.borrow() == 3 {
                cancel.store(true, std::sync::atomic::Ordering::SeqCst);
            }
        }
    };
    let clock = VirtualClock::default();
    let r2 = radio.clone();
    let serial = move |_: &str| -> ToolResult<Box<dyn RawSerial>> { Ok(Box::new(Shared(r2.clone()))) };
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
    live_tool(&argv(&["--port", "COM9", "--interval", "1"]), &ctx, &cancel).unwrap();
    assert_eq!(3, *polls.borrow());
    let r = radio.borrow();
    assert_eq!(1, r.closes);
    assert_eq!(2 + 3 * 4, r.frames.len());
    assert!(r.frames.iter().all(|f| LIVE_ALLOWED.contains(&(f[2], f[6]))));
}

#[test]
fn refused_arguments_and_ports_open_nothing() {
    let opened = RefCell::new(0);
    let serial = |_: &str| -> ToolResult<Box<dyn RawSerial>> {
        *opened.borrow_mut() += 1;
        Ok(Box::new(FakeDebug::default()))
    };
    let clock = VirtualClock::default();
    let print = |_: &str| {};
    let ctx =
        ToolContext { print: &print, clock: &clock, serial: &serial, release_key_hex: None, codec: Codec::dm32() };
    let c = AtomicBool::new(false);
    assert_eq!("SystemExit", live_tool(&argv(&["--port", "COM9", "--interval", "9"]), &ctx, &c).unwrap_err().py_type);
    assert_eq!("SystemExit", live_tool(&argv(&["--port", "COM9", "--count", "-1"]), &ctx, &c).unwrap_err().py_type);
    assert_eq!("SystemExit", live_tool(&argv(&["--interval", "2"]), &ctx, &c).unwrap_err().py_type);
    assert_eq!("SystemExit", health_tool(&argv(&["--port", "COM9", "--write"]), &ctx).unwrap_err().py_type);
    assert_eq!("ValueError", health_tool(&argv(&["--port", "udp:10.0.0.1"]), &ctx).unwrap_err().py_type);
    assert_eq!("ValueError", live_tool(&argv(&["--port", "rfc2217://host:1"]), &ctx, &c).unwrap_err().py_type);
    assert_eq!(0, *opened.borrow());
    let none = ToolContext {
        print: &print,
        clock: &clock,
        serial: &ddm32_core::radio::port::no_port,
        release_key_hex: None,
        codec: Codec::dm32(),
    };
    assert_eq!("NoPortGiven", health_tool(&argv(&["--port", "COM9"]), &none).unwrap_err().py_type);
}

fn argv(a: &[&str]) -> Vec<String> {
    a.iter().map(|s| s.to_string()).collect()
}

#[test]
fn presentation_helpers_equal_python() {
    for x in fx()["s_meter"].as_array().unwrap() {
        assert_eq!(x[1].as_str().unwrap(), live::s_meter(x[0].as_i64()), "{x}");
    }
    for x in fx()["format_mhz"].as_array().unwrap() {
        assert_eq!(x[1].as_str().unwrap(), live::format_mhz(x[0].as_i64(), true), "{x}");
    }
    for x in fx()["format_uptime"].as_array().unwrap() {
        assert_eq!(x[1].as_str().unwrap(), live::format_uptime(x[0].as_i64()), "{x}");
    }
    assert_eq!("434.25000 MHz", live::format_mhz(Some(434_250_000), false));
    assert_eq!(0, live::dbm_percent(None));
    assert_eq!(0, live::dbm_percent(Some(-150)));
    assert_eq!(53, live::dbm_percent(Some(-87)));
    assert_eq!(100, live::dbm_percent(Some(-10)));
    assert_eq!("1.200 B", live::bytes(Some(1200), true));
    assert_eq!("1,200 B", live::bytes(Some(1200), false));
    assert_eq!("-", live::bytes(None, true));
    assert_eq!(Some(26), live::py_int0("0x1a"));
    assert_eq!(Some(-87), live::py_int0("-87"));
    assert_eq!(None, live::py_int0("012"));
    assert_eq!(Some(0), live::py_int0("00"));
    assert_eq!(None, live::py_int0("abc"));
}
