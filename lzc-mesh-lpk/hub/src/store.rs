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
}

impl Device {
    pub fn right(&self) -> f64 {
        self.x + self.w
    }
    pub fn bottom(&self) -> f64 {
        self.y + self.h
    }
}

#[derive(Debug, Default, Clone, Serialize, Deserialize)]
pub struct Snapshot {
    #[serde(default)]
    pub devices: BTreeMap<String, Device>,
    #[serde(default)]
    pub settings: Settings,
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
        // User ids come from the ingress header; keep the file name strictly safe.
        let safe: String = user
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
            .collect();
        self.dir.join(format!("{safe}.json"))
    }

    pub fn load(&self, user: &str) -> Snapshot {
        std::fs::read(self.path(user))
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default()
    }

    /// Writes atomically so a crash never leaves a truncated layout behind.
    pub fn save(&self, user: &str, snapshot: &Snapshot) {
        let path = self.path(user);
        let tmp = path.with_extension("json.tmp");
        let result = std::fs::create_dir_all(&self.dir)
            .and_then(|_| std::fs::write(&tmp, serde_json::to_vec_pretty(snapshot).unwrap_or_default()))
            .and_then(|_| std::fs::rename(&tmp, &path));
        if let Err(err) = result {
            tracing::warn!("save layout for {user} failed: {err}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layouts_saved_before_the_clipboard_setting_existed_get_it_on() {
        let old: Snapshot = serde_json::from_str(r#"{"devices":{},"settings":{"swap_cmd_ctrl":false}}"#).unwrap();
        assert!(old.settings.clipboard && !old.settings.swap_cmd_ctrl);
        let explicit: Snapshot = serde_json::from_str(r#"{"settings":{"swap_cmd_ctrl":true,"clipboard":false}}"#).unwrap();
        assert!(!explicit.settings.clipboard, "an explicit choice is kept");
        assert!(Snapshot::default().settings.clipboard);
    }
}
