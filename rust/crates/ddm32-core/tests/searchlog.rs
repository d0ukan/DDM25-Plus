// SPDX-License-Identifier: GPL-3.0-or-later
//! The search log (radio::searchlog), held to the C# SearchLogTests: the firmware's p25s / dmrs sections become rows.
use ddm32_core::app_info;
use ddm32_core::radio::debug::SEARCHLOG_ALLOWED;
use ddm32_core::radio::searchlog::{self, parse_dmr, parse_p25, SearchLogBook, Stamp, CSV_HEADER};

// 2026-11-21 22:00:00 local
const T0: Stamp = Stamp(1_795_298_400);

fn poll(p25: &str, dmr: &str) -> Vec<String> {
    [
        "section=p25s",
        "p25s.on=1",
        "p25s.phase=1",
        "p25s.at=433512500",
        "p25s.n=1",
        p25,
        "section=dmrs",
        "dmrs.on=1",
        "dmrs.phase=1",
        "dmrs.at=438000000",
        "dmrs.n=1",
        dmr,
    ]
    .iter()
    .map(|s| s.to_string())
    .collect()
}

#[test]
fn fields_are_parsed() {
    assert_eq!("2026-11-21 22:00:00", searchlog::format_time(T0));
    let p = parse_p25("433500000,456,655,1234567,3,10", T0).unwrap();
    assert_eq!(
        ("P25", 433500000, "$1C8", 655, 1234567, 3),
        (p.mode, p.hz, p.code.as_str(), p.group, p.source, p.calls)
    );
    assert_eq!(Stamp(T0.0 - 10), p.time);
    let d = parse_dmr("438612500,1,2,1,1,0,2620,2620123,4,0", T0).unwrap();
    assert_eq!(
        ("DMR", "CC1", "TS2 RPT", 2620, 2620123, false),
        (d.mode, d.code.as_str(), d.slot.as_str(), d.group, d.source, d.encrypted)
    );
    assert_eq!("CC?", parse_dmr("438612500,255,0,2,1,1,0,0,0,0", T0).unwrap().code);
    assert!(parse_p25("garbage", T0).is_none());
    assert!(parse_dmr("1,2,3", T0).is_none());
}

#[test]
fn only_new_calls_become_rows() {
    let mut b = SearchLogBook::new();
    let r = b.add(&poll("p25s.f0=433500000,456,655,1234567,1,30", "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,5"), T0);
    assert_eq!(2, r.len());
    assert_eq!("P25", r[0].mode);
    assert_eq!(Some(true), b.p25_on);
    assert_eq!(Some(433512500), b.p25_at_hz);
    let same = poll("p25s.f0=433500000,456,655,1234567,1,31", "dmrs.f0=438612500,1,1,1,1,0,2620,2620123,2,6");
    assert!(b.add(&same, Stamp(T0.0 + 1)).is_empty());
    let r = b.add(
        &poll("p25s.f0=433500000,456,655,7654321,2,0", "dmrs.f0=438612500,1,1,1,1,0,2620,2620999,2,0"),
        Stamp(T0.0 + 2),
    );
    assert_eq!(2, r.len());
    assert_eq!(4, b.calls().len());
    assert_eq!(2, b.frequencies());
    assert!(b.add(&poll("p25s.f0=433500000,456,655,7654321,1,0", ""), Stamp(T0.0 + 3)).is_empty());
    assert!(b.add::<String>(&[], Stamp(T0.0 + 4)).is_empty());
    assert_eq!(None, b.dmr_on);
}

#[test]
fn the_days_csv_gets_one_header() {
    let dir = tempfile::tempdir().unwrap();
    let mut b = SearchLogBook::new();
    let rows = b.add(&poll("p25s.f0=433500000,456,655,1234567,1,0", ""), T0);
    let path = searchlog::append(dir.path(), &rows, T0).unwrap();
    searchlog::append(dir.path(), &rows, T0).unwrap();
    let text = std::fs::read_to_string(&path).unwrap();
    let lines: Vec<&str> = text.lines().collect();
    assert_eq!(dir.path().join("search-20261121.csv"), path);
    assert_eq!(CSV_HEADER, lines[0]);
    assert_eq!(3, lines.len());
    assert_eq!("2026-11-21 22:00:00,P25,433.5000,$1C8,,655,1234567,0,1", lines[1]);
}

#[test]
fn the_reader_sends_only_its_two_views() {
    assert_eq!([(0x09, 28), (0x09, 29)], SEARCHLOG_ALLOWED);
    assert!(app_info::RESERVED_NAMES.contains(&app_info::LOGS_FOLDER));
}
