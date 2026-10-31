//! Wire protocol shared by the hub, the native clients and the web page.
//! Every frame is one JSON object tagged by `t`.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Platform {
    Macos,
    Windows,
    Linux,
    Web,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Dir {
    Left,
    Right,
    Up,
    Down,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "t", rename_all = "snake_case")]
pub enum ClientMsg {
    /// First frame. Viewers (web page) only watch and edit the layout.
    Hello {
        #[serde(default)]
        id: String,
        #[serde(default)]
        name: String,
        platform: Platform,
        #[serde(default)]
        w: f64,
        #[serde(default)]
        h: f64,
        #[serde(default)]
        version: String,
    },
    /// The physical cursor of this device is pushing against a screen edge.
    /// `pos` is the 0..1 position along that edge.
    Edge { dir: Dir, pos: f64 },
    Motion { dx: f64, dy: f64 },
    Button { button: u8, down: bool },
    /// Units of 1/120 notch; positive `dy` scrolls content towards the top.
    Wheel { dx: f64, dy: f64 },
    /// `code` is a Linux evdev key code; every client translates to and from it.
    Key { code: u32, down: bool },
    /// The controlling device gave up control (hotkey, lost event tap...).
    Release,
    /// Move a device in the layout; the hub snaps it to its neighbours.
    Layout { id: String, x: f64, y: f64 },
    /// Remove an offline device from the layout.
    Forget { id: String },
    /// Partial settings update from the layout page; omitted fields keep their value.
    Settings {
        #[serde(default)]
        swap_cmd_ctrl: Option<bool>,
        #[serde(default)]
        clipboard: Option<bool>,
    },
    /// Local clipboard text changed on this device (sent only while sharing is enabled).
    Clipboard { text: String },
    Ping,
}

#[derive(Debug, Clone, Serialize)]
pub struct DeviceView {
    pub id: String,
    pub name: String,
    pub platform: Platform,
    pub w: f64,
    pub h: f64,
    pub x: f64,
    pub y: f64,
    pub online: bool,
    pub version: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Settings {
    pub swap_cmd_ctrl: bool,
    /// Shared clipboard between the account's devices. Off unless enabled on the layout page.
    #[serde(default)]
    pub clipboard: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self { swap_cmd_ctrl: true, clipboard: false }
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct FileView {
    pub name: String,
    pub size: u64,
    /// Unix seconds.
    pub modified: u64,
    /// Device or browser that uploaded the file.
    pub uploader: String,
}

#[derive(Debug, Clone, Serialize)]
pub struct ActiveView {
    pub source: String,
    pub target: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "t", rename_all = "snake_case")]
pub enum HubMsg {
    Welcome { id: String, user: String },
    State { devices: Vec<DeviceView>, active: Option<ActiveView>, settings: Settings },
    /// Screen edges of the receiving device that lead to an online neighbour.
    Edges { left: bool, right: bool, up: bool, down: bool },
    /// You are now controlling another device: swallow local input.
    Capture,
    /// Control came back: stop swallowing; when `warp` move the cursor to (x, y), normalised.
    Release { x: f64, y: f64, warp: bool },
    /// Another device controls you; the cursor enters at (x, y), normalised.
    Enter { x: f64, y: f64 },
    Move { x: f64, y: f64 },
    Button { button: u8, down: bool },
    Wheel { dx: f64, dy: f64 },
    /// `code` is a Linux evdev key code.
    Key { code: u32, down: bool },
    Leave,
    /// Clipboard text copied on another device; `from` is its display name.
    Clipboard { text: String, from: String },
    /// The shared file list changed.
    Files { files: Vec<FileView> },
    Pong,
    Error { message: String },
}
