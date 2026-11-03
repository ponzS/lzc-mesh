//! Per-user rooms. A room owns the layout of one account's devices and routes
//! input from the controlling device to the device under the shared cursor.

use std::collections::{HashMap, HashSet};
#[cfg(test)]
use std::collections::BTreeMap;
use std::sync::Arc;

use tokio::sync::{mpsc, watch, Mutex};

use crate::layout;
use crate::protocol::{ActiveView, ClientMsg, DeviceView, Dir, FileView, HubMsg, Platform, DirectPeer, TransportMode};
use crate::store::{Device, Snapshot, Store};
#[path = "device_auth.rs"]
mod device_auth;

pub type Tx = mpsc::UnboundedSender<HubMsg>;

/// Largest clipboard text relayed between devices (bytes).
pub const MAX_CLIPBOARD_BYTES: usize = 256 * 1024;

const KEY_ESC: u32 = 1;
const KEY_LEFTCTRL: u32 = 29;
const KEY_LEFTALT: u32 = 56;
const KEY_RIGHTCTRL: u32 = 97;
const KEY_LEFTMETA: u32 = 125;
const KEY_RIGHTMETA: u32 = 126;

struct Conn {
    /// Device id, or None for a layout viewer (web page).
    device: Option<String>,
    manager: bool,
    native: bool,
    verified_key: Option<String>,
    challenge: Option<device_auth::Challenge>,
    file_token: Option<String>,
    tx: Tx,
    /// Keyboard/mouse sharing switched on in the client (tray switch).
    sharing: bool,
    direct: Option<DirectPeer>,
    transport_status: String,
    channel_status: HashMap<String,String>,
}

struct Active {
    source: String,
    target: String,
    /// Shared cursor in global layout coordinates.
    x: f64,
    y: f64,
    /// Keys and buttons currently held on the target, released on hand-over.
    keys: HashSet<u32>,
    buttons: HashSet<u8>,
    /// Physical keys held on the controlling keyboard, before any remapping.
    src_keys: HashSet<u32>,
    session: Option<String>,
    control_id: String,
}

pub struct Room {
    user: String,
    store: Store,
    snapshot: Snapshot,
    conns: HashMap<u64, Conn>,
    active: Option<Active>,
    device_watches: HashMap<String, watch::Sender<bool>>,
}

#[derive(Clone)]
pub struct Hub {
    store: Store,
    rooms: Arc<Mutex<HashMap<String, Arc<Mutex<Room>>>>>,
}

impl Hub {
    pub fn new(store: Store) -> Self {
        Self { store, rooms: Arc::default() }
    }

    pub async fn room(&self, user: &str) -> Arc<Mutex<Room>> {
        let mut rooms = self.rooms.lock().await;
        rooms
            .entry(user.to_string())
            .or_insert_with(|| {
                Arc::new(Mutex::new(Room {
                    user: user.to_string(),
                    store: self.store.clone(),
                    snapshot: self.store.load(user),
                    conns: HashMap::new(),
                    active: None,
                    device_watches: HashMap::new(),
                }))
            })
            .clone()
    }
}

fn norm(d: &Device, x: f64, y: f64) -> (f64, f64) {
    (((x - d.x) / d.w).clamp(0.0, 1.0), ((y - d.y) / d.h).clamp(0.0, 1.0))
}

impl Room {
    pub fn attach(&mut self, conn: u64, tx: Tx) {
        let _ = tx.send(HubMsg::Welcome { id: String::new(), user: self.user.clone(), auth_version: 1 });
        self.conns.insert(conn, Conn { device: None, manager: false, native: false, verified_key: None, challenge: None, file_token: None, tx, sharing: true, direct: None, transport_status: String::new(), channel_status: HashMap::new() });
        self.broadcast();
    }

    pub fn detach(&mut self, conn: u64) {
        let Some(old) = self.conns.remove(&conn) else { return };
        if let Some(id) = old.device {
            if !self.is_online(&id) {
                self.drop_device_from_active(&id);
            }
            self.update_lease(&id);
        }
        self.broadcast();
        self.broadcast_peers();
    }

    fn is_online(&self, id: &str) -> bool {
        self.conns.values().any(|c| c.device.as_deref() == Some(id))
    }

    fn send_to(&self, id: &str, msg: HubMsg) {
        for c in self.conns.values().filter(|c| c.device.as_deref() == Some(id) && self.trusted_conn(c)) {
            let _ = c.tx.send(msg.clone());
        }
    }

    fn reply(&self, conn: u64, msg: HubMsg) {
        if let Some(c) = self.conns.get(&conn) {
            let _ = c.tx.send(msg);
        }
    }

