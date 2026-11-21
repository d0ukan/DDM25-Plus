// SPDX-License-Identifier: GPL-3.0-or-later
//! A signed release in a local folder (developer builds; Releases/LocalRelease.cs): ddm32-manifest.json, its .sig
//! and the files it lists, as pc/release/make_release.py writes them. It stands in for GitHub as the network; from
//! there on the code path is the downloaded release's own (signature, schema, tag, zip size and SHA-256, the
//! application inside). Only plain file names directly in the folder are served.

use super::catalog::Catalog;
use super::client::load_catalog;
use super::net::{NetError, Progress, ReleaseNet};
use super::signed_manifest::{get, is_safe_file_name, parse_json};
use super::{ManifestSchema, API_URL, CURRENT, SCHEMAS};
use std::path::{Path, PathBuf};
use std::sync::Arc;

#[derive(Debug)]
pub struct LocalReleaseNet {
    folder: PathBuf,
    tag: String,
    schema: ManifestSchema,
}

impl LocalReleaseNet {
    pub fn new(folder: &Path) -> Result<LocalReleaseNet, NetError> {
        let folder = std::path::absolute(folder).unwrap_or_else(|_| folder.to_path_buf());
        let schema = SCHEMAS
            .iter()
            .find(|s| folder.join(s.manifest_name).is_file())
            .copied()
            .ok_or_else(|| NetError(format!("No {} in {}", CURRENT.manifest_name, folder.display())))?
            .clone();
        let data =
            std::fs::read(folder.join(schema.manifest_name)).map_err(|e| NetError(format!("IOException: {e}")))?;
        let doc = parse_json(&data).ok_or_else(|| NetError("The manifest is not JSON".into()))?;
        let tag = match get(&doc, "release") {
            Some(serde_json::Value::String(s)) => s.clone(),
            _ => return Err(NetError("The manifest names no release".into())),
        };
        if !is_safe_file_name(&tag) {
            return Err(NetError("The manifest's release name is not usable".into()));
        }
        Ok(LocalReleaseNet { folder, tag, schema })
    }

    pub fn folder(&self) -> &Path {
        &self.folder
    }

    fn file(&self, name: &str, limit: u64) -> Result<Vec<u8>, NetError> {
        if !is_safe_file_name(name) || name.contains("..") {
            return Err(NetError(format!("Refusing the file name {name}")));
        }
        let path = self.folder.join(name);
        let meta = std::fs::metadata(&path).map_err(|_| NetError(format!("Not in the release folder: {name}")))?;
        if !meta.is_file() {
            return Err(NetError(format!("Not in the release folder: {name}")));
        }
        if meta.len() > limit {
            return Err(NetError(format!("Answer larger than {limit} bytes: {name}")));
        }
        std::fs::read(&path).map_err(|e| NetError(format!("IOException: {e}")))
    }

    fn name_of(&self, url: &str, prefix: &str) -> Result<String, NetError> {
        let name =
            url.strip_prefix(prefix).ok_or_else(|| NetError(format!("Refusing a URL outside the release: {url}")))?;
        if name.contains('/') || name.contains('\\') {
            return Err(NetError(format!("Refusing a URL outside the release: {url}")));
        }
        Ok(name.to_string())
    }
}

impl ReleaseNet for LocalReleaseNet {
    fn fetch(&self, url: &str, limit: usize) -> Result<Vec<u8>, NetError> {
        if url == API_URL {
            let assets: Vec<serde_json::Value> = [self.schema.manifest_name.to_string(), self.schema.sig_name()]
                .into_iter()
                .filter(|n| self.folder.join(n).is_file())
                .map(|n| {
                    serde_json::json!({
                        "name": n,
                        "browser_download_url": format!("{}{}/{}", CURRENT.url_prefix(), self.tag, n),
                    })
                })
                .collect();
            let listing = serde_json::json!([{ "tag_name": self.tag, "assets": assets }]);
            return Ok(listing.to_string().into_bytes());
        }
        let name = self.name_of(url, &format!("{}{}/", CURRENT.url_prefix(), self.tag))?;
        self.file(&name, limit as u64)
    }

    fn download(&self, url: &str, dest: &Path, size: u64, mut progress: Progress<'_>) -> Result<(), NetError> {
        let rest = url
            .strip_prefix(&CURRENT.url_prefix())
            .ok_or_else(|| NetError(format!("Refusing a URL outside the release: {url}")))?;
        let parts: Vec<&str> = rest.split('/').collect();
        if parts.len() != 2 {
            return Err(NetError(format!("Refusing a URL outside the release: {url}")));
        }
        let data = self.file(parts[1], size)?; // more than the signed size is refused, like a download
        let part = super::net::partial_path(dest);
        let io = |e: std::io::Error| NetError(format!("IOException: {e}"));
        std::fs::write(&part, &data).map_err(io)?;
        if let Some(p) = progress.as_mut() {
            p(data.len() as u64, size);
        }
        std::fs::rename(&part, dest).map_err(io)
    }
}

/// The signed release in a local folder, verified exactly like one from GitHub; its versions are marked local (their
/// files come from the folder). Manifests verified here are kept under releases_root/local.
pub fn load_local(folder: &Path, releases_root: &Path, key: Option<&str>) -> Result<Catalog, NetError> {
    let net = Arc::new(LocalReleaseNet::new(folder)?);
    let mut cat = load_catalog(&releases_root.join("local"), net.as_ref(), key, None)?;
    cat.source = "local".into();
    for fv in &mut cat.firmware {
        fv.source = Some(net.clone());
    }
    Ok(cat)
}
