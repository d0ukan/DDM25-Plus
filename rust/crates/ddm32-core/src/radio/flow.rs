// SPDX-License-Identifier: GPL-3.0-or-later
//! The install and restore flows' rules (pc/ddm32_app/wizard.py, Radio/Flow.cs) and the file-only steps the pages
//! run (detection and plans, the same as the tools'). Radio steps start only from their own button; nothing here
//! starts a step by itself.

use super::codec::Codec;
use super::error::{ToolError, ToolResult};
use super::fast_install;
use super::plan;
use super::unit_image;
use crate::pyjson::{self, Py, PyDict};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum StepState {
    #[default]
    Idle,
    Running,
    Ok,
    Failed,
    Used,
    Stale,
    Skipped,
}

/// Where the install's backup comes from: a new one (the default), an earlier one of the same radio (quick update),
/// or none (install without backup). The last two only over an installed DDM32 Plus.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum BackupPath {
    #[default]
    Normal,
    Quick,
    None,
}

/// The flow refused a step (an InvalidOperationException in C#): a bug in the caller, never a radio step.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FlowError(pub &'static str);

/// How a radio step ended, read from what the tool left (pc/ddm32_app/parsers.py).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct StepResult {
    pub passed: bool,
    pub writes_started: bool,
    pub timeout: bool,
    pub error: String,
}

fn json_true(path: &Path, key: &str) -> bool {
    std::fs::read(path)
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .is_some_and(|v| v.get(key) == Some(&serde_json::Value::Bool(true)))
}

impl StepResult {
    pub fn new(passed: bool, writes_started: bool, timeout: bool, error: &str) -> Self {
        Self { passed, writes_started, timeout, error: error.into() }
    }

    /// The radio was not switched on in time: the tool gave up waiting for its boot ROM.
    pub fn is_timeout(error: &str) -> bool {
        error.contains("did not start its serial boot ROM within")
    }

    /// A finished backup tool run: verified when its manifest says so.
    pub fn backup(out_dir: &Path, error: Option<&ToolError>) -> Self {
        let verified = json_true(&out_dir.join("backup-manifest.json"), "backup_verified");
        let msg = error.map(ToolError::record).unwrap_or_default();
        Self { passed: verified && error.is_none(), writes_started: false, timeout: Self::is_timeout(&msg), error: msg }
    }

    /// A finished install or restore run: passed with its receipt saying restart_sent; writes_started from the receipt
    /// (no receipt: nothing was written).
    pub fn install(out_dir: &Path, error: Option<&ToolError>) -> Self {
        let r = out_dir.join("install-receipt.json");
        let started = json_true(&r, "writes_started");
        let restarted = json_true(&r, "restart_sent");
        let msg = error.map(ToolError::record).unwrap_or_default();
        Self {
            passed: error.is_none() && restarted,
            writes_started: started,
            timeout: Self::is_timeout(&msg),
            error: msg,
        }
    }
}

/// A downloaded release the wizard may install.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Installable {
    pub sha256: String,
    pub installable: bool,
}

