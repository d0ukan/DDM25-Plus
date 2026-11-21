// SPDX-License-Identifier: GPL-3.0-or-later
//! Network access for the release list and downloads (Releases/ReleaseNet.cs). GitHub over HTTPS only (rustls, the
//! Mozilla root set built in, so no system CA store is needed on any OS), with every redirect checked against the
//! allowed hosts. TLS is not what makes a release trusted: every file is checked against the Ed25519-signed
//! manifest before it is used. Tests pass a fake [`HttpTransport`]: no socket is opened.

use super::ALLOWED_HOSTS;
use std::fmt;
use std::io::{Read, Write};
use std::path::Path;

/// A network failure or a refused address (releases.NetError).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetError(pub String);

impl fmt::Display for NetError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for NetError {}

pub type Progress<'a> = Option<&'a mut dyn FnMut(u64, u64)>;

/// Network access for the release list and downloads; tests pass a fake.
pub trait ReleaseNet {
    /// The body of url, at most limit bytes.
    fn fetch(&self, url: &str, limit: usize) -> Result<Vec<u8>, NetError>;

    /// url into dest (via dest.partial), refusing more than size bytes; progress(done, size).
    fn download(&self, url: &str, dest: &Path, size: u64, progress: Progress<'_>) -> Result<(), NetError>;
}

/// One HTTP answer: the status, the Location header, the body.
pub struct HttpResponse {
    pub status: u16,
    pub reason: String,
    pub location: Option<String>,
    pub body: Box<dyn Read + Send>,
}

/// One GET without following redirects. The real one is [`UreqTransport`]; tests pass a fake.
pub trait HttpTransport: Send + Sync {
    fn get(&self, url: &str, accept: &str) -> Result<HttpResponse, NetError>;
}

/// A parsed absolute URL, as far as the checks need it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Url {
    pub scheme: String,
    pub host: String,
    pub has_userinfo: bool,
    /// Everything from the path on ("/a/b?x").
    pub rest: String,
}

impl Url {
    pub fn parse(url: &str) -> Option<Url> {
        let (scheme, after) = url.split_once("://")?;
        if scheme.is_empty() || !scheme.chars().all(|c| c.is_ascii_alphanumeric() || "+-.".contains(c)) {
            return None;
        }
        let end = after.find(['/', '?', '#']).unwrap_or(after.len());
        let authority = &after[..end];
        let rest = &after[end..];
        let (userinfo, hostport) = match authority.rsplit_once('@') {
            Some((u, h)) => (Some(u), h),
            None => (None, authority),
        };
        let host = match hostport.rsplit_once(':') {
            Some((h, port)) if port.chars().all(|c| c.is_ascii_digit()) => h,
            _ => hostport,
        };
        if host.is_empty() || host.contains(' ') || url.contains(' ') {
            return None;
        }
        Some(Url {
            scheme: scheme.to_ascii_lowercase(),
            host: host.to_ascii_lowercase(),
            has_userinfo: userinfo.is_some_and(|u| !u.is_empty()),
            rest: if rest.is_empty() { "/".into() } else { rest.into() },
        })
    }

    /// A Location header against this URL (absolute, scheme-relative or path-relative).
    pub fn join(&self, location: &str) -> String {
        if location.contains("://") {
            location.to_string()
        } else if let Some(r) = location.strip_prefix("//") {
            format!("{}://{}", self.scheme, r)
        } else if location.starts_with('/') {
            format!("{}://{}{}", self.scheme, self.host, location)
        } else {
            let path = self.rest.split(['?', '#']).next().unwrap_or("/");
            let dir = &path[..path.rfind('/').map_or(0, |i| i + 1)];
            format!("{}://{}{}{}", self.scheme, self.host, dir, location)
        }
    }
}

/// Refuses anything but https:// on the allowed hosts (releases._check_url); the message names the address.
pub fn check_url_hosts(url: &str, hosts: &[&str], what: &str) -> Result<Url, NetError> {
    match Url::parse(url) {
        Some(u) if u.scheme == "https" && hosts.contains(&u.host.as_str()) && !u.has_userinfo => Ok(u),
        Some(u) => Err(NetError(format!("Refusing a download outside {what} over HTTPS: {}://{}", u.scheme, u.host))),
        None => Err(NetError(format!("Refusing a download outside {what} over HTTPS: {url}"))),
    }
}

/// Refuses anything but https:// on the GitHub hosts.
pub fn check_url(url: &str) -> Result<Url, NetError> {
    check_url_hosts(url, &ALLOWED_HOSTS, "GitHub")
}

const MAX_REDIRECTS: usize = 10;

