// SPDX-License-Identifier: GPL-3.0-or-later
//! The DM-32UV programming cable is a WCH CH340 (USB VID_1A86&PID_7523; the CH341 in serial mode is PID_5523).
//! Windows lists it even without its driver (problem code 28), so a missing driver is found without opening a port
//! (Radio/Cable.cs). Listing devices never opens a port.

/// A Plug and Play device as the OS lists it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PnpDevice {
    pub device_id: String,
    pub name: String,
    pub error_code: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CableState {
    Ok,
    NoCable,
    NoDriver,
    Problem,
    NoPort,
}

/// What the cable check found: the state, the port of a working cable, the Windows problem code, all ports.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CableStatus {
    pub state: CableState,
    pub port: Option<String>,
    pub error_code: i32,
    pub ports: Vec<String>,
}

const IDS: [&str; 2] = ["VID_1A86&PID_7523", "VID_1A86&PID_5523"];

pub fn is_ch340(d: &PnpDevice) -> bool {
    let id = d.device_id.to_uppercase();
    id.starts_with("USB\\") && IDS.iter().any(|i| id.contains(i))
}

/// "(COM14)" in the device name.
pub fn port_of(d: &PnpDevice) -> Option<String> {
    let re = regex::Regex::new(r"\((COM\d+)\)").unwrap();
    re.captures(&d.name).map(|c| c[1].to_string())
}

pub fn evaluate(devices: &[PnpDevice]) -> CableStatus {
    let none = |state, code| CableStatus { state, port: None, error_code: code, ports: vec![] };
    let cables: Vec<&PnpDevice> = devices.iter().filter(|d| is_ch340(d)).collect();
    if cables.is_empty() {
        return none(CableState::NoCable, 0);
    }
    let mut ports: Vec<String> = cables.iter().filter(|d| d.error_code == 0).filter_map(|d| port_of(d)).collect();
    ports.sort_by_key(|p| p[3..].parse::<u32>().unwrap_or(u32::MAX));
    if !ports.is_empty() {
        return CableStatus { state: CableState::Ok, port: Some(ports[0].clone()), error_code: 0, ports };
    }
    // 28: no driver installed; 1 (not configured) and 18 (reinstall) mean the same to the user
    if let Some(m) = cables.iter().find(|d| matches!(d.error_code, 28 | 1 | 18)) {
        return none(CableState::NoDriver, m.error_code);
    }
    if let Some(p) = cables.iter().find(|d| d.error_code != 0) {
        return none(CableState::Problem, p.error_code);
    }
    none(CableState::NoPort, 0)
}
