// SPDX-License-Identifier: GPL-3.0-or-later
//! The versions the window shows: the full version with its prerelease and label on Home, and the order of every list
//! the C# order (ReleaseVersion: a prerelease before its release, numeric parts as numbers, a number before text).
use ddm32_core::releases::catalog::Catalog;
use ddm32_core::releases::{signed_manifest as sm, CURRENT};
use ddm32_gui::{offered, version_line};
use serde_json::json;

fn entry(version: &str, channel: &str) -> serde_json::Value {
    let app = format!("app {version}").into_bytes();
    let name = format!("ddm32-plus-{version}.zip");
    json!({
        "version": version, "channel": channel, "file": name,
        "url": format!("{}v{version}/{name}", CURRENT.url_prefix()),
        "sha256": sm::sha256_hex(b"zip"), "size": 3,
        "application_sha256": sm::sha256_hex(&app), "application_size": app.len(),
        "settings_format": 1, "codeplug_format": 1, "min_compatible_version": version,
        "notes": {"tr": "", "en": ""},
    })
}

fn catalog(versions: &[(&str, &str)]) -> Catalog {
    let secret: Vec<u8> = (0..32u8).collect();
    let key = hex::encode(ddm32_core::crypto::ed25519::secret_to_public(&secret));
    let mut cat = Catalog::default();
    for (v, ch) in versions {
        let m = json!({"format": CURRENT.format, "repo": CURRENT.repo, "release": format!("v{v}"),
            "created_utc": "2026-11-21T00:00:00Z", "firmware": [entry(v, ch)]});
        let data = serde_json::to_vec_pretty(&m).unwrap();
        let sig = sm::sign_bytes(&data, &secret);
        cat.add(&format!("v{v}"), &sm::verify_bytes(&data, sig.as_bytes(), Some(&key), None).unwrap(), None);
    }
    cat.sort();
    cat
}

#[test]
fn home_names_the_full_version_and_its_label() {
    let cat = catalog(&[("1.0.0-beta.5", "beta"), ("0.9.0", "stable")]);
    let beta = offered(&cat, 1);
    assert_eq!("1.0.0-beta.5 (Beta)", version_line(&beta[0], &cat.firmware, "en"));
    assert_eq!("0.9.0 (Latest)", version_line(&beta[1], &cat.firmware, "en"));
    assert_eq!("0.9.0 (En yeni)", version_line(&offered(&cat, 0)[0], &cat.firmware, "tr"));
    let test = catalog(&[("1.1.0-alpha.1", "beta")]);
    assert_eq!("1.1.0-alpha.1 (Test / Alpha)", version_line(&test.firmware[0], &test.firmware, "en"));
}

#[test]
fn every_list_is_in_the_csharp_order() {
    let cat = catalog(&[
        ("1.0.0", "stable"),
        ("1.0.0-beta.2", "beta"),
        ("1.0.0-beta.10", "beta"),
        ("1.0.0-rc.1", "beta"),
        ("1.1.0-alpha.1", "beta"),
        ("0.9.0", "stable"),
        ("1.0.0-beta.10.1", "beta"),
    ]);
    let names = |ch: i32| offered(&cat, ch).iter().map(|f| f.version().to_string()).collect::<Vec<_>>();
    assert_eq!(vec!["1.0.0", "0.9.0"], names(0));
    assert_eq!(vec!["1.0.0", "1.0.0-rc.1", "1.0.0-beta.10.1", "1.0.0-beta.10", "1.0.0-beta.2", "0.9.0"], names(1));
    assert_eq!(
        vec!["1.1.0-alpha.1", "1.0.0", "1.0.0-rc.1", "1.0.0-beta.10.1", "1.0.0-beta.10", "1.0.0-beta.2", "0.9.0"],
        names(2)
    );
}