/// Opens url, following at most `max_redirects` redirects, each checked by `check` first.
pub fn open_checked(
    transport: &dyn HttpTransport,
    url: &str,
    max_redirects: usize,
    check: &dyn Fn(&str) -> Result<Url, NetError>,
    accept: &dyn Fn(&Url) -> &'static str,
) -> Result<HttpResponse, NetError> {
    let mut current = check(url)?;
    let mut current_text = url.to_string();
    for _ in 0..=max_redirects {
        let resp = transport.get(&current_text, accept(&current))?;
        if (300..400).contains(&resp.status) {
            if let Some(loc) = &resp.location {
                let next = current.join(loc);
                current = check(&next)?;
                current_text = next;
                continue;
            }
        }
        if !(200..300).contains(&resp.status) {
            return Err(NetError(format!("HTTP Error {}: {}", resp.status, resp.reason)));
        }
        return Ok(resp);
    }
    Err(NetError("Too many redirects".into()))
}

/// Reads at most limit bytes of body; more is an error.
pub fn read_limited(body: &mut dyn Read, limit: u64, url: &str) -> Result<Vec<u8>, NetError> {
    let mut out = Vec::new();
    let mut block = vec![0u8; 1 << 16];
    loop {
        let n = body.read(&mut block).map_err(|e| NetError(format!("IOException: {e}")))?;
        if n == 0 {
            return Ok(out);
        }
        out.extend_from_slice(&block[..n]);
        if out.len() as u64 > limit {
            return Err(NetError(format!("Answer larger than {limit} bytes: {url}")));
        }
    }
}

/// Streams body into dest via dest.partial, refusing more than size bytes (the partial file is removed then).
pub fn stream_to_file(
    body: &mut dyn Read,
    dest: &Path,
    size: u64,
    too_large: &str,
    mut progress: Progress<'_>,
) -> Result<(), NetError> {
    let part = partial_path(dest);
    let io = |e: std::io::Error| NetError(format!("IOException: {e}"));
    {
        let mut out = std::fs::File::create(&part).map_err(io)?;
        let mut block = vec![0u8; 1 << 16];
        let mut done = 0u64;
        loop {
            let n = match body.read(&mut block) {
                Ok(n) => n,
                Err(e) => {
                    drop(out);
                    let _ = std::fs::remove_file(&part);
                    return Err(io(e));
                }
            };
            if n == 0 {
                break;
            }
            done += n as u64;
            if done > size {
                drop(out);
                let _ = std::fs::remove_file(&part);
                return Err(NetError(too_large.into()));
            }
            out.write_all(&block[..n]).map_err(io)?;
            if let Some(p) = progress.as_mut() {
                p(done, size);
            }
        }
        out.flush().map_err(io)?;
    }
    std::fs::rename(&part, dest).map_err(io)
}

pub fn partial_path(dest: &Path) -> std::path::PathBuf {
    let mut s = dest.as_os_str().to_owned();
    s.push(".partial");
    s.into()
}

/// GitHub over HTTPS with the checks above.
pub struct HttpReleaseNet {
    transport: Box<dyn HttpTransport>,
}

impl HttpReleaseNet {
    pub fn new(transport: Box<dyn HttpTransport>) -> Self {
        Self { transport }
    }

    /// The real network (ureq with rustls).
    pub fn real() -> Self {
        Self::new(Box::new(UreqTransport::new(60)))
    }

    fn open(&self, url: &str) -> Result<HttpResponse, NetError> {
        open_checked(self.transport.as_ref(), url, MAX_REDIRECTS, &check_url, &|u: &Url| {
            if u.host == "api.github.com" {
                "application/vnd.github+json"
            } else {
                "application/octet-stream, */*"
            }
        })
    }
}

impl ReleaseNet for HttpReleaseNet {
    fn fetch(&self, url: &str, limit: usize) -> Result<Vec<u8>, NetError> {
        let mut resp = self.open(url)?;
        read_limited(&mut resp.body, limit as u64, url)
    }

    fn download(&self, url: &str, dest: &Path, size: u64, progress: Progress<'_>) -> Result<(), NetError> {
        let mut resp = self.open(url)?;
        stream_to_file(&mut resp.body, dest, size, "Download larger than the signed size", progress)
    }
}

/// The real HTTP transport: ureq, rustls, no automatic redirects, no decompression.
pub struct UreqTransport {
    agent: ureq::Agent,
}

impl UreqTransport {
    pub fn new(timeout_seconds: u64) -> Self {
        let agent = ureq::AgentBuilder::new()
            .redirects(0)
            .timeout_connect(std::time::Duration::from_secs(30))
            .timeout(std::time::Duration::from_secs(timeout_seconds))
            .user_agent(&format!("{}/{}", crate::app_info::EXE_NAME, crate::app_info::VERSION))
            .build();
        Self { agent }
    }
}

impl HttpTransport for UreqTransport {
    fn get(&self, url: &str, accept: &str) -> Result<HttpResponse, NetError> {
        let resp = match self.agent.get(url).set("Accept", accept).call() {
            Ok(r) => r,
            Err(ureq::Error::Status(_, r)) => r,
            Err(ureq::Error::Transport(t)) => return Err(NetError(format!("HttpRequestException: {t}"))),
        };
        Ok(HttpResponse {
            status: resp.status(),
            reason: resp.status_text().to_string(),
            location: resp.header("location").map(str::to_string),
            body: Box::new(resp.into_reader()),
        })
    }
}
