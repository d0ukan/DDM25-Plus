// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The codeplug viewer against the Python viewer on synthetic images (Fixtures/codeplug-results.json.gz,
//! make_codeplug_fixtures.py; the C# CodeplugTests): every row, zone, contact, list, count and CSV equal.
mod common;
use common::*;
use ddm32_core::codeplug::{self as cp, Codeplug, Table, EN, TR};
use serde_json::Value;
use std::io::Read;
use std::sync::OnceLock;

fn fx() -> &'static Value {
    static V: OnceLock<Value> = OnceLock::new();
    V.get_or_init(|| {
        let f = std::fs::File::open(fixture(&["codeplug-results.json.gz"])).unwrap();
        let mut s = String::new();
        flate2::read::GzDecoder::new(f).read_to_string(&mut s).unwrap();
        serde_json::from_str(&s).unwrap()
    })
}

fn image(name: &str) -> Vec<u8> {
    let mut img = vec![0xFFu8; cp::IMAGE_SIZE];
    for run in fx()["images"][name]["sparse"].as_array().unwrap() {
        let a = run[0].as_u64().unwrap() as usize;
        let data = b64(run[1].as_str().unwrap());
        img[a..a + data.len()].copy_from_slice(&data);
    }
    img
}

fn text(v: &Value) -> String {
    match v {
        Value::Null => String::new(),
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

#[test]
fn decodes_as_the_python_viewer() {
    for name in fx()["images"].as_object().unwrap().keys() {
        let e = &fx()["images"][name]["expected"];
        let c = Codeplug::decode(&image(name), &TR).unwrap();
        let counts = &e["counts"];
        assert_eq!(counts["channels"].as_u64().unwrap() as usize, c.channels.len(), "{name}");
        assert_eq!(counts["zones"].as_u64().unwrap() as usize, c.zones.len(), "{name}");
        assert_eq!(counts["contacts"].as_u64().unwrap() as usize, c.contacts.len(), "{name}");
        assert_eq!(counts["rx_group_lists"].as_u64().unwrap() as usize, c.lists.len(), "{name}");
        let mc: std::collections::HashMap<String, usize> = e["mode_counts"]
            .as_object()
            .unwrap()
            .iter()
            .map(|(k, v)| (k.clone(), v.as_u64().unwrap() as usize))
            .collect();
        assert_eq!(mc, c.mode_counts(), "{name}");
        assert_eq!(e["bad_records"].as_u64().unwrap() as usize, c.bad_records);
        assert_eq!(e["rfkv_entries"].as_u64().unwrap() as usize, c.rfkv_entries, "{name}");
        assert_eq!(e["zone_capacity"].as_u64().unwrap() as usize, c.zone_capacity);
        assert_eq!(e["radio_name"].as_str().unwrap(), c.radio_name);
        assert_eq!(e["dmr_id"].as_i64().unwrap(), c.dmr_id);
        for (i, want) in e["channels"].as_array().unwrap().iter().enumerate() {
            let got = &c.channels[i];
            assert_eq!(want["number"].as_i64().unwrap(), got.number);
            assert_eq!(want["mode"].as_str().unwrap(), got.mode);
            for (key, _) in TR.channel_columns {
                assert_eq!(text(&want[*key]), got.get(key), "{name} channel {} {key}", got.number);
            }
        }
        for (i, z) in e["zones"].as_array().unwrap().iter().enumerate() {
            let g = &c.zones[i];
            assert_eq!(
                (
                    z["index"].as_i64().unwrap(),
                    z["slot"].as_i64().unwrap(),
                    z["name"].as_str().unwrap(),
                    z["count"].as_u64().unwrap() as usize
                ),
                (g.index, g.slot, g.name.as_str(), g.channels.len())
            );
            assert_eq!(
                z["channels"].as_array().unwrap().iter().map(|x| x.as_i64().unwrap()).collect::<Vec<_>>(),
                g.channels
            );
            assert_eq!(z["channel_names"].as_str().unwrap(), g.channel_names);
        }
        for (i, x) in e["contacts"].as_array().unwrap().iter().enumerate() {
            let g = &c.contacts[i];
            assert_eq!(
                (
                    x["index"].as_i64().unwrap(),
                    x["name"].as_str().unwrap(),
                    x["id"].as_i64().unwrap(),
                    x["type"].as_str().unwrap(),
                    x["ts"].as_str().unwrap()
                ),
                (g.index, g.name.as_str(), g.id, g.kind.as_str(), g.ts.as_str())
            );
        }
        for (i, l) in e["lists"].as_array().unwrap().iter().enumerate() {
            let g = &c.lists[i];
            assert_eq!(
                (
                    l["index"].as_i64().unwrap(),
                    l["name"].as_str().unwrap(),
                    l["count"].as_u64().unwrap() as usize,
                    l["contact_names"].as_str().unwrap()
                ),
                (g.index, g.name.as_str(), g.contacts.len(), g.contact_names.as_str())
            );
            assert_eq!(
                l["contacts"].as_array().unwrap().iter().map(|x| x.as_i64().unwrap()).collect::<Vec<_>>(),
                g.contacts
            );
        }
        for (t, key) in [
            (Table::Channels, "channels"),
            (Table::Zones, "zones"),
            (Table::Contacts, "contacts"),
            (Table::Lists, "lists"),
        ] {
            assert_eq!(e["csv"][key].as_str().unwrap(), cp::to_csv(c.columns(t), &c.rows(t)), "{name} {key} csv");
        }
    }
}

#[test]
fn column_titles_and_helpers_equal_python() {
    let cols = &fx()["columns"];
    for (key, mine) in [
        ("channels", TR.channel_columns),
        ("zones", TR.zone_columns),
        ("contacts", TR.contact_columns),
        ("lists", TR.list_columns),
    ] {
        let want: Vec<(String, String)> = cols[key]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| (x[0].as_str().unwrap().into(), x[1].as_str().unwrap().into()))
            .collect();
        assert_eq!(want, mine.iter().map(|(a, b)| (a.to_string(), b.to_string())).collect::<Vec<_>>());
    }
    assert_eq!(
        TR.channel_columns.iter().map(|c| c.0).collect::<Vec<_>>(),
        EN.channel_columns.iter().map(|c| c.0).collect::<Vec<_>>()
    );
    for x in fx()["name_check"].as_array().unwrap() {
        let mut raw = [0xFFu8; 16];
        let b: Vec<u8> = x[0].as_str().unwrap().chars().map(|c| c as u32 as u8).collect();
        let n = b.len().min(16);
        raw[..n].copy_from_slice(&b[..n]);
        assert_eq!(x[1].as_i64().unwrap(), cp::name_check(&raw));
    }
    for x in fx()["tones"].as_array().unwrap() {
        assert_eq!(x[1].as_str().unwrap(), cp::tone(x[0].as_i64().unwrap()));
    }
}

#[test]
fn nac_in_the_firmwares_decimal_form() {
    assert_eq!("659 ($293)", TR.nac(0x293));
    assert_eq!("1 ($001)", EN.nac(1));
    assert_eq!("4095 ($FFF)", EN.nac(0xFFF));
    assert_eq!("Herhangi 3966 ($F7E)", TR.nac(0xF7E));
    assert_eq!("Any 3966 ($F7E)", EN.nac(0xF7E));
}

#[test]
fn english_words() {
    let c = Codeplug::decode(&image("sample"), &EN).unwrap();
    let ch = |n: i64| c.channels.iter().find(|x| x.number == n).unwrap();
    assert_eq!("NAC 659 ($293) · TG 101", ch(4).get("summary"));
    assert_eq!("Same as RX", ch(4).get("tx_nac"));
    assert!(ch(5).get("nac").starts_with("Any 3966"));
    assert_eq!("Yes", ch(5).get("rx_only"));
    assert_eq!("CC Any · TS Any · all TGs · RX only", ch(3).get("summary"));
    assert_eq!("Tone RX 67.0 / TX D023I", ch(1).get("summary"));
    assert_eq!("Default", ch(3).get("power"));
    assert_eq!("RAN any", ch(7).get("code"));
    assert_eq!(vec!["Group", "Private", "All call"], c.contacts.iter().map(|x| x.kind.as_str()).collect::<Vec<_>>());
    let p = Codeplug::decode(&image("p25"), &EN).unwrap();
    let pc = |n: i64| p.channels.iter().find(|x| x.number == n).unwrap();
    assert_eq!("101 (all groups)", pc(13).get("tg"));
    assert_eq!("contact 3", pc(15).get("tg"));
    assert_eq!("Unknown (RFKV kind 3)", pc(18).mode);
    assert_eq!("Unknown (0x03)", pc(38).mode);
    assert!(pc(32).get("nac").ends_with("(CH_MODE 2 form)"));
}

#[test]
fn a_full_codeplug_has_68_zones_of_80() {
    let c = Codeplug::decode(&image("full"), &TR).unwrap();
    assert_eq!(cp::MAX_CHANNELS, c.channels.len());
    assert_eq!(cp::ZONE_SLOTS, c.zones.len());
    assert!(c.zones.iter().all(|z| z.channels.len() == 80));
    let z68 = c.zones.last().unwrap();
    assert_eq!("Zone 68", z68.name);
    assert_eq!(z68.channels, c.zone_channels(z68).iter().map(|x| x.number).collect::<Vec<_>>());
    assert_eq!(6, c.mode_counts().len());
}

#[test]
fn search_matches_any_column_ignoring_case() {
    let c = Codeplug::decode(&image("sample"), &TR).unwrap();
    let rows = c.rows(Table::Channels);
    assert_eq!(rows.len(), rows.iter().filter(|r| cp::matches(r, "")).count());
    assert_eq!(rows.len(), rows.iter().filter(|r| cp::matches(r, "   ")).count());
    let p25: Vec<String> = rows.iter().filter(|r| cp::matches(r, "p25")).map(|r| r["number"].clone()).collect();
    assert_eq!(vec!["4", "5"], p25);
    for s in ["659", "$293", "d023i", "430.9"] {
        assert_eq!(1, rows.iter().filter(|r| cp::matches(r, s)).count(), "{s}");
    }
    assert!(!rows.iter().any(|r| cp::matches(r, "no such text")));
    assert_eq!(1, c.rows(Table::Zones).iter().filter(|r| cp::matches(r, "bank1")).count());
}

#[test]
fn csv_quotes_only_when_needed_and_keeps_a_bom() {
    let cols = [("a", "A"), ("b", "B,1")];
    let mut r1 = std::collections::HashMap::new();
    r1.insert("a".to_string(), "x\"y".to_string());
    r1.insert("b".to_string(), "1, 2".to_string());
    let mut r2 = std::collections::HashMap::new();
    r2.insert("a".to_string(), "plain".to_string());
    assert_eq!("A,\"B,1\"\n\"x\"\"y\",\"1, 2\"\nplain,\n", cp::to_csv(&cols, &[r1, r2]));
    let t = tmp();
    let p = t.path().join("t.csv");
    cp::write_csv(&p, "No,Ad\n1,Kanal ğüşİ\n").unwrap();
    let b = std::fs::read(&p).unwrap();
    assert_eq!([0xEF, 0xBB, 0xBF], b[..3]);
    assert_eq!("No,Ad\n1,Kanal ğüşİ\n", String::from_utf8(b[3..].to_vec()).unwrap());
}

#[test]
fn wrong_size_is_refused() {
    assert!(Codeplug::decode(&[0u8; 1024], &TR).is_err());
    let t = tmp();
    std::fs::write(t.path().join("q128-pass1.bin"), [0u8; 4096]).unwrap();
    assert!(Codeplug::load_image(t.path()).is_err());
    std::fs::write(t.path().join("q128-pass1.bin"), image("zone16")).unwrap();
    assert_eq!(16, Codeplug::decode(&Codeplug::load_image(t.path()).unwrap(), &TR).unwrap().zone_capacity);
}