/// A message the UI shows: a resource key and its arguments.
pub type Message = (&'static str, Vec<String>);

fn msg(key: &'static str) -> Message {
    (key, vec![])
}

/// The faster paths are for an update over an installed DDM32 Plus only.
pub fn is_ddm32(d: Option<&PyDict>) -> bool {
    d.is_some_and(|d| d.str("layout") == Some("classic") && d.str("firmware") == Some("ddm32"))
}

/// The install flow: backup, detect, plan, install.
#[derive(Debug, Clone, Default)]
pub struct Wizard {
    pub port: Option<String>,
    pub package: Option<Installable>,
    pub backup: StepState,
    pub backup_dir: Option<PathBuf>,
    pub backup_existing: bool,
    pub detect_state: StepState,
    pub detection: Option<PyDict>,
    pub plan: StepState,
    pub plan_data: Option<PyDict>,
    pub plan_key: Option<(PathBuf, String, String)>,
    pub install: StepState,
    pub install_result: Option<StepResult>,
    pub locked: bool,
    pub running: Option<&'static str>,
    pub message: Message,
    /// What the Backup step shows: the backup's own result.
    pub backup_message: Message,
    pub path: BackupPath,
    /// The "I understand" box and the second confirmation of the install without backup.
    pub risk_accepted: bool,
}

impl Wizard {
    pub fn new() -> Self {
        Self { message: msg(""), backup_message: msg(""), ..Default::default() }
    }

    pub fn set_port(&mut self, port: Option<&str>) -> Result<(), FlowError> {
        if matches!(self.running, Some("backup") | Some("install")) {
            return Err(FlowError("port change while a radio step runs"));
        }
        self.port = port.filter(|p| !p.is_empty()).map(str::to_string);
        Ok(())
    }

    pub fn set_package(&mut self, package: Option<Installable>) -> Result<(), FlowError> {
        if self.running.is_some() {
            return Err(FlowError("package change while a step runs"));
        }
        // the same application (SHA-256) and installability is the same package: the plan stays
        let same = match (&package, &self.package) {
            (Some(a), Some(b)) => a.sha256 == b.sha256 && a.installable == b.installable,
            (None, None) => true,
            _ => false,
        };
        if !same {
            self.reset_plan();
        }
        self.package = package;
        Ok(())
    }

    fn reset_plan(&mut self) {
        self.plan = StepState::Idle;
        self.plan_data = None;
        self.plan_key = None;
        if self.install != StepState::Running {
            self.install = StepState::Idle;
        }
    }

    /// install_plus / install_plus_from_stock / install_plus_quick / install_plus_no_backup, or None.
    pub fn mode(&self) -> Option<&'static str> {
        if self.path == BackupPath::None {
            return Some("install_plus_no_backup");
        }
        let d = self.detection.as_ref()?;
        if self.detect_state != StepState::Ok || !d.get("supported").is_some_and(Py::is_true) {
            return None;
        }
        if self.path == BackupPath::Quick {
            return is_ddm32(Some(d)).then_some("install_plus_quick");
        }
        match d.str("install_mode") {
            Some("over_classic") => Some("install_plus"),
            Some("from_stock") => Some("install_plus_from_stock"),
            _ => None,
        }
    }

    pub fn can_backup(&self) -> bool {
        self.running.is_none() && self.port.is_some()
    }

    pub fn can_detect(&self) -> bool {
        self.running.is_none() && self.backup == StepState::Ok && self.path != BackupPath::None && !self.locked
    }

    fn package_ok(&self) -> bool {
        self.package.as_ref().is_some_and(|p| p.installable)
    }

    pub fn can_plan(&self) -> bool {
        self.running.is_none()
            && self.backup == StepState::Ok
            && self.detect_state == StepState::Ok
            && self.mode().is_some()
            && !self.locked
            && self.install != StepState::Ok
            && self.package_ok()
    }

    pub fn can_install(&self) -> bool {
        if self.path == BackupPath::None {
            return self.running.is_none()
                && self.backup == StepState::Skipped
                && self.risk_accepted
                && self.port.is_some()
                && !self.locked
                && self.install == StepState::Idle
                && self.package_ok();
        }
        self.running.is_none()
            && self.plan == StepState::Ok
            && self.port.is_some()
            && !self.locked
            && self.install == StepState::Idle
            && self.package_ok()
            && self.plan_key
                == Some((
                    self.backup_dir.clone().unwrap_or_default(),
                    self.package.as_ref().unwrap().sha256.clone(),
                    self.mode().unwrap_or("").to_string(),
                ))
    }

    /// (A) quick update: an earlier verified backup of this radio (checked live by the tool before writing).
    pub fn use_quick_backup(&mut self, backup_dir: &Path) -> Result<(), FlowError> {
        self.use_existing_backup(backup_dir)?;
        self.path = BackupPath::Quick;
        self.message = msg("Wiz_QuickChosen");
        Ok(())
    }

    /// (B) install without backup: only with the risk accepted ("I understand").
    pub fn choose_no_backup(&mut self, understood: bool) -> Result<(), FlowError> {
        if self.running.is_some() {
            return Err(FlowError("busy"));
        }
        if !understood {
            return Err(FlowError("the risk of an install without backup must be accepted first"));
        }
        self.backup = StepState::Skipped;
        self.backup_dir = None;
        self.backup_existing = false;
        self.detect_state = StepState::Idle;
        self.detection = None;
        self.locked = false;
        self.reset_plan();
        self.install = StepState::Idle;
        self.install_result = None;
        self.path = BackupPath::None;
        self.risk_accepted = true;
        self.message = msg("Wiz_NoBackupChosen");
        Ok(())
    }

    /// (tool, arguments) of the install for the chosen path.
    pub fn install_command(&self, plus_folder: &str, out_dir: &str) -> Result<(&'static str, Vec<String>), FlowError> {
        let mode = self.mode().ok_or(FlowError("no install mode"))?;
        let port = self.port.clone().unwrap_or_default();
        let backup = self.backup_dir.as_ref().map(|b| b.to_string_lossy().into_owned()).unwrap_or_default();
        let v = |items: &[&str]| items.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        Ok(match mode {
            "install_plus_quick" => (
                "fast_install",
                v(&[
                    "--port",
                    &port,
                    "--quick",
                    "--backup",
                    &backup,
                    "--plus",
                    plus_folder,
                    "--out",
                    out_dir,
                    "--wait",
                    "600",
                ]),
            ),
            "install_plus_no_backup" => (
                "fast_install",
                v(&["--port", &port, "--no-backup", "--plus", plus_folder, "--out", out_dir, "--wait", "600"]),
            ),
            "install_plus_from_stock" => (
                "install_plus",
                v(&[
                    "--port",
                    &port,
                    "--backup",
                    &backup,
                    "--plus",
                    plus_folder,
                    "--out",
                    out_dir,
                    "--wait",
                    "600",
                    "--from-stock",
                ]),
            ),
            _ => (
                "install_plus",
                v(&["--port", &port, "--backup", &backup, "--plus", plus_folder, "--out", out_dir, "--wait", "600"]),
            ),
        })
    }

    pub fn start_backup(&mut self, backup_dir: &Path) -> Result<(), FlowError> {
        if !self.can_backup() {
            return Err(FlowError("backup not allowed now"));
        }
        self.running = Some("backup");
        self.path = BackupPath::Normal;
        self.risk_accepted = false;
        self.backup = StepState::Running;
        self.backup_dir = Some(backup_dir.to_path_buf());
        self.backup_existing = false;
        self.detect_state = StepState::Idle;
        self.detection = None;
        self.reset_plan();
        self.install = StepState::Idle;
        self.install_result = None;
        self.message = msg("Wiz_BackupRunning");
        self.backup_message = msg("Wiz_BackupRunning");
        Ok(())
    }

    pub fn backup_finished(&mut self, result: &StepResult) {
        self.running = None;
        if result.passed {
            self.backup = StepState::Ok;
            self.locked = false;
            self.message = msg("Wiz_BackupVerified");
            let dir = self.backup_dir.as_ref().map(|d| d.to_string_lossy().into_owned()).unwrap_or_default();
            self.backup_message = ("Wiz_BackupDone", vec![dir]);
        } else {
            self.backup = StepState::Failed;
            let m = if result.timeout {
                msg("Wiz_BackupTimeout")
            } else {
                ("Wiz_BackupFailed", vec![result.error.clone()])
            };
            self.message = m.clone();
            self.backup_message = m;
        }
    }

    pub fn use_existing_backup(&mut self, backup_dir: &Path) -> Result<(), FlowError> {
        if self.running.is_some() {
            return Err(FlowError("busy"));
        }
        self.path = BackupPath::Normal;
        self.risk_accepted = false;
        self.backup = StepState::Ok;
        self.backup_dir = Some(backup_dir.to_path_buf());
        self.backup_existing = true;
        self.detect_state = StepState::Idle;
        self.detection = None;
        self.locked = false;
        self.reset_plan();
        self.install = StepState::Idle;
        self.install_result = None;
        self.message = msg("Wiz_ExistingBackup");
        self.backup_message = msg("Wiz_ExistingBackup");
        Ok(())
    }

    pub fn start_detect(&mut self) -> Result<(), FlowError> {
        if !self.can_detect() {
            return Err(FlowError("detect not allowed now"));
        }
        self.running = Some("detect");
        self.detect_state = StepState::Running;
        self.detection = None;
        self.reset_plan();
        Ok(())
    }

    pub fn detect_finished(&mut self, detection: Option<PyDict>, error: &str) {
        self.running = None;
        match detection {
            Some(d) => {
                self.message = if !d.get("supported").is_some_and(Py::is_true) {
                    ("Wiz_Unsupported", vec![d.str("reason").unwrap_or("").to_string()])
                } else if self.path == BackupPath::Quick && !is_ddm32(Some(&d)) {
                    msg("Wiz_QuickNeedsDdm32")
                } else {
                    msg("")
                };
                self.detect_state = StepState::Ok;
                self.detection = Some(d);
            }
            None => {
                self.detect_state = StepState::Failed;
                self.detection = None;
                self.message = ("Wiz_DetectFailed", vec![if error.is_empty() { "?".into() } else { error.into() }]);
            }
        }
    }

    pub fn start_plan(&mut self) -> Result<(), FlowError> {
        if !self.can_plan() {
            return Err(FlowError("plan not allowed now"));
        }
        self.running = Some("plan");
        self.plan = StepState::Running;
        self.plan_data = None;
        self.plan_key = Some((
            self.backup_dir.clone().unwrap_or_default(),
            self.package.as_ref().unwrap().sha256.clone(),
            self.mode().unwrap().to_string(),
        ));
        self.message = msg("Wiz_Planning");
        Ok(())
    }

    pub fn plan_finished(&mut self, plan: Option<PyDict>, error: &str, safe: bool) {
        self.running = None;
        if plan.is_some() && safe {
            self.plan = StepState::Ok;
            self.plan_data = plan;
            self.install = StepState::Idle;
            self.message = msg("Wiz_PlanReady");
        } else {
            self.plan = StepState::Failed;
            self.plan_data = plan;
            self.plan_key = None;
            self.message = ("Wiz_PlanFailed", vec![if error.is_empty() { "unsafe plan".into() } else { error.into() }]);
        }
    }

    pub fn start_install(&mut self) -> Result<(), FlowError> {
        if !self.can_install() {
            return Err(FlowError("install not allowed now"));
        }
        self.running = Some("install");
        self.install = StepState::Running;
        self.plan = StepState::Used; // one plan, one install click: a new try needs a new plan
        self.message = msg("Wiz_Installing");
        Ok(())
    }

    pub fn install_finished(&mut self, result: &StepResult) {
        self.running = None;
        self.install_result = Some(result.clone());
        if result.passed {
            self.install = StepState::Ok;
            self.backup = StepState::Stale; // the radio no longer matches the backup
            self.message = msg("Wiz_InstallPassed");
        } else if result.writes_started {
            self.install = StepState::Failed;
            self.locked = true;
            let key = if self.path == BackupPath::None {
                "Wiz_NoBackupStoppedAfterWrites"
            } else {
                "Wiz_InstallStoppedAfterWrites"
            };
            self.message = (key, vec![result.error.clone()]);
        } else {
            self.install = StepState::Idle;
            self.plan = StepState::Idle;
            let key = if result.timeout { "Wiz_InstallNotStartedTimeout" } else { "Wiz_InstallNotStarted" };
            self.message = (key, vec![result.error.clone()]);
        }
    }
}

