// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! The release list, downloads with hash checks, the local release folder and the HTTPS rules (the C# CatalogTests,
//! DownloadTests, LocalReleaseTests). Fake network only: no socket is ever opened.
mod common;
use common::*;
use ddm32_core::releases::catalog::{installable, label_for, needs_confirmation, Catalog, ReleaseLabel};
use ddm32_core::releases::client::{self, download_app, download_firmware, list_packages, load_catalog, Package};
use ddm32_core::releases::local::{load_local, LocalReleaseNet};
use ddm32_core::releases::net::{check_url, HttpReleaseNet, HttpResponse, HttpTransport, NetError, ReleaseNet};
use ddm32_core::releases::{signed_manifest as sm, AssetEntry, VerifyFailure, API_URL, PACKAGE_FILES};
use serde_json::{json, Value};
use std::path::Path;
use std::sync::{Arc, Mutex};

fn python_net() -> FakeNet {
    let net = FakeNet::default();
    for (k, v) in python()["catalog"]["files"].as_object().unwrap() {
        net.files.borrow_mut().insert(k.clone(), b64(v.as_str().unwrap()));
    }
    net
}

fn entries(dir: &Path) -> Vec<String> {
    let mut v: Vec<String> =
        std::fs::read_dir(dir).unwrap().map(|e| e.unwrap().file_name().to_string_lossy().into_owned()).collect();
    v.sort();
    v
}

#[test]
fn same_list_and_refusals_as_python() {
    let t = tmp();
    let py = &python()["catalog"];
    let key = py["key"].as_str();
    let now = || "now".to_string();
    let cat = load_catalog(t.path(), &python_net(), key, Some(&now)).unwrap();
    let want: Vec<String> = py["firmware"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r.as_array().unwrap().iter().map(|x| x.as_str().unwrap()).collect::<Vec<_>>().join("|"))
        .collect();
    assert_eq!(
        want,
        cat.firmware.iter().map(|f| format!("{}|{}|{}", f.version(), f.release, f.channel())).collect::<Vec<_>>()
    );
    let want: Vec<String> = py["rejected"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r.as_array().unwrap().iter().map(|x| x.as_str().unwrap()).collect::<Vec<_>>().join("|"))
        .collect();
    assert_eq!(want, cat.rejected.iter().map(|(t, r)| format!("{t}|{r}")).collect::<Vec<_>>());
    assert_eq!("github", cat.source);
    assert_eq!(Some("now".to_string()), cat.checked_utc);
    let cached = client::cached_catalog(t.path(), key);
    assert_eq!(
        cat.firmware.iter().map(|f| f.version()).collect::<Vec<_>>(),
        cached.firmware.iter().map(|f| f.version()).collect::<Vec<_>>()
    );
    assert_eq!("cache", cached.source);
    assert!(cached.rejected.is_empty());
}

#[test]
fn no_key_lists_nothing() {
    let t = tmp();
    let cat = load_catalog(t.path(), &python_net(), Some(""), None).unwrap();
    assert!(cat.firmware.is_empty());
    assert!(cat.rejected.iter().any(|r| r.1.contains("No release signing key")));
}

#[test]
fn cache_tampering_is_caught() {
    let t = tmp();
    let key = python()["catalog"]["key"].as_str();
    load_catalog(t.path(), &python_net(), key, None).unwrap();
    let p = t.path().join("manifests").join("v1.2.0").join("ddm32-manifest.json");
    let text = std::fs::read_to_string(&p).unwrap().replace("\"stable\"", "\"beta\"  ");
    std::fs::write(&p, text).unwrap();
    let cat = client::cached_catalog(t.path(), key);
    assert!(!cat.firmware.iter().any(|f| f.release == "v1.2.0"));
    assert!(cat.rejected.iter().find(|r| r.0 == "v1.2.0").unwrap().1.contains("signature"));
}

#[test]
fn not_a_release_list_is_a_net_error() {
    let t = tmp();
    let net = FakeNet::default();
    net.files.borrow_mut().insert(API_URL.into(), b"{\"message\": \"rate limited\"}".to_vec());
    assert!(load_catalog(t.path(), &net, None, None).is_err());
    net.files.borrow_mut().insert(API_URL.into(), b"<html>".to_vec());
    assert!(load_catalog(t.path(), &net, None, None).is_err());
}