    pub fn handle(&mut self, conn: u64, msg: ClientMsg) {
        let device = self.conns.get(&conn).and_then(|c| c.device.clone());
        let registration = matches!(&msg,ClientMsg::Hello{..}|ClientMsg::DeviceHello{..}|ClientMsg::DeviceProof{..}|ClientMsg::Ping);
        let own_preference = matches!(&msg,ClientMsg::Sharing{..}) && device.is_some()
            || matches!(&msg,ClientMsg::DeviceEnabled{id,..} if Some(id)==device.as_ref());
        if !registration && !own_preference && !self.manager_or_trusted(conn) {
            self.reply(conn,HubMsg::Error{message:"approved device or account management required".into()});return;
        }
        match msg {
            ClientMsg::Hello { id, name, platform, w, h, version } => {
                if platform!=Platform::Web && device.as_deref()==Some(&id) && self.conns.get(&conn).is_some_and(|c|c.verified_key.is_some()) {
                    self.hello(conn,id,name,platform,w,h,version);self.broadcast();
                } else if platform==Platform::Web && self.conns.get(&conn).is_some_and(|c|!c.native && c.device.is_none()) {
                    if let Some(c)=self.conns.get_mut(&conn){c.manager=true;}
                    self.broadcast();
                } else {
                    if platform!=Platform::Web {if let Some(c)=self.conns.get_mut(&conn){c.native=true;}}
                    self.reply(conn,HubMsg::Error{message:"device credentials required; upgrade this client".into()});
                }
            }
            ClientMsg::DeviceHello{id,name,platform,w,h,version,public_key} => self.device_hello(conn,id,name,platform,w,h,version,public_key),
            ClientMsg::DeviceProof{signature} => self.device_proof(conn,signature),
            ClientMsg::DeviceTrust{id,fingerprint,approved} => self.device_trust(conn,id,fingerprint,approved),
            ClientMsg::Ping => self.reply(conn, HubMsg::Pong),
            ClientMsg::Takeover { control_id } => {
                if let Some(id) = device {
                    if self.sharing_ids().contains(id.as_str()) && self.active.as_ref().is_some_and(|a| a.target == id && a.control_id == control_id) {
                        self.stop(true);
                    }
                }
            }
            ClientMsg::Layout { id, x, y } => self.move_device(&id, x, y),
            ClientMsg::Forget { id } => self.forget(conn, &id),
            ClientMsg::DeviceEnabled { id, enabled } => {
                let Some(d) = self.snapshot.devices.get_mut(&id) else {
                    self.reply(conn, HubMsg::Error { message: "unknown device in this account".into() });
                    return;
                };
                if d.enabled != enabled {
                    d.enabled = enabled;
                    self.update_lease(&id);
                    self.drop_device_from_active(&id);
                    for c in self.conns.values_mut().filter(|c| c.device.as_deref() == Some(&id)) {
                        c.direct = None;
                        c.transport_status.clear(); c.channel_status.clear();
                    }
                    self.store.save(&self.user, &self.snapshot);
                    self.broadcast_peers();
                }
                self.broadcast();
            }
            ClientMsg::Sharing { enabled } => {
                if let Some(c) = self.conns.get_mut(&conn) {
                    c.sharing = enabled;
                }
                if let (false, Some(id)) = (enabled, device.as_deref()) {
                    self.drop_device_from_active(id);
                }
                self.broadcast();
            }
            ClientMsg::Settings { swap_cmd_ctrl, clipboard, transport_mode, input_enabled } => {
                if let Some(v) = swap_cmd_ctrl {
                    self.snapshot.settings.swap_cmd_ctrl = v;
                }
                if let Some(v) = clipboard {
                    self.snapshot.settings.clipboard = v;
                }
                if let Some(v) = transport_mode { self.snapshot.settings.transport_mode = v; }
                if let Some(v) = input_enabled {
                    self.snapshot.settings.input_enabled = v;
                    if !v { self.stop(true); }
                }
                self.store.save(&self.user, &self.snapshot);
                self.broadcast();
            }
            ClientMsg::DirectHello { version, certificate, addresses, bluetooth } => {
                if version != 1 || certificate.len() > 16384 || addresses.len() > 16 { return; }
                let bluetooth=bluetooth.filter(|v| {
                    v.to_string().len()<512 && (v["role"]=="connector" || (v["role"]=="listener" && v["channel"].as_u64().is_some_and(|n|(1..=30).contains(&n)) && v["address"].as_str().is_some_and(|s|s.len()==17 && s.split(':').count()==6 && s.split(':').all(|p|p.len()==2 && u8::from_str_radix(p,16).is_ok()))))
                });
                if let Some(id) = device {
                    if !self.device_enabled(&id) { return; }
                    let addresses = addresses.into_iter().filter(|s| {
                        s.parse::<std::net::SocketAddr>().is_ok_and(|addr| matches!(addr.ip(), std::net::IpAddr::V4(ip) if ip.is_private()) && addr.port() > 0)
                    }).collect();
                    if let Some(c) = self.conns.get_mut(&conn) { c.direct = Some(DirectPeer { id, certificate, addresses, bluetooth }); }
                    self.broadcast_peers();
                }
            }
            ClientMsg::DirectBegin { dir, pos } => {
                if let Some(id) = device {
                    if self.conns.get(&conn).is_some_and(|c| c.direct.is_some()) { self.begin_direct(&id, dir, pos); }
                }
            }
            ClientMsg::DirectAlive { session } => {
                if let Some(id) = device {
                    if self.active.as_ref().is_some_and(|a| a.source == id && a.session.as_deref() == Some(&session)) { self.direct_broadcast(HubMsg::DirectLease { session }); }
                }
            }
            ClientMsg::DirectStop { session, warp } => {
                if let Some(id) = device {
                    if self.active.as_ref().is_some_and(|a| a.source == id && a.session.as_deref() == Some(&session)) { self.stop(warp); }
                }
            }
            ClientMsg::DirectRoute { session, target, x, y } => {
                if let Some(id) = device { self.direct_route(&id, &session, &target, x, y, false); }
            }
            ClientMsg::DirectLegacy { session, target, x, y } => {
                if let Some(id) = device { self.direct_route(&id, &session, &target, x, y, true); }
            }
            ClientMsg::DirectRelay { to, data } => {
                if let Some(id) = device { self.direct_relay(&id, &to, data); }
            }
            ClientMsg::DirectStatus { status, channel } => {
                if channel.as_deref().is_some_and(|s|s!="pointer" && s!="keyboard"){return;}
                if let Some(id) = device {
                    let status: String = status.chars().take(120).collect();
                    if let Some(c) = self.conns.get_mut(&conn) {
                        if let Some(channel)=&channel {c.channel_status.insert(channel.clone(),status.clone());}
                        else {c.transport_status=status.clone();}
                    }
                    for c in self.conns.values() { let _ = c.tx.send(HubMsg::TransportStatus { id: id.clone(), status: status.clone(), channel: channel.clone() }); }
                }
            }
            ClientMsg::Clipboard { text } => {
                if let Some(id) = device {
                    self.clipboard(&id, text);
                }
            }
            other => {
                if let Some(id) = device {
                    self.input(&id, other);
                }
            }
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn hello(&mut self, conn: u64, id: String, name: String, platform: Platform, w: f64, h: f64, version: String) -> bool {
        if platform == Platform::Web || id.is_empty() {
            return false;
        }
        let valid = id.len() <= 64 && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_');
        if !valid || !(w.is_finite() && h.is_finite() && w >= 100.0 && h >= 100.0 && w < 100_000.0 && h < 100_000.0) {
            self.reply(conn, HubMsg::Error { message: "invalid device hello".into() });
            return false;
        }
        let previous=self.snapshot.clone();
        let name: String = name.chars().take(64).collect();
        let existing = self.snapshot.devices.get(&id).map(|d| (d.x, d.y));
        let (x, y) = existing.unwrap_or_else(|| layout::place_new(&self.snapshot.devices));
        let resized = self.snapshot.devices.get(&id).is_some_and(|d| d.w != w || d.h != h);
        let enabled = self.snapshot.devices.get(&id).is_none_or(|d| d.enabled);
        self.snapshot.devices.insert(id.clone(), Device { id: id.clone(), name, platform, w, h, x, y, version, enabled });
        if resized {
            // Keep the layout valid after a resolution change.
            let (sx, sy) = layout::snap(&self.snapshot.devices, &id, x, y);
            if let Some(d) = self.snapshot.devices.get_mut(&id) {
                (d.x, d.y) = (sx, sy);
            }
        }
        if !self.store.save(&self.user, &self.snapshot) {
            self.snapshot=previous;
            self.reply(conn,HubMsg::Error{message:"cannot persist device identity".into()});return false;
        }
        // Only a proved matching key reaches this method. Retire an older
        // connection so a reconnect cannot duplicate input or keep old tokens.
        let fingerprint=self.auth_status(&id).1;
        for (n,c) in &mut self.conns {
            if *n!=conn && c.device.as_deref()==Some(&id) {
                let _=c.tx.send(HubMsg::DeviceAuth{status:"replaced".into(),fingerprint:fingerprint.clone(),token:None});
                c.device=None;c.verified_key=None;c.file_token=None;c.direct=None;
                c.transport_status.clear();c.channel_status.clear();
            }
        }
        self.drop_device_from_active(&id);
        if let Some(c) = self.conns.get_mut(&conn) {
            c.device = Some(id.clone());
            let _ = c.tx.send(HubMsg::Welcome { id, user: self.user.clone(), auth_version: 1 });
        }
        // Registration publishes state only after the connection token is issued.
        true
    }

    fn move_device(&mut self, id: &str, x: f64, y: f64) {
        if !self.snapshot.devices.contains_key(id) || !x.is_finite() || !y.is_finite() {
            return;
        }
        let (sx, sy) = layout::snap(&self.snapshot.devices, id, x, y);
        if let Some(d) = self.snapshot.devices.get_mut(id) {
            (d.x, d.y) = (sx, sy);
        }
        self.store.save(&self.user, &self.snapshot);
        self.stop(true);
        self.broadcast();
    }

    fn forget(&mut self, conn: u64, id: &str) {
        if self.is_online(id) {
            self.reply(conn, HubMsg::Error { message: "only offline devices can be removed".into() });
            return;
        }
        self.snapshot.devices.remove(id);
        self.snapshot.bindings.remove(id);
        self.device_watches.remove(id);
        self.store.save(&self.user, &self.snapshot);
        self.broadcast();
    }

    /// Relays clipboard text to the account's other online devices. Never stored.
    fn clipboard(&self, from: &str, text: String) {
        if !self.device_enabled(from) || !self.snapshot.settings.clipboard || text.is_empty() || text.len() > MAX_CLIPBOARD_BYTES {
            return;
        }
        let name = self.snapshot.devices.get(from).map(|d| d.name.clone()).unwrap_or_default();
        let msg = HubMsg::Clipboard { text, from: name };
        for c in self.conns.values() {
            if self.trusted_conn(c) && c.device.as_deref().is_some_and(|id| id != from && self.device_enabled(id)) {
                let _ = c.tx.send(msg.clone());
            }
        }
    }

    /// Pushes the shared file list to every connection (devices and pages).
    pub fn files_changed(&self, files: Vec<FileView>) {
        let msg = HubMsg::Files { files };
        for c in self.conns.values().filter(|c| c.manager || (self.trusted_conn(c) && c.device.as_deref().is_some_and(|id| self.device_enabled(id)))) {
            let _ = c.tx.send(msg.clone());
        }
    }

    pub fn device_enabled(&self, id: &str) -> bool {
        self.snapshot.devices.get(id).is_some_and(|d| d.enabled) && self.snapshot.bindings.get(id).is_some_and(|b|b.approved)
    }

    pub fn device_watch(&mut self, id: &str) -> Option<watch::Receiver<bool>> {
        self.snapshot.devices.get(id)?;
        let enabled = self.device_enabled(id);
        Some(self.device_watches.entry(id.to_string()).or_insert_with(|| watch::channel(enabled).0).subscribe())
    }

    /// Devices that are online and take part in keyboard/mouse sharing.
    fn online(&self) -> impl Iterator<Item = &Device> + Clone {
        let sharing = self.sharing_ids();
        self.snapshot.devices.values().filter(move |d| sharing.contains(d.id.as_str()))
    }

    fn sharing_ids(&self) -> HashSet<&str> {
        // A device is paused only when all of its connections have sharing off.
        self.conns.values().filter(|c| c.sharing && c.device.as_deref().is_some_and(|id| self.device_enabled(id))).filter_map(|c| c.device.as_deref()).collect()
    }

    fn broadcast(&self) {
        let online: HashSet<&str> = self.conns.values().filter_map(|c| c.device.as_deref()).collect();
        let sharing = self.sharing_ids();
        let devices: Vec<DeviceView> = self
            .snapshot
            .devices
            .values()
            .map(|d| DeviceView {
                id: d.id.clone(),
                name: d.name.clone(),
                platform: d.platform,
                w: d.w,
                h: d.h,
                x: d.x,
                y: d.y,
                online: online.contains(d.id.as_str()),
                sharing: sharing.contains(d.id.as_str()),
                enabled: d.enabled,
                trusted: self.snapshot.bindings.get(&d.id).is_some_and(|b|b.approved),
                fingerprint: self.auth_status(&d.id).1,
                approval_status: self.auth_status(&d.id).0.into(),
                version: d.version.clone(),
            })
            .collect();
        let active = self.active.as_ref().map(|a| ActiveView { source: a.source.clone(), target: a.target.clone(), control_id: a.control_id.clone() });
        let state = HubMsg::State { devices: devices.clone(), active, settings: self.snapshot.settings.clone() };
        for c in self.conns.values() {
            if !c.manager && !self.trusted_conn(c) {
                if let Some(id)=&c.device {
                    let own=devices.iter().filter(|d|d.id==*id).cloned().collect();
                    let _=c.tx.send(HubMsg::State{devices:own,active:None,settings:self.snapshot.settings.clone()});
                    let _=c.tx.send(HubMsg::Edges{left:false,right:false,up:false,down:false});
                }
                continue;
            }
            let _ = c.tx.send(state.clone());
            for peer in self.conns.values() {
                if let Some(id) = &peer.device {
                    if !peer.transport_status.is_empty() { let _ = c.tx.send(HubMsg::TransportStatus { id: id.clone(), status: peer.transport_status.clone(), channel: None }); }
                    for (channel,status) in &peer.channel_status {let _=c.tx.send(HubMsg::TransportStatus {id:id.clone(),status:status.clone(),channel:Some(channel.clone())});}
                }
            }
            if let Some(id) = &c.device {
                if let Some(me) = self.snapshot.devices.get(id) {
                    let [left, right, up, down] = if c.sharing && self.snapshot.settings.input_enabled { layout::edges(self.online(), me) } else { [false; 4] };
                    let _ = c.tx.send(HubMsg::Edges { left, right, up, down });
                }
            }
        }
    }

    fn drop_device_from_active(&mut self, id: &str) {
        let involved = self.active.as_ref().is_some_and(|a| a.source == id || a.target == id);
        if involved {
            self.stop(false);
        }
    }

    /// Ends remote control. `warp` returns the source cursor to its centre.
    fn stop(&mut self, warp: bool) {
        let Some(a) = self.active.take() else { return };
        if let Some(session) = a.session.clone() {
            self.direct_broadcast(HubMsg::DirectEnd { session, warp });
            self.broadcast();
            return;
        }
        self.release_held(&a);
        self.send_to(&a.target, HubMsg::Leave);
        self.send_to(&a.source, HubMsg::Release { x: 0.5, y: 0.5, warp });
        self.broadcast();
    }

    fn release_held(&self, a: &Active) {
        for &button in &a.buttons {
            self.send_to(&a.target, HubMsg::Button { button, down: false });
        }
        for &code in &a.keys {
            self.send_to(&a.target, HubMsg::Key { code, down: false });
        }
    }

    fn input(&mut self, from: &str, msg: ClientMsg) {
        if !self.snapshot.settings.input_enabled { return; }
        if matches!(self.snapshot.settings.transport_mode, TransportMode::Direct | TransportMode::Bluetooth) && !matches!(msg, ClientMsg::Release) { return; }
        match msg {
            ClientMsg::Edge { dir, pos } => self.edge(from, dir, pos),
            ClientMsg::Release => {
                if self.active.as_ref().is_some_and(|a| a.source == from) {
                    self.stop(false);
                }
            }
            msg => {
                if !self.active.as_ref().is_some_and(|a| a.source == from) {
                    return;
                }
                match msg {
                    ClientMsg::Motion { dx, dy } => self.motion(dx, dy),
                    ClientMsg::Button { button, down } => {
                        let a = self.active.as_mut().unwrap();
                        if down { a.buttons.insert(button) } else { a.buttons.remove(&button) };
                        let target = a.target.clone();
                        self.send_to(&target, HubMsg::Button { button, down });
                    }
                    ClientMsg::Wheel { scroll } => {
                        let target = self.active.as_ref().unwrap().target.clone();
                        self.send_to(&target, HubMsg::Wheel { scroll });
                    }
                    ClientMsg::Key { code, down } => self.key(code, down),
                    _ => {}
                }
            }
        }
    }

    fn direct_broadcast(&self, msg: HubMsg) {
        for c in self.conns.values().filter(|c| c.direct.is_some() && self.trusted_conn(c)) { let _ = c.tx.send(msg.clone()); }
    }

    fn broadcast_peers(&self) {
        let mut map = HashMap::new();
        for c in self.conns.values() {
            if self.trusted_conn(c) && c.device.as_deref().is_some_and(|id|self.device_enabled(id)) {
                if let Some(peer) = &c.direct { map.insert(peer.id.clone(), peer.clone()); }
            }
        }
        self.direct_broadcast(HubMsg::DirectPeers { peers: map.into_values().collect() });
    }

    fn begin_direct(&mut self, from: &str, dir: Dir, pos: f64) {
        if !self.snapshot.settings.input_enabled || self.active.is_some() || !self.sharing_ids().contains(from) || !pos.is_finite() { return; }
        let Some(src) = self.snapshot.devices.get(from).cloned() else { return };
        let pos = pos.clamp(0.0, 1.0);
        let (gx, gy) = match dir {
            Dir::Left => (src.x, src.y + pos * src.h), Dir::Right => (src.right(), src.y + pos * src.h),
            Dir::Up => (src.x + pos * src.w, src.y), Dir::Down => (src.x + pos * src.w, src.bottom()),
        };
        let Some(target) = layout::neighbour(self.online(), &src, dir, gx, gy).cloned() else { return };
        if !self.conns.values().any(|c| c.device.as_deref() == Some(&target.id) && c.direct.is_some()) { if !matches!(self.snapshot.settings.transport_mode, TransportMode::Direct | TransportMode::Bluetooth) { self.edge(from, dir, pos); } return; }
        use std::io::Read;
        let mut random = [0u8; 32];
        if std::fs::File::open("/dev/urandom").and_then(|mut f| f.read_exact(&mut random)).is_err() { return; }
        let session: String = random.iter().map(|b| format!("{b:02x}")).collect();
        let (x, y) = entry_point(&target, dir, gx, gy);
        self.active = Some(Active { source: from.into(), target: target.id.clone(), x, y, keys: HashSet::new(), buttons: HashSet::new(), src_keys: HashSet::new(), session: Some(session.clone()), control_id: session.clone() });
        self.direct_broadcast(HubMsg::DirectSession { session, source: from.into(), target: target.id, x, y });
        self.broadcast();
    }

    fn direct_route(&mut self, from: &str, session: &str, target: &str, x: f64, y: f64, legacy: bool) {
        if !self.snapshot.settings.input_enabled || (legacy && matches!(self.snapshot.settings.transport_mode, TransportMode::Direct | TransportMode::Bluetooth)) || !x.is_finite() || !y.is_finite() || !self.sharing_ids().contains(target) { return; }
        if !self.active.as_ref().is_some_and(|a| a.source == from && a.session.as_deref() == Some(session)) { return; }
        let Some(d) = self.snapshot.devices.get(target).cloned() else { return };
        let (x, y) = (x.clamp(d.x, d.right()-1.0), y.clamp(d.y, d.bottom()-1.0));
        if legacy { self.direct_broadcast(HubMsg::DirectEnd { session: session.into(), warp: false }); }
        let a = self.active.as_mut().unwrap();
        a.target = target.into(); a.x = x; a.y = y;
        if legacy {
            a.session = None;
            let (x, y) = norm(&d, x, y);
            self.send_to(target, HubMsg::Enter { x, y });
        }
        self.broadcast();
    }

    fn direct_relay(&self, from: &str, to: &str, data: serde_json::Value) {
        if !self.snapshot.settings.input_enabled || matches!(self.snapshot.settings.transport_mode, TransportMode::Direct | TransportMode::Bluetooth) || !self.is_online(to) || data.to_string().len() > 8192 { return; }
        let Some(a) = &self.active else { return };
        let Some(session) = &a.session else { return };
        let allowed = match data["kind"].as_str() {
            Some("input") => from == a.source && data["input"]["session"] == *session && data["input"]["source"] == from && data["input"]["target"] == to,
            Some("ack" | "motion_ack") => to == a.source && data["session"] == *session && data["source"] == to && data["target"] == from,
            _ => false,
        };
        if allowed { self.send_to(to, HubMsg::DirectRelay { from: from.into(), data }); }
    }

    fn platform(&self, id: &str) -> Option<Platform> {
        self.snapshot.devices.get(id).map(|d| d.platform)
    }

    fn key(&mut self, code: u32, down: bool) {
        let a = self.active.as_mut().unwrap();
        if down { a.src_keys.insert(code) } else { a.src_keys.remove(&code) };
        // Escape hatch: Ctrl+Alt+Esc on the controlling keyboard always returns control.
        if code == KEY_ESC && down && a.src_keys.contains(&KEY_LEFTCTRL) && a.src_keys.contains(&KEY_LEFTALT) {
            self.stop(true);
            return;
        }
        let a = self.active.as_ref().unwrap();
        let mac_source = self.platform(&a.source) == Some(Platform::Macos);
        let mac_target = self.platform(&a.target) == Some(Platform::Macos);
        let code = if self.snapshot.settings.swap_cmd_ctrl && mac_source != mac_target {
            match code {
                KEY_LEFTMETA => KEY_LEFTCTRL,
                KEY_LEFTCTRL => KEY_LEFTMETA,
                KEY_RIGHTMETA => KEY_RIGHTCTRL,
                KEY_RIGHTCTRL => KEY_RIGHTMETA,
                c => c,
            }
        } else {
            code
        };
        let a = self.active.as_mut().unwrap();
        if down { a.keys.insert(code) } else { a.keys.remove(&code) };
        let target = a.target.clone();
        self.send_to(&target, HubMsg::Key { code, down });
    }

    fn edge(&mut self, from: &str, dir: Dir, pos: f64) {
        if !self.snapshot.settings.input_enabled || self.active.is_some() || !self.sharing_ids().contains(from) {
            return;
        }
        let Some(src) = self.snapshot.devices.get(from).cloned() else { return };
        let pos = pos.clamp(0.0, 1.0);
        let (gx, gy) = match dir {
            Dir::Left => (src.x, src.y + pos * src.h),
            Dir::Right => (src.right(), src.y + pos * src.h),
            Dir::Up => (src.x + pos * src.w, src.y),
            Dir::Down => (src.x + pos * src.w, src.bottom()),
        };
        let Some(target) = layout::neighbour(self.online(), &src, dir, gx, gy).cloned() else { return };
        let (x, y) = entry_point(&target, dir, gx, gy);
        let Some(control_id) = fresh_control_id() else { return };
        self.active = Some(Active {
            source: from.to_string(),
            target: target.id.clone(),
            x,
            y,
            keys: HashSet::new(),
            buttons: HashSet::new(),
            src_keys: HashSet::new(),
            session: None,
            control_id,
        });
        let (nx, ny) = norm(&target, x, y);
        self.send_to(from, HubMsg::Capture);
        self.send_to(&target.id, HubMsg::Enter { x: nx, y: ny });
        self.broadcast();
    }

    fn motion(&mut self, dx: f64, dy: f64) {
        if !dx.is_finite() || !dy.is_finite() {
            return;
        }
        let a = self.active.as_ref().unwrap();
        let Some(target) = self.snapshot.devices.get(&a.target).cloned() else { return };
        let (nx, ny) = (a.x + dx, a.y + dy);
        let exit = if nx < target.x {
            Some(Dir::Left)
        } else if nx >= target.right() {
            Some(Dir::Right)
        } else if ny < target.y {
            Some(Dir::Up)
        } else if ny >= target.bottom() {
            Some(Dir::Down)
        } else {
            None
        };
        if let Some(dir) = exit {
            let (gx, gy) = (nx.clamp(target.x, target.right()), ny.clamp(target.y, target.bottom()));
            if let Some(next) = layout::neighbour(self.online(), &target, dir, gx, gy).cloned() {
                self.hand_over(next, dir, gx, gy);
                return;
            }
        }
        let a = self.active.as_mut().unwrap();
        a.x = nx.clamp(target.x, target.right() - 1.0);
        a.y = ny.clamp(target.y, target.bottom() - 1.0);
        let (mx, my) = norm(&target, a.x, a.y);
        let id = a.target.clone();
        self.send_to(&id, HubMsg::Move { x: mx, y: my });
    }

    /// The shared cursor crosses from the current target into `next`.
    fn hand_over(&mut self, next: Device, dir: Dir, gx: f64, gy: f64) {
        let a = self.active.take().unwrap();
        self.release_held(&a);
        self.send_to(&a.target, HubMsg::Leave);
        let (x, y) = entry_point(&next, dir, gx, gy);
        if next.id == a.source {
            let (nx, ny) = norm(&next, x, y);
            self.send_to(&a.source, HubMsg::Release { x: nx, y: ny, warp: true });
        } else {
            let (nx, ny) = norm(&next, x, y);
            self.send_to(&next.id, HubMsg::Enter { x: nx, y: ny });
            // Keys still held on the controlling keyboard are re-pressed on the new target lazily by the user.
            self.active = Some(Active { source: a.source, target: next.id, x, y, keys: HashSet::new(), buttons: HashSet::new(), src_keys: a.src_keys, session: None, control_id: a.control_id });
        }
        self.broadcast();
    }

    #[cfg(test)]
    pub fn devices(&self) -> &BTreeMap<String, Device> {
        &self.snapshot.devices
    }
}

fn fresh_control_id() -> Option<String> {
    use ring::rand::{SecureRandom, SystemRandom};
    let mut bytes = [0u8; 32];
    SystemRandom::new().fill(&mut bytes).ok()?;
    Some(bytes.iter().map(|b| format!("{b:02x}")).collect())
}

/// Where the cursor appears on `d` after crossing its edge opposite to `dir`.
fn entry_point(d: &Device, dir: Dir, gx: f64, gy: f64) -> (f64, f64) {
    let inset = 2.0;
    let cx = gx.clamp(d.x, d.right() - 1.0);
    let cy = gy.clamp(d.y, d.bottom() - 1.0);
    match dir {
        Dir::Left => (d.right() - inset, cy),
        Dir::Right => (d.x + inset, cy),
        Dir::Up => (cx, d.bottom() - inset),
        Dir::Down => (cx, d.y + inset),
    }
}

#[cfg(test)]
#[path = "device_tests.rs"]
mod device_tests;
#[cfg(test)]
#[path = "handoff_tests.rs"]
mod handoff_tests;
#[cfg(test)]
#[path = "trust_tests.rs"]
mod trust_tests;

#[cfg(test)]
mod tests_support {
    pub use super::tests::{drain, hello, room};
}

#[cfg(test)]
mod tests {
    use super::*;

    pub fn room() -> Room {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        let dir = std::env::temp_dir().join(format!("lzc-mesh-test-{}-{}", std::process::id(), NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)));
        let mut r=Room { user: "u".into(), store: Store::new(dir), snapshot: Snapshot::default(), conns: HashMap::new(), active: None, device_watches: HashMap::new() };
        let (tx,_rx)=mpsc::unbounded_channel();r.attach(0,tx);
        r.handle(0,ClientMsg::Hello{id:String::new(),name:String::new(),platform:Platform::Web,w:0.0,h:0.0,version:String::new()});
        r
    }

    pub fn hello(r: &mut Room, conn: u64, id: &str, platform: Platform, w: f64, h: f64) -> mpsc::UnboundedReceiver<HubMsg> {
        let (tx, rx) = mpsc::unbounded_channel();
        r.attach(conn, tx);
        use std::sync::{Mutex as StdMutex,OnceLock};
        static KEYS:OnceLock<StdMutex<HashMap<String,String>>>=OnceLock::new();
        let mut keys=KEYS.get_or_init(||StdMutex::new(HashMap::new())).lock().unwrap();
        let stored=keys.entry(id.to_string()).or_insert_with(||crate::identity::DeviceIdentity::generate().unwrap().export()).clone();
        drop(keys);
        let key=crate::identity::DeviceIdentity::from_stored(&stored).unwrap();
        r.handle(conn,ClientMsg::DeviceHello{id:id.into(),name:id.into(),platform,w,h,version:String::new(),public_key:key.public_key()});
        let nonce=r.conns[&conn].challenge.as_ref().unwrap().nonce.clone();
        r.handle(conn,ClientMsg::DeviceProof{signature:key.proof(&r.user,id,&nonce)});
        r.handle(0,ClientMsg::DeviceTrust{id:id.into(),fingerprint:key.fingerprint(),approved:true});
        rx
    }

    pub fn drain(rx: &mut mpsc::UnboundedReceiver<HubMsg>) -> Vec<HubMsg> {
        let mut out = Vec::new();
        while let Ok(m) = rx.try_recv() {
            if !matches!(m, HubMsg::State { .. } | HubMsg::Edges { .. } | HubMsg::Welcome { .. } | HubMsg::DeviceChallenge { .. } | HubMsg::DeviceAuth { .. }) {
                out.push(m);
            }
        }
        out
    }

    #[test]
    fn cursor_crosses_from_mac_up_into_windows_and_back() {
        let mut r = room();
        let mut mac = hello(&mut r, 1, "mac", Platform::Macos, 1470.0, 956.0);
        let mut win = hello(&mut r, 2, "win", Platform::Windows, 1707.0, 960.0);
        r.handle(2, ClientMsg::Layout { id: "win".into(), x: 0.0, y: -960.0 });
        assert_eq!(r.devices()["win"].y, -960.0);
        drain(&mut mac);
        drain(&mut win);

        r.handle(1, ClientMsg::Edge { dir: Dir::Up, pos: 0.5 });
        assert!(matches!(drain(&mut mac)[..], [HubMsg::Capture]));
        let got = drain(&mut win);
        let HubMsg::Enter { y, .. } = got[0] else { panic!("{got:?}") };
        assert!(y > 0.99);

        r.handle(1, ClientMsg::Motion { dx: 0.0, dy: -100.0 });
        r.handle(1, ClientMsg::Key { code: KEY_LEFTMETA, down: true });
        let got = drain(&mut win);
        assert!(matches!(got[0], HubMsg::Move { .. }));
        assert!(matches!(got[1], HubMsg::Key { code: KEY_LEFTCTRL, down: true }));

        r.handle(1, ClientMsg::Motion { dx: 0.0, dy: 200.0 });
        let got = drain(&mut win);
        assert!(matches!(got[..], [HubMsg::Key { code: KEY_LEFTCTRL, down: false }, HubMsg::Leave]));
        assert!(matches!(drain(&mut mac)[..], [HubMsg::Release { warp: true, .. }]));
        assert!(r.active.is_none());
    }
}

#[cfg(test)]
mod escape_tests {
    use super::*;