/// "Geri dön" (wizard.RestoreFlow): a backup of this radio, detect, plan, restore. The tool follows the backup's
/// layout: stock: install_firstboot --restore-stock; Classic layout: install_plus --restore.
#[derive(Debug, Clone, Default)]
pub struct RestoreFlow {
    pub port: Option<String>,
    pub kind: Option<String>,
    pub backup_dir: Option<PathBuf>,
    pub detect_state: StepState,
    pub detection: Option<PyDict>,
    pub plan: StepState,
    pub plan_data: Option<PyDict>,
    pub restore: StepState,
    pub result: Option<StepResult>,
    pub running: Option<&'static str>,
    pub message: Message,
}

impl RestoreFlow {
    pub fn new() -> Self {
        Self { message: msg(""), ..Default::default() }
    }

    pub fn choose(&mut self, kind: &str, backup_dir: &Path) -> Result<(), FlowError> {
        if self.running.is_some() {
            return Err(FlowError("busy"));
        }
        self.kind = Some(kind.into());
        self.backup_dir = Some(backup_dir.to_path_buf());
        self.detect_state = StepState::Idle;
        self.detection = None;
        self.plan = StepState::Idle;
        self.plan_data = None;
        self.restore = StepState::Idle;
        self.result = None;
        self.message = msg("");
        Ok(())
    }