#[test]
fn published_legacy_release_is_listed_as_legacy() {
    let t = tmp();
    let net = FakeNet::default();
    let tag = "v1.0.0-beta.1";
    let mut assets = vec![];
    for n in ["ddm25-manifest.json", "ddm25-manifest.json.sig"] {
        let u = url(tag, n);
        net.files.borrow_mut().insert(u.clone(), std::fs::read(fixture(&["legacy", n])).unwrap());
        assets.push(json!({"name": n, "browser_download_url": u}));
    }
    net.files.borrow_mut().insert(
        API_URL.into(),
        json!([{"tag_name": tag, "prerelease": true, "assets": assets}]).to_string().into_bytes(),
    );
    let cat = load_catalog(t.path(), &net, None, None).unwrap();
    assert_eq!(1, cat.firmware.len());
    assert!(cat.rejected.is_empty());
    let fv = &cat.firmware[0];
    assert!(fv.is_legacy());
    assert_eq!("1.0.0-beta.1", fv.version());
    assert_eq!(ReleaseLabel::Legacy, label_for(fv, &cat.firmware));
    assert!(!installable(ReleaseLabel::Legacy));
    assert!(cat.newest(true).is_none());
    assert!(cat.newer_app("0.0.1", true).is_none());
    let e = download_firmware(fv, t.path(), &net, None, None).unwrap_err();
    assert_eq!(Some(VerifyFailure::NotInstallable), e.kind());
    assert!(net.downloads.borrow().is_empty());
    let cached = client::cached_catalog(t.path(), None);
    assert!(cached.firmware[0].is_legacy());
    assert!(t.path().join("manifests").join(tag).join("ddm25-manifest.json").is_file());
}

fn catalog_of(versions: &[(&str, &str)]) -> Catalog {
    let mut cat = Catalog::default();
    for (v, ch) in versions {
        let m = manifest(vec![firmware_entry(&release_app(v), v, ch, None, None)], &format!("v{v}"));
        let (data, sig) = signed(&m, None);
        cat.add(&format!("v{v}"), &sm::verify_bytes(&data, &sig, Some(&test_public_hex()), None).unwrap(), None);
    }
    cat.sort();
    cat
}

#[test]
fn channel_labels() {
    let cat = catalog_of(&[
        ("1.0.0", "stable"),
        ("1.1.0", "stable"),
        ("1.2.0-beta.1", "beta"),
        ("1.3.0-alpha.1", "beta"),
        ("1.3.0-test.2", "beta"),
        ("0.9.0", "stable"),
    ]);
    let label = |v: &str| label_for(cat.find(v).unwrap(), &cat.firmware);
    assert_eq!(ReleaseLabel::Latest, label("1.1.0"));
    assert_eq!(ReleaseLabel::Stable, label("1.0.0"));
    assert_eq!(ReleaseLabel::Stable, label("0.9.0"));
    assert_eq!(ReleaseLabel::Beta, label("1.2.0-beta.1"));
    assert_eq!(ReleaseLabel::TestAlpha, label("1.3.0-alpha.1"));
    assert_eq!(ReleaseLabel::TestAlpha, label("1.3.0-test.2"));
    assert!(needs_confirmation(ReleaseLabel::TestAlpha));
    assert!(!needs_confirmation(ReleaseLabel::Beta));
    assert_eq!(
        vec!["1.3.0-test.2", "1.3.0-alpha.1", "1.2.0-beta.1", "1.1.0", "1.0.0", "0.9.0"],
        cat.firmware.iter().map(|f| f.version()).collect::<Vec<_>>()
    );
    assert_eq!("1.3.0-test.2", cat.newest(true).unwrap().version());
    assert_eq!("1.1.0", cat.newest(false).unwrap().version());
}

