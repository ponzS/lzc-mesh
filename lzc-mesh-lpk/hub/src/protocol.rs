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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ScrollSource {
    Wheel,
    Finger,
    /// The sending OS already supplies the complete stream, including inertia.
    /// Receivers must not generate another fling on finger release.
    Continuous,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ScrollPhase {
    Began,
    Changed,
    Ended,
    Cancelled,
    MayBegin,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DirectPeer {
    pub id: String,
    pub certificate: String,
    pub addresses: Vec<String>,
    #[serde(default)]
    pub bluetooth: Option<serde_json::Value>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum TransportMode { #[default] Auto, Direct, Bluetooth, Relay }

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Scroll {
    /// Existing wire units: 1/120 notch, or 12 units per continuous point.
    pub dx: f64,
    pub dy: f64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<ScrollSource>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub phase: Option<ScrollPhase>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub momentum_phase: Option<ScrollPhase>,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub stop_x: bool,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub stop_y: bool,
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
    DeviceHello { id: String, name: String, platform: Platform, w: f64, h: f64, #[serde(default)] version: String, public_key: String },
    DeviceProof { signature: String },
    DeviceTrust { id: String, fingerprint: String, approved: bool },
    /// The physical cursor of this device is pushing against a screen edge.
    /// `pos` is the 0..1 position along that edge.
    Edge { dir: Dir, pos: f64 },
    Motion { dx: f64, dy: f64 },
    Button { button: u8, down: bool },
    /// Units of 1/120 notch; positive `dy` scrolls content towards the top.
    Wheel { #[serde(flatten)] scroll: Scroll },
    /// `code` is a Linux evdev key code; every client translates to and from it.
    Key { code: u32, down: bool },
    /// The controlling device gave up control (hotkey, lost event tap...).
    Release,
    /// A verified receiver observed real local input. Bound to the active control ID.
    Takeover { control_id: String },
    /// Move a device in the layout; the hub snaps it to its neighbours.
    Layout { id: String, x: f64, y: f64 },
    /// Remove an offline device from the layout.
    Forget { id: String },
    /// Pause or resume keyboard/mouse sharing for this device (tray switch). A paused
    /// device stays online for clipboard, files and layout but never takes part in control.
    Sharing { enabled: bool },
    /// Account-scoped client dormancy, independent of the keyboard/mouse switch.
    DeviceEnabled { id: String, enabled: bool },
    /// Partial settings update from the layout page; omitted fields keep their value.
    Settings {
        #[serde(default)]
        swap_cmd_ctrl: Option<bool>,
        #[serde(default)]
        clipboard: Option<bool>,
        #[serde(default)]
        transport_mode: Option<TransportMode>,
        #[serde(default)]
        input_enabled: Option<bool>,
    },
    DirectHello { version: u32, certificate: String, addresses: Vec<String>, #[serde(default)] bluetooth: Option<serde_json::Value> },
    DirectBegin { dir: Dir, pos: f64 },
    DirectAlive { session: String },
    DirectRoute { session: String, target: String, x: f64, y: f64 },
    DirectLegacy { session: String, target: String, x: f64, y: f64 },
    DirectStop { session: String, #[serde(default)] warp: bool },
    DirectRelay { to: String, data: serde_json::Value },
    DirectStatus { status: String, #[serde(default)] channel: Option<String> },
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
    /// Online and taking part in keyboard/mouse sharing.
    pub sharing: bool,
    pub enabled: bool,
    pub trusted: bool,
    pub fingerprint: String,
    pub approval_status: String,
    pub version: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Settings {
    pub swap_cmd_ctrl: bool,
    /// Shared clipboard between the account's devices. On by default; can be turned off on
    /// the layout page or from a client's tray menu.
    #[serde(default = "default_true")]
    pub clipboard: bool,
    #[serde(default)]
    pub transport_mode: TransportMode,
    #[serde(default = "default_true")]
    pub input_enabled: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self { swap_cmd_ctrl: true, clipboard: true, transport_mode: TransportMode::Auto, input_enabled: true }
    }
}

fn default_true() -> bool {
    true
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
    pub control_id: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "t", rename_all = "snake_case")]
pub enum HubMsg {
    Welcome { id: String, user: String, auth_version: u32 },
    DeviceChallenge { user: String, nonce: String },
    DeviceAuth { status: String, fingerprint: String, #[serde(skip_serializing_if = "Option::is_none")] token: Option<String> },
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
    Wheel { #[serde(flatten)] scroll: Scroll },
    /// `code` is a Linux evdev key code.
    Key { code: u32, down: bool },
    Leave,
    /// Clipboard text copied on another device; `from` is its display name.
    Clipboard { text: String, from: String },
    /// The shared file list changed.
    Files { files: Vec<FileView> },
    Pong,
    DirectPeers { peers: Vec<DirectPeer> },
    DirectSession { session: String, source: String, target: String, x: f64, y: f64 },
    DirectLease { session: String },
    DirectEnd { session: String, warp: bool },
    DirectRelay { from: String, data: serde_json::Value },
    TransportStatus { id: String, status: String, #[serde(skip_serializing_if = "Option::is_none")] channel: Option<String> },
    Error { message: String },
}
