use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Peer {
    pub id: String,
    pub certificate: String,
    pub addresses: Vec<String>,
    #[serde(default)]
    pub bluetooth: Option<serde_json::Value>,
}

#[derive(Clone, Debug, Deserialize)]
pub struct Device {
    pub id: String,
    #[serde(default)]
    pub platform: String,
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    #[serde(default)]
    pub online: bool,
    #[serde(default = "yes")]
    pub sharing: bool,
    #[serde(default = "yes")]
    pub enabled: bool,
    #[serde(default)]
    pub trusted: bool,
}
fn yes() -> bool {
    true
}
impl Device {
    pub fn right(&self) -> f64 {
        self.x + self.w
    }
    pub fn bottom(&self) -> f64 {
        self.y + self.h
    }
    pub fn norm(&self, x: f64, y: f64) -> (f64, f64) {
        (
            ((x - self.x) / self.w).clamp(0.0, 1.0),
            ((y - self.y) / self.h).clamp(0.0, 1.0),
        )
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Grant {
    pub session: String,
    pub source: String,
    pub target: String,
    pub x: f64,
    pub y: f64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct MotionTrace {
    pub captured_us: u64,
    pub source_queue_us: u64,
    #[serde(skip)]
    pub received_us: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Input {
    pub session: String,
    pub source: String,
    pub target: String,
    /// Position sequence; absolute coordinates make intermediate motion disposable.
    pub seq: u64,
    /// Reliable action sequence for this target. Motion waits for this barrier.
    pub barrier: u64,
    pub x: f64,
    pub y: f64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub event: Option<Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trace: Option<MotionTrace>,
    #[serde(skip)]
    pub received_direct: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Wire {
    Input {
        input: Input,
    },
    Ack {
        session: String,
        source: String,
        target: String,
        barrier: u64,
    },
    MotionAck {
        session: String,
        source: String,
        target: String,
        seq: u64,
        receiver_queue_us: u64,
    },
    Ping {
        nonce: u64,
    },
    Pong {
        nonce: u64,
    },
}

pub fn neighbour<'a>(
    devices: &'a [Device],
    from: &Device,
    dir: &str,
    gx: f64,
    gy: f64,
) -> Option<&'a Device> {
    devices
        .iter()
        .filter(|d| d.online && d.enabled && d.trusted && d.sharing && d.id != from.id)
        .find(|d| match dir {
            "left" => (d.right() - from.x).abs() < 1.5 && gy >= d.y - 0.5 && gy <= d.bottom() + 0.5,
            "right" => {
                (d.x - from.right()).abs() < 1.5 && gy >= d.y - 0.5 && gy <= d.bottom() + 0.5
            }
            "up" => (d.bottom() - from.y).abs() < 1.5 && gx >= d.x - 0.5 && gx <= d.right() + 0.5,
            "down" => (d.y - from.bottom()).abs() < 1.5 && gx >= d.x - 0.5 && gx <= d.right() + 0.5,
            _ => false,
        })
}

pub fn entry(d: &Device, dir: &str, gx: f64, gy: f64) -> (f64, f64) {
    match dir {
        "left" => (d.right() - 2.0, gy.clamp(d.y, d.bottom() - 1.0)),
        "right" => (d.x + 2.0, gy.clamp(d.y, d.bottom() - 1.0)),
        "up" => (gx.clamp(d.x, d.right() - 1.0), d.bottom() - 2.0),
        _ => (gx.clamp(d.x, d.right() - 1.0), d.y + 2.0),
    }
}