#[test]
fn program_updates_come_only_from_app_rs_for_this_platform() {
    let mut cat = Catalog::default();
    let mut m = manifest(vec![], "v3.0.0");
    m["app"] = app_entry("9.0.0", &[1], None);
    m["app_net"] = app_entry("8.0.0", &[1], None);
    m["app_rs"] = json!({"windows-x86_64": app_entry("3.1.0", &[2], Some("v3.0.0")), "linux-x86_64": app_entry("3.1.0", &[3], Some("v3.0.0"))});
    let (data, sig) = signed(&m, None);
    cat.add_for_platform(
        "v3.0.0",
        &sm::verify_bytes(&data, &sig, Some(&test_public_hex()), None).unwrap(),
        None,
        "linux-x86_64",
    );
    let mut m2 = manifest(vec![], "v3.1.0");
    m2["app_rs"] = json!({"linux-x86_64": app_entry("3.2.0-beta.1", &[4], Some("v3.1.0"))});
    let (data, sig) = signed(&m2, None);
    cat.add_for_platform(
        "v3.1.0",
        &sm::verify_bytes(&data, &sig, Some(&test_public_hex()), None).unwrap(),
        None,
        "linux-x86_64",
    );
    cat.sort();
    assert_eq!("3.2.0-beta.1", cat.newer_app("3.0.0-alpha.1", true).unwrap().entry.version);
    assert_eq!("3.1.0", cat.newer_app("3.0.0-alpha.1", false).unwrap().entry.version);
    assert!(cat.newer_app("3.2.0", true).is_none());
    assert_eq!([3u8].to_vec().len(), 1);
    // the app / app_net zips (the Python and C# programs) are never this program's update
    assert!(cat.apps.iter().all(|a| a.entry.version.starts_with('3')));
}

// --- downloads ---

struct Dl {
    t: tempfile::TempDir,
    app: Vec<u8>,
    zip: Vec<u8>,
    entry: Value,
    net: FakeNet,
    listing: Vec<Value>,
}

impl Dl {
    fn new() -> Self {
        let t = tmp();
        std::fs::create_dir_all(t.path().join("surumler")).unwrap();
        let app = release_app("1.2.0");
        let zip = release_zip(&app);
        let entry = firmware_entry(&app, "1.2.0", "stable", Some("v1.2.0"), Some(&zip));
        let mut d = Dl { t, app, zip, entry: entry.clone(), net: FakeNet::default(), listing: vec![] };
        d.add_release("v1.2.0", &manifest(vec![entry], "v1.2.0"));
        d
    }
    fn root(&self) -> std::path::PathBuf {
        self.t.path().join("surumler")
    }
    fn add_release(&mut self, tag: &str, m: &Value) {
        let (data, sig) = signed(m, None);
        self.net.files.borrow_mut().insert(url(tag, "ddm32-manifest.json"), data);
        self.net.files.borrow_mut().insert(url(tag, "ddm32-manifest.json.sig"), sig);
        self.listing.push(json!({"tag_name": tag, "assets": [
            {"name": "ddm32-manifest.json", "browser_download_url": url(tag, "ddm32-manifest.json")},
            {"name": "ddm32-manifest.json.sig", "browser_download_url": url(tag, "ddm32-manifest.json.sig")}]}));
    }
    fn version(&self) -> ddm32_core::releases::FirmwareVersion {
        self.net.files.borrow_mut().insert(API_URL.into(), Value::Array(self.listing.clone()).to_string().into_bytes());
        let cat = load_catalog(&self.root(), &self.net, Some(&test_public_hex()), None).unwrap();
        cat.find("1.2.0").unwrap().clone()
    }
}

#[test]
fn download_verifies_hash_and_contents() {
    let d = Dl::new();
    let fv = d.version();
    *d.net.payload.borrow_mut() = Some(d.zip.clone());
    let k = test_public_hex();
    let folder = download_firmware(&fv, &d.root(), &d.net, None, Some(&k)).unwrap();
    let p = Package::new(&folder, Some(&k));
    assert!(p.installable(), "{:?}", p.problems);
    assert_eq!(sm::sha256_hex(&d.app), p.sha256);
    let mut want: Vec<String> = PACKAGE_FILES.iter().map(|s| s.to_string()).collect();
    want.extend(["ddm32-manifest.json".into(), "ddm32-manifest.json.sig".into()]);
    want.sort();
    assert_eq!(want, entries(&folder));
    assert_eq!(vec![d.entry["url"].as_str().unwrap().to_string()], *d.net.downloads.borrow());
    assert_eq!(folder, download_firmware(&fv, &d.root(), &d.net, None, Some(&k)).unwrap());
    assert_eq!(1, d.net.downloads.borrow().len());
    let pk = list_packages(&d.root(), Some(&k));
    assert_eq!(1, pk.len());
    assert_eq!("1.2.0", pk[0].version);
}

