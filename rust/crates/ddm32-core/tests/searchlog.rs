// SPDX-License-Identifier: GPL-3.0-or-later
//! The search log (radio::searchlog), held to the C# SearchLogTests: the firmware's p25s / dmrs / alls sections become
//! rows.
use ddm32_core::app_info;
use ddm32_core::radio::debug::SEARCHLOG_ALLOWED;
use ddm32_core::radio::searchlog::{self, parse_all, parse_dmr, parse_p25, SearchLogBook, Stamp, CSV_HEADER};

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
    assert_eq!("2026-11-21 22:00:00,P25,433.5000,$1C8,,655,1234567,0,1,", lines[1]);
}

#[test]
fn the_reader_sends_only_its_three_views() {
    assert_eq!([(0x09, 28), (0x09, 29), (0x09, 30)], SEARCHLOG_ALLOWED);
    assert!(app_info::RESERVED_NAMES.contains(&app_info::LOGS_FOLDER));
}

#[test]
fn all_digital_fields_are_parsed() {
    let p = parse_all("433500000,P25,456,0,0,1,0,655,1234567,3,10,", T0).unwrap();
    assert_eq!(
        ("P25", 433500000, "$1C8", "", 655, 1234567, 3, ""),
        (p.mode, p.hz, p.code.as_str(), p.slot.as_str(), p.group, p.source, p.calls, p.caller.as_str())
    );
    assert_eq!(Stamp(T0.0 - 10), p.time);
    let d = parse_all("438612500,DMR,1,2,1,1,0,2620,2620123,4,0,", T0).unwrap();
    assert_eq!(("DMR", "CC1", "TS2 RPT", 2620, 2620123), (d.mode, d.code.as_str(), d.slot.as_str(), d.group, d.source));
    let y = parse_all("439100000,YSF,0,0,0,0,0,0,0,2,5,TA2DOU/P", T0).unwrap();
    assert_eq!(
        ("YSF", "DG0", "", 0, 0, 2, "TA2DOU/P"),
        (y.mode, y.code.as_str(), y.slot.as_str(), y.group, y.source, y.calls, y.caller.as_str())
    );
    let n = parse_all("438800000,NXDN,5,0,0,1,1,101,1234,1,0,", T0).unwrap();
    assert_eq!(("NXDN", "RAN5", 101, 1234, true), (n.mode, n.code.as_str(), n.group, n.source, n.encrypted));
    assert_eq!("RAN?", parse_all("438800000,NXDN,65535,0,0,0,0,0,0,0,0,", T0).unwrap().code);
    assert_eq!("$?", parse_all("438800000,P25,65535,0,0,0,0,0,0,0,0,", T0).unwrap().code);
    assert_eq!("", parse_all("438800000,YSF,0,0,0,0,0,0,0,0,0", T0).unwrap().caller);
    assert!(parse_all("438800000,FM,0,0,0,0,0,0,0,0,0,", T0).is_none());
    assert!(parse_all("438800000,P25,1,2", T0).is_none());
    assert!(parse_all("0,P25,0,0,0,0,0,0,0,0,0,", T0).is_none());
}

#[test]
fn all_digital_calls_are_their_own_rows() {
    let mut b = SearchLogBook::new();
    let first = [
        "p25s.on=0",
        "p25s.f0=433500000,456,655,1234567,2,30",
        "alls.on=1",
        "alls.mode=P25",
        "alls.at=433512500",
        "alls.n=2",
        "alls.f0=433500000,P25,456,0,0,1,0,655,1234567,1,0,",
        "alls.f1=439100000,YSF,0,0,0,0,0,0,0,1,4,TA2DOU",
    ];
    let r = b.add(&first, T0);
    assert_eq!(3, r.len());
    assert_eq!(Some(true), b.all_on);
    assert_eq!(Some(false), b.p25_on);
    assert_eq!(Some(433512500), b.all_at_hz);
    assert_eq!("TA2DOU", r.iter().find(|c| c.mode == "YSF").unwrap().caller);
    let same = [
        "alls.f0=433500000,P25,456,0,0,1,0,655,1234567,1,1,",
        "alls.f1=439100000,YSF,0,0,0,0,0,0,0,1,5,TA2DOU",
        "p25s.f0=433500000,456,655,1234567,2,31",
    ];
    assert!(b.add(&same, Stamp(T0.0 + 1)).is_empty());
    let r = b.add(&["alls.f0=439100000,YSF,0,0,0,0,0,0,0,1,0,YM2KDZ"], Stamp(T0.0 + 2));
    assert_eq!(1, r.len());
    assert_eq!("YM2KDZ", r[0].caller);
    let dir = tempfile::tempdir().unwrap();
    let path = searchlog::append(dir.path(), &r, T0).unwrap();
    let text = std::fs::read_to_string(&path).unwrap();
    assert!(text.lines().nth(1).unwrap().ends_with(",YSF,439.1000,DG0,,0,0,0,1,YM2KDZ"));
}