    /// (tool, extra args, plan mode) for the chosen backup, or None.
    pub fn tool(&self) -> Option<(&'static str, Vec<&'static str>, &'static str)> {
        let d = self.detection.as_ref()?;
        if self.detect_state != StepState::Ok {
            return None;
        }
        if d.str("layout") == Some("stock") {
            return Some(("install_firstboot", vec!["--restore-stock", "--use-saved-backup"], "restore_stock"));
        }
        if d.str("layout") == Some("classic") && self.kind.as_deref() == Some("previous") {
            return Some(("install_plus", vec!["--restore"], "restore"));
        }
        None
    }

    pub fn can_detect(&self) -> bool {
        self.running.is_none() && self.backup_dir.is_some()
    }

    pub fn start_detect(&mut self) -> Result<(), FlowError> {
        if !self.can_detect() {
            return Err(FlowError("detect not allowed now"));
        }
        self.running = Some("detect");
        self.detect_state = StepState::Running;
        Ok(())
    }

    pub fn detect_finished(&mut self, detection: Option<PyDict>, error: &str) {
        self.running = None;
        let reason = detection.as_ref().and_then(|d| d.str("reason")).unwrap_or("").to_string();
        let found = detection.is_some();
        self.detect_state = if found { StepState::Ok } else { StepState::Failed };
        self.detection = detection;
        if !found {
            self.message = ("Res_DetectFailed", vec![if error.is_empty() { "?".into() } else { error.into() }]);
        } else if self.tool().is_none() {
            self.message = if self.kind.as_deref() == Some("stock") {
                msg("Res_NotStock")
            } else {
                ("Res_CannotRestore", vec![reason])
            };
        }
    }