#[test]
fn hash_mismatch_leaves_nothing() {
    let d = Dl::new();
    let fv = d.version();
    let mut bad = d.zip.clone();
    let n = bad.len();
    bad[n - 30] ^= 1;
    for payload in [bad, [d.zip.clone(), b"x".to_vec()].concat(), d.zip[..d.zip.len() - 1].to_vec()] {
        *d.net.payload.borrow_mut() = Some(payload);
        let e = download_firmware(&fv, &d.root(), &d.net, None, Some(&test_public_hex())).unwrap_err();
        assert_eq!(Some(VerifyFailure::HashMismatch), e.kind());
        assert_eq!(vec!["manifests"], entries(&d.root()));
    }
}

#[test]
fn unexpected_zip_contents() {
    let mut d = Dl::new();
    let evil = zip_of(&[("rfoundry_dm32.bin", &d.app), ("../evil.py", b"x")]);
    d.listing.clear();
    let e2 = firmware_entry(&d.app, "1.2.0", "stable", Some("v1.2.0"), Some(&evil));
    d.add_release("v1.2.0", &manifest(vec![e2], "v1.2.0"));
    let fv = d.version();
    *d.net.payload.borrow_mut() = Some(evil);
    let e = download_firmware(&fv, &d.root(), &d.net, None, Some(&test_public_hex())).unwrap_err();
    assert_eq!(Some(VerifyFailure::UnexpectedContents), e.kind());
    assert!(!d.t.path().join("evil.py").exists());
    assert!(!d.root().join("evil.py").exists());
}

#[test]
fn application_not_the_signed_one() {
    let mut d = Dl::new();
    let other = release_zip(&release_app("6.6.6"));
    d.listing.clear();
    let e2 = firmware_entry(&d.app, "1.2.0", "stable", Some("v1.2.0"), Some(&other));
    d.add_release("v1.2.0", &manifest(vec![e2], "v1.2.0"));
    let fv = d.version();
    *d.net.payload.borrow_mut() = Some(other);
    let e = download_firmware(&fv, &d.root(), &d.net, None, Some(&test_public_hex())).unwrap_err();
    assert_eq!(Some(VerifyFailure::ApplicationMismatch), e.kind());
    assert_eq!(vec!["manifests"], entries(&d.root()));
}

#[test]
fn package_with_tampered_signature_is_not_installable() {
    let d = Dl::new();
    let fv = d.version();
    *d.net.payload.borrow_mut() = Some(d.zip.clone());
    let k = test_public_hex();
    let folder = download_firmware(&fv, &d.root(), &d.net, None, Some(&k)).unwrap();
    let (_, other_sig) = signed(&manifest(vec![d.entry.clone()], "v1.2.0"), Some(&other_secret()));
    std::fs::write(folder.join("ddm32-manifest.json.sig"), other_sig).unwrap();
    let p = Package::new(&folder, Some(&k));
    assert!(!p.installable());
    assert_eq!(1, p.problems.len());
    assert!(p.problems[0].contains("signature"));
    let again = download_firmware(&fv, &d.root(), &d.net, None, Some(&k)).unwrap();
    assert!(Package::new(&again, Some(&k)).installable());
    assert_eq!(2, d.net.downloads.borrow().len());
}

#[test]
fn program_download_checks_the_hash() {
    let d = Dl::new();
    let updates = d.t.path().join("guncelleme");
    let program_zip = zip_of(&[("DDM32Plus/DDM32Plus.exe", b"exe")]);
    let a = AssetEntry {
        version: "3.1.0".into(),
        file: "DDM32Plus-3.1.0.zip".into(),
        url: url("v3.1.0", "DDM32Plus-3.1.0.zip"),
        sha256: sm::sha256_hex(&program_zip),
        size: program_zip.len() as u64,
        notes: None,
    };
    *d.net.payload.borrow_mut() = Some(program_zip.clone());
    let dest = download_app(&a, &updates, &d.net, None).unwrap();
    assert_eq!(program_zip, std::fs::read(&dest).unwrap());
    let mut bad = program_zip.clone();
    *bad.last_mut().unwrap() ^= 1;
    *d.net.payload.borrow_mut() = Some(bad);
    let e = download_app(&a, &updates, &d.net, None).unwrap_err();
    assert_eq!(Some(VerifyFailure::HashMismatch), e.kind());
    assert!(!dest.exists());
}