    #[test]
    fn ctrl_alt_esc_returns_control_even_with_cmd_ctrl_swap() {
        let mut r = super::tests_support::room();
        let mut mac = super::tests_support::hello(&mut r, 1, "mac", Platform::Macos, 1470.0, 956.0);
        let mut win = super::tests_support::hello(&mut r, 2, "win", Platform::Windows, 1707.0, 960.0);
        r.handle(2, ClientMsg::Layout { id: "win".into(), x: 0.0, y: -960.0 });
        r.handle(1, ClientMsg::Edge { dir: Dir::Up, pos: 0.5 });
        for code in [KEY_LEFTCTRL, KEY_LEFTALT, KEY_ESC] {
            r.handle(1, ClientMsg::Key { code, down: true });
        }
        assert!(r.active.is_none());
        let got = super::tests_support::drain(&mut win);
        assert!(got.iter().any(|m| matches!(m, HubMsg::Key { code: KEY_LEFTMETA, down: false })));
        assert!(super::tests_support::drain(&mut mac).iter().any(|m| matches!(m, HubMsg::Release { .. })));
    }
}

#[cfg(test)]
mod scroll_tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn scroll_metadata_and_zero_distance_end_reach_only_the_controlled_device() {
        let mut r = super::tests_support::room();
        let mut mac = super::tests_support::hello(&mut r, 1, "mac-scroll", Platform::Macos, 1470.0, 956.0);
        let mut linux = super::tests_support::hello(&mut r, 2, "linux-scroll", Platform::Linux, 1920.0, 1080.0);
        r.handle(2, ClientMsg::Layout { id: "linux-scroll".into(), x: 0.0, y: -1080.0 });
        r.handle(1, ClientMsg::Edge { dir: Dir::Up, pos: 0.5 });
        super::tests_support::drain(&mut mac);
        super::tests_support::drain(&mut linux);
        let frames = [
            json!({"t":"wheel","dx":1.5,"dy":108.0,"source":"continuous","phase":"began"}),
            json!({"t":"wheel","dx":120.0,"dy":120.0,"source":"continuous","phase":"changed"}),
            json!({"t":"wheel","dx":0.0,"dy":0.0,"source":"continuous","phase":"ended","stop_x":true,"stop_y":true}),
            json!({"t":"wheel","dx":0.0,"dy":6.0,"source":"continuous","momentum_phase":"changed"}),
            json!({"t":"wheel","dx":0.0,"dy":0.0,"source":"continuous","momentum_phase":"ended","stop_x":true,"stop_y":true}),
        ];
        for frame in frames {
            r.handle(1, serde_json::from_value(frame.clone()).unwrap());
            let got = super::tests_support::drain(&mut linux);
            assert_eq!(got.len(), 1);
            assert_eq!(serde_json::to_value(&got[0]).unwrap(), frame);
            assert!(super::tests_support::drain(&mut mac).is_empty());
        }
        // The receiving device cannot inject scrolls back into the controller.
        r.handle(2, serde_json::from_value(json!({"t":"wheel","dx":0,"dy":120})).unwrap());
        assert!(super::tests_support::drain(&mut mac).is_empty());
    }

