// SPDX-License-Identifier: GPL-3.0-or-later
//! The verified releases (releases.Catalog, Releases/Catalog.cs) and the labels the user sees.

use super::local::LocalReleaseNet;
use super::version::ReleaseVersion;
use super::{AssetEntry, FirmwareEntry, Manifest, ManifestSchema};
use std::path::PathBuf;
use std::sync::Arc;

/// A firmware version from a verified manifest (releases.FirmwareVersion).
#[derive(Debug, Clone)]
pub struct FirmwareVersion {
    pub entry: FirmwareEntry,
    pub release: String,
    pub manifest_dir: Option<PathBuf>,
    pub schema: ManifestSchema,
    /// Where its files come from when not from GitHub: a local release folder (developer builds).
    pub source: Option<Arc<LocalReleaseNet>>,
}

impl FirmwareVersion {
    pub fn is_local(&self) -> bool {
        self.source.is_some()
    }
    pub fn version(&self) -> &str {
        &self.entry.asset.version
    }
    pub fn channel(&self) -> &str {
        &self.entry.channel
    }
    pub fn application_sha256(&self) -> &str {
        &self.entry.application_sha256
    }
    pub fn application_size(&self) -> u64 {
        self.entry.application_size
    }
    pub fn is_beta(&self) -> bool {
        self.channel() == "beta"
    }
    /// From a release that still uses the DDM25 names: listed, never installed by this program.
    pub fn is_legacy(&self) -> bool {
        self.schema.is_legacy
    }
    pub fn key(&self) -> ReleaseVersion {
        ReleaseVersion::parse(self.version()).expect("checked by the manifest schema")
    }
    pub fn notes(&self, lang: &str) -> String {
        self.entry.notes.for_lang(lang).to_string()
    }
}

/// A program zip from a verified manifest, with its release tag and cache folder.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppRelease {
    pub entry: AssetEntry,
    pub release: String,
    pub manifest_dir: Option<PathBuf>,
}

/// The verified releases: firmware newest first, program updates, refused releases.
#[derive(Debug, Clone, Default)]
pub struct Catalog {
    pub firmware: Vec<FirmwareVersion>,
    /// This program's builds for this platform (`app_rs`), newest first.
    pub apps: Vec<AppRelease>,
    pub rejected: Vec<(String, String)>,
    pub checked_utc: Option<String>,
    pub source: String,
}

impl Catalog {
    pub fn new(source: &str) -> Self {
        Self { source: source.into(), ..Default::default() }
    }

    pub fn add(&mut self, tag: &str, manifest: &Manifest, manifest_dir: Option<PathBuf>) {
        self.add_for_platform(tag, manifest, manifest_dir, &super::platform_key());
    }

    /// As [`Catalog::add`], with the program builds of the given platform (tests).
    pub fn add_for_platform(&mut self, tag: &str, manifest: &Manifest, manifest_dir: Option<PathBuf>, platform: &str) {
        for e in &manifest.firmware {
            if !self.firmware.iter().any(|f| f.version() == e.version()) {
                self.firmware.push(FirmwareVersion {
                    entry: e.clone(),
                    release: tag.into(),
                    manifest_dir: manifest_dir.clone(),
                    schema: manifest.schema.clone(),
                    source: None,
                });
            }
        }
        // Only this program's own build for this platform ("app_rs") is a program update; never from a legacy one
        if !manifest.schema.is_legacy {
            if let Some((_, a)) = manifest.app_rs.iter().find(|(p, _)| p == platform) {
                self.apps.push(AppRelease { entry: a.clone(), release: tag.into(), manifest_dir });
            }
        }
    }

    /// Adds the versions of a local release that this list does not have.
    pub fn add_local(&mut self, local: &Catalog) {
        for fv in &local.firmware {
            if !self.firmware.iter().any(|f| f.version() == fv.version()) {
                self.firmware.push(fv.clone());
            }
        }
        self.sort();
    }

    /// Newest first; a stable sort, like Python's list.sort.
    pub fn sort(&mut self) {
        self.firmware.sort_by_key(|f| std::cmp::Reverse(f.key()));
        self.apps.sort_by_key(|a| std::cmp::Reverse(ReleaseVersion::parse(&a.entry.version).expect("checked")));
    }

    pub fn find(&self, version: &str) -> Option<&FirmwareVersion> {
        self.firmware.iter().find(|f| f.version() == version)
    }

    pub fn by_application(&self, sha: &str) -> Option<&FirmwareVersion> {
        self.firmware.iter().find(|f| f.application_sha256() == sha)
    }

    /// The newest installable (not legacy) firmware, betas only when wanted.
    pub fn newest(&self, include_beta: bool) -> Option<&FirmwareVersion> {
        self.firmware.iter().find(|f| !f.is_legacy() && (include_beta || !f.is_beta()))
    }

    /// A program newer than current (prereleases only when include_beta), else None.
    pub fn newer_app(&self, current: &str, include_beta: bool) -> Option<&AppRelease> {
        let cur = ReleaseVersion::parse(current).ok()?;
        self.apps.iter().find(|a| {
            ReleaseVersion::parse(&a.entry.version).is_ok_and(|v| v > cur)
                && (include_beta || !a.entry.version.contains('-'))
        })
    }
}

/// What the user sees for a firmware version.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReleaseLabel {
    /// The newest stable version.
    Latest,
    /// An older stable version.
    Stable,
    Beta,
    /// A test or alpha build: asks "are you sure?" before it is used.
    TestAlpha,
    /// A release with the old DDM25 names: shown, not installable here.
    Legacy,
}

const TEST_PREFIXES: [&str; 3] = ["alpha", "test", "dev"];

/// A test or alpha build is a beta whose prerelease starts with alpha, test or dev (e.g. 1.2.0-alpha.1).
pub fn is_test_or_alpha(version: &str) -> bool {
    let Some(pre) = ReleaseVersion::try_parse(version).and_then(|v| v.prerelease) else { return false };
    let first = pre.split('.').next().unwrap_or("").to_lowercase();
    TEST_PREFIXES.iter().any(|p| first.starts_with(p))
}

pub fn label_for(fv: &FirmwareVersion, all: &[FirmwareVersion]) -> ReleaseLabel {
    if fv.is_legacy() {
        return ReleaseLabel::Legacy;
    }
    if is_test_or_alpha(fv.version()) {
        return ReleaseLabel::TestAlpha;
    }
    if fv.is_beta() {
        return ReleaseLabel::Beta;
    }
    // the first of the highest (OrderByDescending(...).FirstOrDefault(), a stable order)
    let mut newest_stable: Option<&FirmwareVersion> = None;
    for f in all.iter().filter(|f| !f.is_legacy() && !f.is_beta() && !is_test_or_alpha(f.version())) {
        if newest_stable.is_none_or(|n| f.key() > n.key()) {
            newest_stable = Some(f);
        }
    }
    match newest_stable {
        Some(n) if n.version() == fv.version() => ReleaseLabel::Latest,
        _ => ReleaseLabel::Stable,
    }
}

/// A second "are you sure?" before this label is downloaded or installed.
pub fn needs_confirmation(label: ReleaseLabel) -> bool {
    label == ReleaseLabel::TestAlpha
}

pub fn installable(label: ReleaseLabel) -> bool {
    label != ReleaseLabel::Legacy
}