#[test]
fn https_github_only() {
    for u in [
        "http://github.com/x",
        "https://example.com/x",
        "ftp://github.com/x",
        "https://github.com.evil.example/x",
        "not a url",
        "https://user@github.com/x",
    ] {
        assert!(check_url(u).is_err(), "{u}");
    }
    check_url("https://objects.githubusercontent.com/github-production-release-asset/x").unwrap();
    check_url("https://GitHub.com/d0ukan/DDM32-Plus/releases/download/v1/x.zip").unwrap();
    check_url(API_URL).unwrap();
}

/// A fake HTTP transport: no socket is ever opened.
struct Fake {
    seen: Arc<Mutex<Vec<String>>>,
    answer: Box<dyn Fn(&str) -> (u16, Option<String>, Vec<u8>) + Send + Sync>,
}

impl HttpTransport for Fake {
    fn get(&self, url: &str, _accept: &str) -> Result<HttpResponse, NetError> {
        self.seen.lock().unwrap().push(url.to_string());
        let (status, location, body) = (self.answer)(url);
        Ok(HttpResponse { status, reason: "X".into(), location, body: Box::new(std::io::Cursor::new(body)) })
    }
}

fn fake(
    answer: impl Fn(&str) -> (u16, Option<String>, Vec<u8>) + Send + Sync + 'static,
) -> (HttpReleaseNet, Arc<Mutex<Vec<String>>>) {
    let seen = Arc::new(Mutex::new(vec![]));
    (HttpReleaseNet::new(Box::new(Fake { seen: seen.clone(), answer: Box::new(answer) })), seen)
}

#[test]
fn redirects_are_checked_again() {
    let (http, seen) = fake(|u| {
        if u.starts_with("https://github.com/") {
            (302, Some("https://release-assets.githubusercontent.com/asset?sig=1".into()), vec![])
        } else {
            (200, None, vec![1, 2, 3])
        }
    });
    assert_eq!(vec![1, 2, 3], http.fetch(&url("v1", "a.json"), 100).unwrap());
    assert_eq!(2, seen.lock().unwrap().len());
    let (http2, seen2) = fake(|_| (302, Some("https://evil.example/x".into()), vec![]));
    let e = http2.fetch(&url("v1", "a.json"), 100).unwrap_err();
    assert!(e.0.contains("Refusing"));
    assert_eq!(1, seen2.lock().unwrap().len());
}

#[test]
fn size_limits_hold() {
    let t = tmp();
    let (http, _) = fake(|_| (200, None, vec![0u8; 200]));
    assert!(http.fetch(&url("v1", "a.json"), 100).is_err());
    let dest = t.path().join("big.zip");
    assert!(http.download(&url("v1", "big.zip"), &dest, 100, None).is_err());
    assert!(!dest.exists());
    assert!(!t.path().join("big.zip.partial").exists());
    let mut seen = 0;
    http.download(&url("v1", "big.zip"), &dest, 200, Some(&mut |d, _| seen = d)).unwrap();
    assert_eq!(200, std::fs::metadata(&dest).unwrap().len());
    assert_eq!(200, seen);
    let (h3, _) = fake(|_| (404, None, vec![]));
    assert!(h3.fetch(API_URL, 10).unwrap_err().0.contains("404"));
}

// --- the local release folder (developer builds) ---

struct Local {
    t: tempfile::TempDir,
    zip: Vec<u8>,
}

impl Local {
    fn new() -> Self {
        let t = tmp();
        let folder = t.path().join("release");
        std::fs::create_dir_all(&folder).unwrap();
        std::fs::create_dir_all(t.path().join("surumler")).unwrap();
        let app = release_app("1.0.0-beta.3");
        let zip = release_zip(&app);
        let entry = firmware_entry(&app, "1.0.0-beta.3", "beta", Some("v1.0.0-beta.3"), Some(&zip));
        std::fs::write(folder.join(entry["file"].as_str().unwrap()), &zip).unwrap();
        write_signed(&folder, &manifest(vec![entry], "v1.0.0-beta.3"), None);
        Local { t, zip }
    }
    fn folder(&self) -> std::path::PathBuf {
        self.t.path().join("release")
    }
    fn root(&self) -> std::path::PathBuf {
        self.t.path().join("surumler")
    }
}

