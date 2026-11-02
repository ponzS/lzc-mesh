//! Per-user layout persistence under the LPK's private var directory.

use std::collections::BTreeMap;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::protocol::{Platform, Settings};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Device {
    pub id: String,
    pub name: String,
    pub platform: Platform,
    pub w: f64,
    pub h: f64,
    pub x: f64,
    pub y: f64,
    #[serde(default)]
    pub version: String,
    #[serde(default = "enabled_by_default")]
    pub enabled: bool,
}

fn enabled_by_default() -> bool { true }

impl Device {
    pub fn right(&self) -> f64 {
        self.x + self.w
    }
    pub fn bottom(&self) -> f64 {
        self.y + self.h
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceBinding {
    pub public_key: String,
    #[serde(default)]
    pub approved: bool,
    #[serde(default)]
    pub revoked: bool,
}

#[derive(Debug, Default, Clone, Serialize, Deserialize)]
pub struct Snapshot {
    #[serde(default)]
    pub devices: BTreeMap<String, Device>,
    #[serde(default)]
    pub settings: Settings,
    #[serde(default)]
    pub bindings: BTreeMap<String, DeviceBinding>,
}

// Legacy names containing '_' may also have represented punctuation or Unicode.
// Do not guess ownership of those ambiguous directories during migration.
pub(crate) fn account_key(user: &str) -> String {
    if !user.is_empty() && user.chars().all(|c|c.is_ascii_alphanumeric() || c=='-') { return user.into(); }
    let hash=ring::digest::digest(&ring::digest::SHA256,user.as_bytes());
    format!("~{}",hash.as_ref().iter().map(|v|format!("{v:02x}")).collect::<String>())
}

#[derive(Clone)]
pub struct Store {
    dir: PathBuf,
}

impl Store {
    pub fn new(dir: PathBuf) -> Self {
        Self { dir }
    }

    fn path(&self, user: &str) -> PathBuf {
        self.dir.join(format!("{}.json",account_key(user)))
    }

    pub fn load(&self, user: &str) -> Snapshot {
        std::fs::read(self.path(user))
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default()
    }

    /// Writes atomically so a crash never leaves a truncated layout behind.
    pub fn save(&self, user: &str, snapshot: &Snapshot) -> bool {
        let path = self.path(user);
        let tmp = path.with_extension("json.tmp");
        let result = std::fs::create_dir_all(&self.dir)
            .and_then(|_| std::fs::write(&tmp, serde_json::to_vec_pretty(snapshot).unwrap_or_default()))
            .and_then(|_| std::fs::rename(&tmp, &path));
        match result {
            Ok(()) => true,
            Err(err) => { tracing::warn!("save layout for {user} failed: {err}"); false }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn account_paths_do_not_alias_and_unambiguous_legacy_names_stay_put() {
        assert_eq!(account_key("user-123"),"user-123");
        let values=["a.b","a_b","a/b","a%b","用户", "", "../root"];
        let unique:std::collections::HashSet<_>=values.iter().map(|v|account_key(v)).collect();
        assert_eq!(unique.len(),values.len());
        assert!(unique.iter().all(|v|v.starts_with('~') && v.len()==65 && !v.contains('/')));
    }

    #[test]
    fn layouts_saved_before_the_clipboard_setting_existed_get_it_on() {
        let old: Snapshot = serde_json::from_str(r#"{"devices":{},"settings":{"swap_cmd_ctrl":false}}"#).unwrap();
        assert!(old.settings.clipboard && !old.settings.swap_cmd_ctrl);
        let explicit: Snapshot = serde_json::from_str(r#"{"settings":{"swap_cmd_ctrl":true,"clipboard":false}}"#).unwrap();
        assert!(!explicit.settings.clipboard, "an explicit choice is kept");
        assert!(Snapshot::default().settings.clipboard);
    }
}