    #[test]
    fn legacy_scroll_keeps_its_wire_shape() {
        let old = json!({"t":"wheel","dx":0.0,"dy":120.0});
        let ClientMsg::Wheel { scroll } = serde_json::from_value(old.clone()).unwrap() else { panic!() };
        assert!(scroll.source.is_none());
        assert_eq!(serde_json::to_value(HubMsg::Wheel { scroll }).unwrap(), old);
    }
}

#[cfg(test)]
mod clipboard_tests {
    use super::*;

    #[test]
    fn clipboard_is_on_by_default_can_be_turned_off_and_never_echoed() {
        let mut r = super::tests_support::room();
        let mut mac = super::tests_support::hello(&mut r, 1, "mac", Platform::Macos, 1470.0, 956.0);
        let mut win = super::tests_support::hello(&mut r, 2, "win", Platform::Windows, 1707.0, 960.0);
        let clip = |rx: &mut mpsc::UnboundedReceiver<HubMsg>| {
            let mut n = 0;
            while let Ok(m) = rx.try_recv() {
                if matches!(m, HubMsg::Clipboard { .. }) {
                    n += 1;
                }
            }
            n
        };
        assert!(r.snapshot.settings.clipboard, "clipboard sharing should be on by default");
        r.handle(1, ClientMsg::Clipboard { text: "hello".into() });
        assert_eq!(clip(&mut win), 1);
        assert_eq!(clip(&mut mac), 0, "echoed to the sender");

        r.handle(0, ClientMsg::Settings { swap_cmd_ctrl: None, clipboard: Some(false), transport_mode: None, input_enabled: None });
        assert!(!r.snapshot.settings.clipboard && r.snapshot.settings.swap_cmd_ctrl);
        r.handle(1, ClientMsg::Clipboard { text: "secret".into() });
        assert_eq!(clip(&mut win), 0, "relayed while disabled");
    }
}