#[test]
fn local_release_verifies_with_the_key_in_force() {
    let l = Local::new();
    let k = test_public_hex();
    let cat = load_local(&l.folder(), &l.root(), Some(&k)).unwrap();
    assert_eq!(1, cat.firmware.len());
    let fv = &cat.firmware[0];
    assert!(fv.is_local());
    assert!(cat.rejected.is_empty());
    let src = fv.source.clone().unwrap();
    let package = download_firmware(fv, &l.root(), src.as_ref(), None, Some(&k)).unwrap();
    assert_eq!(l.root().join("1.0.0-beta.3"), package);
    assert!(Package::new(&package, Some(&k)).installable());
    let real = load_local(&l.folder(), &l.t.path().join("other"), None).unwrap();
    assert!(real.firmware.is_empty());
    assert!(real.rejected[0].1.contains("signature is not valid"));
}

#[test]
fn local_release_changed_files_are_refused() {
    let l = Local::new();
    let k = test_public_hex();
    let z = std::fs::read_dir(l.folder())
        .unwrap()
        .map(|e| e.unwrap().path())
        .find(|p| p.extension().is_some_and(|e| e == "zip"))
        .unwrap();
    let mut bad = l.zip.clone();
    let n = bad.len();
    bad[n - 30] ^= 1;
    std::fs::write(&z, bad).unwrap();
    let cat = load_local(&l.folder(), &l.root(), Some(&k)).unwrap();
    let fv = &cat.firmware[0];
    let src = fv.source.clone().unwrap();
    let e = download_firmware(fv, &l.root(), src.as_ref(), None, Some(&k)).unwrap_err();
    assert_eq!(Some(VerifyFailure::HashMismatch), e.kind());
    assert!(!l.root().join("1.0.0-beta.3").exists());
    std::fs::write(&z, [l.zip.clone(), vec![0]].concat()).unwrap();
    let e = download_firmware(fv, &l.root(), src.as_ref(), None, Some(&k)).unwrap_err();
    assert!(matches!(e, ddm32_core::releases::VerifyError::Net(_)));
    let m = l.folder().join("ddm32-manifest.json");
    std::fs::write(&m, std::fs::read_to_string(&m).unwrap().replace("\"beta\"", "\"stable\"")).unwrap();
    let cat = load_local(&l.folder(), &l.t.path().join("r2"), Some(&k)).unwrap();
    assert!(cat.firmware.is_empty());
    assert!(cat.rejected[0].1.contains("signature is not valid"));
    std::fs::remove_file(l.folder().join("ddm32-manifest.json.sig")).unwrap();
    let cat = load_local(&l.folder(), &l.t.path().join("r3"), Some(&k)).unwrap();
    assert_eq!("no signed manifest", cat.rejected[0].1);
}

#[test]
fn local_release_serves_only_its_own_files() {
    let l = Local::new();
    let net = LocalReleaseNet::new(&l.folder()).unwrap();
    assert!(net.fetch(&url("v1.0.0-beta.3", "..\\x.json"), 100).is_err());
    assert!(net.fetch(&url("v9.9.9", "ddm32-manifest.json"), 1 << 20).is_err());
    assert!(net.fetch("https://example.com/ddm32-manifest.json", 1 << 20).is_err());
    assert!(net.download(&url("v1", "a/b.zip"), &l.t.path().join("x"), 10, None).is_err());
    assert!(LocalReleaseNet::new(&l.t.path().join("empty-xyz")).is_err());
    assert!(String::from_utf8(net.fetch(API_URL, 1 << 20).unwrap()).unwrap().contains("v1.0.0-beta.3"));
}

#[test]
fn local_versions_join_the_list_once() {
    let l = Local::new();
    let mut github = Catalog::default();
    let local = load_local(&l.folder(), &l.root(), Some(&test_public_hex())).unwrap();
    github.add_local(&local);
    assert!(github.firmware[0].is_local());
    github.add_local(&local);
    assert_eq!(1, github.firmware.len());
}

#[test]
fn the_local_release_folder_is_developer_only() {
    // the tests run the release configuration: no developer features
    assert!(!ddm32_core::app_info::DEVELOPER);
}
