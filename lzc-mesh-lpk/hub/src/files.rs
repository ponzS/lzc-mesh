//! Shared files: one flat directory per account under the LPK's var directory.
//! Metadata (uploader) lives in a sidecar JSON next to the files.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use serde::{Deserialize, Serialize};

use crate::protocol::FileView;

pub const MAX_FILE_BYTES: u64 = 4 * 1024 * 1024 * 1024;
const META: &str = ".meta.json";

#[derive(Default, Serialize, Deserialize)]
struct Meta {
    #[serde(default)]
    uploader: BTreeMap<String, String>,
}

#[derive(Clone)]
pub struct Files {
    root: PathBuf,
}

/// Maps a user id to a directory name that cannot escape the root.
fn user_dir(user: &str) -> String {
    crate::store::account_key(user)
}

/// Accepts a plain file name only: no paths, no hidden names, no control characters.
pub fn safe_name(raw: &str) -> Option<String> {
    let name = raw.trim();
    let name = name.rsplit(['/', '\\']).next().unwrap_or("").trim();
    let ok = !name.is_empty()
        && name.len() <= 200
        && !name.starts_with('.')
        && !name.chars().any(|c| c.is_control() || matches!(c, ':' | '*' | '?' | '"' | '<' | '>' | '|'));
    ok.then(|| name.to_string())
}

impl Files {
    pub fn new(root: PathBuf) -> Self {
        Self { root }
    }

    pub fn dir(&self, user: &str) -> PathBuf {
        self.root.join(user_dir(user))
    }

    pub fn path(&self, user: &str, name: &str) -> Option<PathBuf> {
        safe_name(name).filter(|n| n == name).map(|n| self.dir(user).join(n))
    }

    fn read_meta(dir: &Path) -> Meta {
        std::fs::read(dir.join(META)).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
    }

    fn write_meta(dir: &Path, meta: &Meta) {
        let tmp = dir.join(".meta.json.tmp");
        if std::fs::write(&tmp, serde_json::to_vec(meta).unwrap_or_default()).is_ok() {
            let _ = std::fs::rename(&tmp, dir.join(META));
        }
    }

    pub fn list(&self, user: &str) -> Vec<FileView> {
        let dir = self.dir(user);
        let meta = Self::read_meta(&dir);
        let mut out: Vec<FileView> = std::fs::read_dir(&dir)
            .into_iter()
            .flatten()
            .flatten()
            .filter_map(|e| {
                let name = e.file_name().into_string().ok()?;
                if name.starts_with('.') {
                    return None;
                }
                let md = e.metadata().ok().filter(|m| m.is_file())?;
                let modified = md.modified().ok()?.duration_since(UNIX_EPOCH).ok()?.as_secs();
                let uploader = meta.uploader.get(&name).cloned().unwrap_or_default();
                Some(FileView { name, size: md.len(), modified, uploader })
            })
            .collect();
        out.sort_by(|a, b| b.modified.cmp(&a.modified).then_with(|| a.name.cmp(&b.name)));
        out
    }

    /// Picks "name", "name (2)", ... so an upload never overwrites another file.
    pub fn unique_name(&self, user: &str, name: &str) -> String {
        let dir = self.dir(user);
        if !dir.join(name).exists() {
            return name.to_string();
        }
        let (stem, ext) = match name.rfind('.') {
            Some(i) if i > 0 => (&name[..i], &name[i..]),
            _ => (name, ""),
        };
        (2..)
            .map(|n| format!("{stem} ({n}){ext}"))
            .find(|candidate| !dir.join(candidate).exists())
            .unwrap()
    }

    pub fn set_uploader(&self, user: &str, name: &str, uploader: &str) {
        let dir = self.dir(user);
        let mut meta = Self::read_meta(&dir);
        meta.uploader.insert(name.to_string(), uploader.chars().take(64).collect());
        Self::write_meta(&dir, &meta);
    }

    pub fn remove(&self, user: &str, name: &str) -> bool {
        let Some(path) = self.path(user, name) else { return false };
        let ok = std::fs::remove_file(&path).is_ok();
        if ok {
            let dir = self.dir(user);
            let mut meta = Self::read_meta(&dir);
            meta.uploader.remove(name);
            Self::write_meta(&dir, &meta);
        }
        ok
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_cannot_escape_the_user_directory() {
        assert_eq!(safe_name("../../etc/passwd").as_deref(), Some("passwd"));
        assert_eq!(safe_name("C:\\Users\\a\\报告.pdf").as_deref(), Some("报告.pdf"));
        assert_eq!(safe_name(".meta.json"), None);
        assert_eq!(safe_name(".."), None);
        assert_eq!(safe_name("a\nb"), None);
        let files = Files::new(PathBuf::from("/data"));
        assert!(files.path("u", "../x").is_none());
        assert_eq!(files.path("u", "x.txt"), Some(PathBuf::from("/data/u/x.txt")));
        assert_eq!(files.dir("../../root").parent(),Some(std::path::Path::new("/data")));
        assert_ne!(files.dir("a.b"),files.dir("a_b"));
    }

    #[test]
    fn duplicate_uploads_get_numbered_names() {
        let root = std::env::temp_dir().join(format!("lzc-mesh-files-{}", std::process::id()));
        let files = Files::new(root.clone());
        std::fs::create_dir_all(files.dir("u")).unwrap();
        std::fs::write(files.dir("u").join("a.txt"), b"1").unwrap();
        assert_eq!(files.unique_name("u", "a.txt"), "a (2).txt");
        assert_eq!(files.unique_name("u", "b"), "b");
        let _ = std::fs::remove_dir_all(root);
    }
}