#[cfg(test)]
mod sharing_tests {
    use super::*;

    #[test]
    fn paused_device_is_skipped_and_cannot_start_control() {
        let mut r = super::tests_support::room();
        let mut mac = super::tests_support::hello(&mut r, 1, "mac", Platform::Macos, 1470.0, 956.0);
        let mut win = super::tests_support::hello(&mut r, 2, "win", Platform::Windows, 1707.0, 960.0);
        r.handle(2, ClientMsg::Layout { id: "win".into(), x: 0.0, y: -960.0 });
        // Windows pauses sharing: the Mac can no longer push the cursor into it.
        r.handle(2, ClientMsg::Sharing { enabled: false });
        r.handle(1, ClientMsg::Edge { dir: Dir::Up, pos: 0.5 });
        assert!(r.active.is_none());
        // ...and pausing while being controlled ends control at once.
        r.handle(2, ClientMsg::Sharing { enabled: true });
        r.handle(1, ClientMsg::Edge { dir: Dir::Up, pos: 0.5 });
        assert!(r.active.is_some());
        super::tests_support::drain(&mut mac);
        super::tests_support::drain(&mut win);
        r.handle(2, ClientMsg::Sharing { enabled: false });
        assert!(r.active.is_none());
        assert!(super::tests_support::drain(&mut win).iter().any(|m| matches!(m, HubMsg::Leave)));
        assert!(super::tests_support::drain(&mut mac).iter().any(|m| matches!(m, HubMsg::Release { .. })));
    }
}