    pub fn can_plan(&self) -> bool {
        self.running.is_none() && self.tool().is_some() && self.restore != StepState::Running
    }

    pub fn start_plan(&mut self) -> Result<(), FlowError> {
        if !self.can_plan() {
            return Err(FlowError("plan not allowed now"));
        }
        self.running = Some("plan");
        self.plan = StepState::Running;
        self.plan_data = None;
        Ok(())
    }

    pub fn plan_finished(&mut self, plan: Option<PyDict>, error: &str, safe: bool) {
        self.running = None;
        if plan.is_some() && safe {
            self.plan = StepState::Ok;
            self.plan_data = plan;
        } else {
            self.plan = StepState::Failed;
            self.plan_data = plan;
            self.message = ("Wiz_PlanFailed", vec![if error.is_empty() { "unsafe plan".into() } else { error.into() }]);
        }
    }

    pub fn can_restore(&self) -> bool {
        self.running.is_none()
            && self.plan == StepState::Ok
            && self.port.is_some()
            && self.restore != StepState::Running
    }

    pub fn start_restore(&mut self) -> Result<(), FlowError> {
        if !self.can_restore() {
            return Err(FlowError("restore not allowed now"));
        }
        self.running = Some("restore");
        self.restore = StepState::Running;
        self.plan = StepState::Used;
        Ok(())
    }

    pub fn restore_finished(&mut self, result: &StepResult) {
        self.running = None;
        self.result = Some(result.clone());
        self.restore = if result.passed { StepState::Ok } else { StepState::Failed };
        if result.passed {
            self.message = msg("Res_Passed");
        } else if result.writes_started {
            self.message = ("Res_StoppedAfterWrites", vec![result.error.clone()]);
            self.plan = StepState::Idle;
        } else {
            let key = if result.timeout { "Wiz_InstallNotStartedTimeout" } else { "Wiz_InstallNotStarted" };
            self.message = (key, vec![result.error.clone()]);
            self.plan = StepState::Idle;
        }
    }
}

/// The file-only steps the pages run (no port): detection and the write plans, the same as the tools'.
pub struct FileSteps;

impl FileSteps {
    pub fn detect_backup(backup: &Path, codec: &Codec) -> ToolResult<PyDict> {
        let images = unit_image::saved_images(backup)?;
        unit_image::detect(&images["progmem-pass1.bin"], codec)
    }

    /// The plan install_plus.py --plan-only prints for this backup and release.
    pub fn plan(mode: &str, backup: &Path, plus: &Path, key_hex: Option<&str>, codec: &Codec) -> ToolResult<PyDict> {
        if mode == "install_plus_quick" {
            return Ok(fast_install::quick_plan(backup, plus, key_hex, codec)?.2);
        }
        if mode == "install_plus_from_stock" {
            let (i, prof) = plan::stock_inputs(backup, plus, key_hex, codec)?;
            unit_image::backup_jedec(backup)?;
            return plan::make_plan(mode, &i.original, &i.target, Some(prof.tag), Some(&prof));
        }
        let (i, tag) = plan::install_inputs(backup, plus, key_hex)?;
        unit_image::backup_jedec(backup)?;
        plan::make_plan("install_plus", &i.original, &i.target, Some(tag), None)
    }

    /// The plan of a restore (install_plus --restore or install_firstboot --restore-stock).
    pub fn restore_plan(mode: &str, backup: &Path) -> ToolResult<PyDict> {
        if mode == "restore" {
            let i = plan::restore_inputs(backup, true)?;
            unit_image::backup_jedec(backup)?;
            return plan::make_plan("restore", &i.original, &i.target, None, None);
        }
        let i = plan::restore_inputs(backup, false)?;
        unit_image::backup_jedec(backup)?;
        let mut p = plan::restore_stock_plan(&i.original, &i.target);
        p.set("boot_area_unchanged", true);
        Ok(p)
    }

    /// The plan is safe to offer: boot area kept, data chip not written.
    pub fn safe(plan: &PyDict) -> bool {
        plan.get("boot_area_unchanged") == Some(&Py::Bool(true))
            && plan.get("data_chip_written") == Some(&Py::Bool(false))
    }

    pub fn plan_text(plan: &PyDict) -> String {
        pyjson::dumps2(&Py::Dict(plan.clone()))
    }
}