#[cfg(test)]
mod direct_tests {
    use super::*;
    use serde_json::json;

    fn setup() -> (Room, mpsc::UnboundedReceiver<HubMsg>, mpsc::UnboundedReceiver<HubMsg>) {
        let mut r=super::tests_support::room();
        let mac=super::tests_support::hello(&mut r,1,"mac-direct",Platform::Macos,1000.0,800.0);
        let linux=super::tests_support::hello(&mut r,2,"linux-direct",Platform::Linux,1000.0,800.0);
        r.handle(2,ClientMsg::Layout {id:"linux-direct".into(),x:1000.0,y:0.0});
        for conn in [1,2] {r.handle(conn,ClientMsg::DirectHello {version:1,certificate:String::new(),addresses:vec![],bluetooth:None});}
        (r,mac,linux)
    }
    fn data(session:&str) -> serde_json::Value {
        json!({"kind":"input","input":{"session":session,"source":"mac-direct","target":"linux-direct","seq":1,"barrier":1,"x":1002.0,"y":80.0,"event":{"t":"key","code":30,"down":true}}})
    }
    #[test]
    fn grant_and_relay_require_the_current_source_and_session() {
        let (mut r,mut mac,mut linux)=setup();
        r.handle(1,ClientMsg::DirectBegin {dir:Dir::Right,pos:0.1});
        let session=r.active.as_ref().unwrap().session.clone().unwrap();
        let initial=super::tests_support::drain(&mut mac);
        assert!(initial.iter().any(|m|matches!(m,HubMsg::DirectSession {session:s,..} if s==&session)));
        super::tests_support::drain(&mut linux);
        r.handle(2,ClientMsg::DirectRelay {to:"linux-direct".into(),data:data(&session)});
        r.handle(1,ClientMsg::DirectRelay {to:"linux-direct".into(),data:data("expired")});
        assert!(super::tests_support::drain(&mut linux).is_empty());
        r.handle(1,ClientMsg::DirectRelay {to:"linux-direct".into(),data:data(&session)});
        assert!(super::tests_support::drain(&mut linux).iter().any(|m|matches!(m,HubMsg::DirectRelay {..})));
    }
    #[test]
    fn global_pause_revokes_direct_grants_and_blocks_legacy_edges() {
        let (mut r,mut mac,mut linux)=setup();
        r.handle(1,ClientMsg::DirectBegin {dir:Dir::Right,pos:0.1});
        let session=r.active.as_ref().unwrap().session.clone().unwrap();
        super::tests_support::drain(&mut mac);super::tests_support::drain(&mut linux);
        r.handle(0,serde_json::from_value(json!({"t":"settings","input_enabled":false})).unwrap());
        assert!(r.active.is_none());
        assert!(super::tests_support::drain(&mut linux).iter().any(|m|matches!(m,HubMsg::DirectEnd {session:s,..} if s==&session)));
        r.handle(1,ClientMsg::DirectBegin {dir:Dir::Right,pos:0.1});
        r.handle(1,ClientMsg::Edge {dir:Dir::Right,pos:0.1});
        assert!(r.active.is_none());
    }
    #[test]
    fn direct_only_rejects_relay_packets_and_legacy_control() {
        let (mut r,mut mac,mut linux)=setup();
        r.handle(0,serde_json::from_value(json!({"t":"settings","transport_mode":"direct"})).unwrap());
        r.handle(1,ClientMsg::Edge {dir:Dir::Right,pos:0.1});assert!(r.active.is_none());
        r.handle(1,ClientMsg::DirectBegin {dir:Dir::Right,pos:0.1});
        let session=r.active.as_ref().unwrap().session.clone().unwrap();
        super::tests_support::drain(&mut mac);super::tests_support::drain(&mut linux);
        r.handle(1,ClientMsg::DirectRelay {to:"linux-direct".into(),data:data(&session)});
        assert!(super::tests_support::drain(&mut linux).is_empty());
    }
    #[test]
    fn viewer_cannot_publish_peer_certificates_or_inject_input() {
        let (mut r,mut mac,mut linux)=setup();
        let (tx,mut viewer)=mpsc::unbounded_channel();r.attach(3,tx);
        while viewer.try_recv().is_ok(){}
        r.handle(3,ClientMsg::DirectHello {version:1,certificate:"foreign".into(),addresses:vec!["192.168.1.2:45000".into()],bluetooth:None});
        r.handle(3,ClientMsg::DirectBegin {dir:Dir::Right,pos:0.1});
        assert!(r.active.is_none());assert!(r.conns[&3].direct.is_none());
        assert!(!super::tests_support::drain(&mut mac).iter().any(|m|matches!(m,HubMsg::DirectPeers {peers} if peers.iter().any(|p|p.certificate=="foreign"))));
        super::tests_support::drain(&mut linux);
    }
}
