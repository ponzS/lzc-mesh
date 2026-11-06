use crate::{
    lan::{self, Identity},
    metrics::{self, KeyboardMetrics, Metrics},
    model::{self, Device, Grant, Input, MotionTrace, Peer, Wire},
    Callback, Command,
};
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, HashMap, HashSet, VecDeque},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};
use tokio::sync::mpsc;

struct Link {
    conn: quinn::Connection,
    stream: mpsc::Sender<Wire>,
    preferred: bool,
    received: Instant,
}
struct Pending {
    input: Input,
    created: Instant,
    hub_sent: Option<Instant>,
}
#[derive(Default)]
struct Receiver {
    barrier: u64,
    position_seq: u64,
    position: (f64, f64),
    actions: BTreeMap<u64, Input>,
    motion: Option<Input>,
    received_motion_seq: u64,
}

struct Router {
    id: String,
    cb: Callback,
    tx: mpsc::UnboundedSender<Command>,
    available: Arc<AtomicBool>,
    identity: Option<Identity>,
    bluetooth: Option<crate::bluetooth::Bluetooth>,
    peers: HashMap<String, Peer>,
    links: HashMap<String, Link>,
    devices: Vec<Device>,
    mode: String,
    enabled: bool,
    dormant: bool,
    connected: bool,
    registered: bool,
    swap: bool,
    auto_center: bool,
    grant: Option<Grant>,
    retired: VecDeque<String>,
    hub_control: Value,
    lease: Instant,
    controlled: bool,
    legacy: bool,
    seq: u64,
    barriers: HashMap<String, u64>,
    pending: BTreeMap<(String, String, u64), Pending>,
    receivers: HashMap<String, Receiver>,
    early: Vec<(String, Wire, Instant)>,
    motion: Option<Input>,
    last_motion: Option<(Input, Instant, u8)>,
    #[cfg(test)]
    drop_next_motion: bool,
    held_keys: HashSet<u64>,
    held_buttons: HashSet<u64>,
    last_heartbeat: Instant,
    last_retry: Instant,
    last_dial: Instant,
    last_lease: Instant,
    last_status: Instant,
    status: String,
    metrics: Metrics,
    keyboard: KeyboardMetrics,
    pointer_status: String,
    keyboard_status: String,
    motion_path: Option<bool>,
    motion_failed: bool,
}

pub async fn run(
    id: String,
    cb: Callback,
    tx: mpsc::UnboundedSender<Command>,
    mut rx: mpsc::UnboundedReceiver<Command>,
    available: Arc<AtomicBool>,
) {
    let now = Instant::now();
    let mut r = Router {
        id,
        cb,
        tx,
        available,
        identity: None,
        bluetooth: None,
        peers: HashMap::new(),
        links: HashMap::new(),
        devices: vec![],
        mode: "auto".into(),
        enabled: true,
        dormant: false,
        connected: false,
        registered: false,
        swap: true,
        auto_center: false, // old hubs without this setting retain edge placement
        grant: None,
        retired: VecDeque::new(),
        hub_control: Value::Null,
        lease: now,
        controlled: false,
        legacy: false,
        seq: 0,
        barriers: HashMap::new(),
        pending: BTreeMap::new(),
        receivers: HashMap::new(),
        early: vec![],
        motion: None,
        last_motion: None,
        #[cfg(test)]
        drop_next_motion: false,
        held_keys: HashSet::new(),
        held_buttons: HashSet::new(),
        last_heartbeat: now,
        last_retry: now,
        last_dial: now,
        last_lease: now,
        last_status: now,
        status: String::new(),
        metrics: Metrics::default(),
        keyboard: KeyboardMetrics::default(),
        pointer_status: String::new(),
        keyboard_status: String::new(),
        motion_path: None,
        motion_failed: false,
    };
    // Maintenance has no role in sending motion. A ready input is processed
    // immediately; only input already waiting in the queue is coalesced.
    let mut tick = tokio::time::interval(Duration::from_millis(20));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    'running: loop {
        tokio::select! {
            command=rx.recv()=> {
                let Some(command)=command else {r.disconnect();break};
                if !r.command(command){break}
                for _ in 0..63 {
                    let Ok(command)=rx.try_recv() else {break};
                    if !r.command(command){break 'running}
                }
                r.flush_motion();
            }
            _=tick.tick()=>r.tick(),
        }
    }
}

impl Router {
    fn command(&mut self, command: Command) -> bool {
        match command {
            Command::Hub(v) => self.hub(v),
            Command::Local(v) => self.local(v),
            Command::Applied(v) => self.applied(v),
            Command::Connected {
                peer,
                conn,
                initiated,
            } => self.connected(peer, conn, initiated),
            Command::Closed { peer, connection } => {
                if self
                    .links
                    .get(&peer)
                    .is_some_and(|l| l.conn.stable_id() == connection)
                {
                    self.links.remove(&peer);
                    self.last_status = Instant::now();
                }
            }
            Command::Received { peer, data, direct } => self.receive(peer, data, direct),
            Command::Disconnect => self.disconnect(),
            Command::Shutdown => {
                self.disconnect();
                return false;
            }
            #[cfg(test)]
            Command::DropLan => self.close_lan(),
            #[cfg(test)]
            Command::RestoreLan => self.advertise(),
            #[cfg(test)]
            Command::DropNextMotion => self.drop_next_motion = true,
        }
        true
    }
    fn flush_motion(&mut self) {
        if let Some(input) = self.motion.take() {
            self.last_motion = Some((input.clone(), Instant::now(), 0));
            self.send_input(&input);
        }
    }

    fn inject(&self, mut v: Value) {
        if let Some(g) = &self.grant { v["_control_session"] = json!(g.session); }
        self.cb.inject(v)
    }
    fn active_ui(&self, source: &str, target: &str) {
        self.inject(json!({"t":"direct_active","source":source,"target":target}));
    }
    fn close_lan(&mut self) {
        for (_, l) in self.links.drain() {
            l.conn.close(0u32.into(), b"LAN disabled");
        }
        if let Some(i) = self.identity.take() {
            i.endpoint.close(0u32.into(), b"LAN disabled");
        }
    }
    fn strict(&self) -> bool {
        matches!(self.mode.as_str(), "direct" | "bluetooth")
    }
    fn close_bluetooth(&mut self) {
        if let Some(mut bt) = self.bluetooth.take() {
            bt.stop();
        }
    }
    fn advertise(&mut self) {
        if !self.connected {
            return;
        }
        if self.mode == "bluetooth" {
            self.close_lan();
            if self.bluetooth.is_none() {
                match crate::bluetooth::Bluetooth::new(self.cb) {
                    Ok(bt) => {
                        self.bluetooth = Some(bt);
                        self.cb.emit(json!({"_lan":"bt_start"}));
                    }
                    Err(e) => {
                        self.cb.status(format!("蓝牙初始化失败：{e}"));
                        return;
                    }
                }
            }
            if let Some(bt) = &self.bluetooth {
                self.cb.hub(bt.advert());
            }
            return;
        }
        self.close_bluetooth();
        if self.mode == "relay" {
            self.close_lan();
            self.cb
                .hub(json!({"t":"direct_hello","version":1,"certificate":"","addresses":[]}));
            return;
        }
        if self.identity.is_none() {
            match Identity::new() {
                Ok(i) => {
                    let endpoint = i.endpoint.clone();
                    let tx = self.tx.clone();
                    tokio::spawn(async move {
                        let handshakes=std::sync::Arc::new(tokio::sync::Semaphore::new(16));
                        while let Some(incoming) = endpoint.accept().await {
                            let allowed = match incoming.remote_address().ip() {
                                std::net::IpAddr::V4(ip) => {
                                    ip.is_private() || (cfg!(test) && ip.is_loopback())
                                }
                                _ => false,
                            };
                            if !allowed {
                                incoming.refuse();
                                continue;
                            }
                            let Ok(permit)=handshakes.clone().try_acquire_owned() else {incoming.refuse();continue;};
                            let tx = tx.clone();
                            tokio::spawn(async move {
                                let _permit=permit;
                                if let Ok(Ok(conn)) =
                                    tokio::time::timeout(Duration::from_secs(2), incoming).await
                                {
                                    let _ = tx.send(Command::Connected {
                                        peer: String::new(),
                                        conn,
                                        initiated: false,
                                    });
                                }
                            });
                        }
                    });
                    self.identity = Some(i);
                }
                Err(_) => {
                    self.cb.status("局域网监听不可用，使用中枢转发".into());
                    self.cb.hub(
                        json!({"t":"direct_hello","version":1,"certificate":"","addresses":[]}),
                    );
                    return;
                }
            }
        }
        if let Some(i) = &self.identity {
            let _ = i.configure(&self.peers);
            self.cb.hub(i.advert());
        }
    }
    fn hub(&mut self, v: Value) {
        if v["t"].as_str().is_some_and(|t| t.starts_with("bt_")) {
            if self.mode != "bluetooth" || !self.enabled || !self.connected {
                return;
            }
            if let Some(bt) = &mut self.bluetooth {
                let received = bt.event(&v, &self.peers);
                if v["t"] == "bt_local" {
                    self.cb.hub(bt.advert());
                }
                for (peer, mut data) in received {
                    if let Wire::Input { input } = &mut data {
                        if let Some(trace) = &mut input.trace {
                            trace.received_us = metrics::now_us();
                        }
                    }
                    self.receive(peer, data, true);
                }
            }
            return;
        }
        match v["t"].as_str().unwrap_or("") {
            "hub_connected" => {
                self.connected = true;
                self.registered = false;
            }
            "state" => {
                self.hub_control = v["active"].clone();
                self.devices = serde_json::from_value(v["devices"].clone()).unwrap_or_default();
                self.legacy = self.grant.is_none()
                    && (v["active"]["source"] == self.id || v["active"]["target"] == self.id);
                self.swap = v["settings"]["swap_cmd_ctrl"].as_bool().unwrap_or(true);
                self.auto_center = v["settings"]["auto_center"].as_bool().unwrap_or(false);
                self.dormant = self.devices.iter().find(|d| d.id == self.id).is_none_or(|d| !d.enabled || !d.trusted);
                let enabled = !self.dormant && v["settings"]["input_enabled"].as_bool().unwrap_or(true);
                if !enabled && self.enabled {
                    self.finish(false, false);
                    self.inject(json!({"t":"release","x":0.5,"y":0.5,"warp":false}));
                    self.inject(json!({"t":"leave"}));
                }
                self.enabled = enabled;
                if v["settings"].get("transport_mode").is_some() {
                    self.available.store(true, Ordering::Release);
                }
                let mode = if enabled {
                    v["settings"]["transport_mode"].as_str().unwrap_or("auto")
                } else {
                    "relay"
                };
                let changed = mode != self.mode;
                self.mode = mode.to_string();
                if changed {
                    self.motion_path = None;
                    self.motion_failed = false;
                    self.keyboard.path = None;
                }
                if changed || !self.registered {
                    self.advertise();
                    self.registered = true;
                }
                if changed
                    && self.strict()
                    && self
                        .grant
                        .as_ref()
                        .is_some_and(|g| g.source == self.id && !self.healthy(&g.target))
                {
                    self.finish(true, true);
                }
            }
            "direct_peers" => {
                let peers: Vec<Peer> =
                    serde_json::from_value(v["peers"].clone()).unwrap_or_default();
                self.peers = peers
                    .into_iter()
                    .filter(|p| p.id != self.id)
                    .map(|p| (p.id.clone(), p))
                    .collect();
                self.available.store(true, Ordering::Release);
                let remove: Vec<String> = self
                    .links
                    .iter()
                    .filter(|(id, l)| {
                        !self.peers.get(*id).is_some_and(|p| {
                            lan::certificate(&l.conn).as_deref() == Some(p.certificate.as_str())
                        })
                    })
                    .map(|(id, _)| id.clone())
                    .collect();
                for id in remove {
                    if let Some(l) = self.links.remove(&id) {
                        l.conn.close(0u32.into(), b"peer revoked");
                    }
                }
                if let Some(i) = &self.identity {
                    let _ = i.configure(&self.peers);
                }
                if let Some(bt) = &mut self.bluetooth {
                    bt.revoke(&self.peers);
                }
                self.dial();
            }
            "direct_session" => {
                if !self.enabled {
                    return;
                }
                let Ok(mut g) = serde_json::from_value::<Grant>(v) else {
                    return;
                };
                if self.retired.contains(&g.session) { return; }
                if self
                    .grant
                    .as_ref()
                    .is_some_and(|old| old.session == g.session)
                {
                    self.lease = Instant::now();
                    return;
                }
                if self.auto_center {
                    if let Some(target) = self.devices.iter().find(|d| d.id == g.target) {
                        g.x = target.x + target.w * 0.5;
                        g.y = target.y + target.h * 0.5;
                    }
                }
                self.finish(false, false);
                self.metrics = Metrics::default();
                self.keyboard = KeyboardMetrics::default();
                self.motion_path = None;
                self.motion_failed = false;
                self.grant = Some(g.clone());
                self.lease = Instant::now();
                self.seq = 0;
                self.barriers.clear();
                self.receivers.clear();
                self.held_keys.clear();
                self.held_buttons.clear();
                self.active_ui(&g.source, &g.target);
                if g.source == self.id {
                    self.inject(json!({"t":"capture"}));
                    self.action(json!({"t":"enter"}));
                }
                let early = std::mem::take(&mut self.early);
                for (peer, data, when) in early {
                    if when.elapsed() < Duration::from_secs(1) {
                        self.receive(peer, data, false)
                    }
                }
            }
            "direct_lease" => {
                if self
                    .grant
                    .as_ref()
                    .is_some_and(|g| g.session == v["session"].as_str().unwrap_or(""))
                {
                    self.lease = Instant::now();
                }
            }
            "direct_end" => {
                if self
                    .grant
                    .as_ref()
                    .is_some_and(|g| g.session == v["session"].as_str().unwrap_or(""))
                {
                    self.finish(v["warp"].as_bool().unwrap_or(false), false);
                }
            }
            "direct_relay" => {
                if let (Some(peer), Ok(data)) = (
                    v["from"].as_str(),
                    serde_json::from_value::<Wire>(v["data"].clone()),
                ) {
                    self.receive(peer.to_string(), data, false);
                }
            }
            _ => {}
        }
    }
    fn disconnect(&mut self) {
        self.connected = false;
        self.registered = false;
        self.available.store(false, Ordering::Release);
        self.finish(false, false);
        self.close_lan();
        self.close_bluetooth();
        self.peers.clear();
        self.pending.clear();
    }
    fn healthy(&self, peer: &str) -> bool {
        if self.mode == "bluetooth" {
            return self.bluetooth.as_ref().is_some_and(|bt| bt.healthy(peer));
        }
        self.mode != "relay"
            && self
                .links
                .get(peer)
                .is_some_and(|l| l.received.elapsed() < Duration::from_millis(700))
    }
    fn dial(&self) {
        if self.mode == "bluetooth" {
            if let Some(bt) = &self.bluetooth {
                if let Some(local) = &bt.local {
                    for p in self.peers.values() {
                        if bluetooth_can_dial(&self.id, local, p) && !bt.connected(&p.id)
                        {
                            self.cb.emit(
                                json!({"_lan":"bt_connect","peer":p.id,"bluetooth":p.bluetooth}),
                            );
                        }
                    }
                }
            }
            return;
        }
        if self.mode == "relay" {
            return;
        }
        if let Some(i) = &self.identity {
            for p in self.peers.values() {
                if !p.certificate.is_empty()
                    && !p.addresses.is_empty()
                    && !self.links.contains_key(&p.id)
                {
                    i.dial(p.clone(), self.tx.clone());
                }
            }
        }
    }
    fn connected(&mut self, mut peer: String, conn: quinn::Connection, initiated: bool) {
        if self.mode == "relay" || self.mode == "bluetooth" || !self.connected {
            conn.close(0u32.into(), b"disabled");
            return;
        }
        let Some(cert) = lan::certificate(&conn) else {
            conn.close(1u32.into(), b"certificate required");
            return;
        };
        if peer.is_empty() {
            peer = self
                .peers
                .values()
                .find(|p| p.certificate == cert)
                .map(|p| p.id.clone())
                .unwrap_or_default();
        }
        if !self.peers.get(&peer).is_some_and(|p| p.certificate == cert) {
            conn.close(1u32.into(), b"unknown peer");
            return;
        }
        let preferred = initiated == (self.id < peer);
        if let Some(old) = self.links.get(&peer) {
            if old.preferred || !preferred {
                conn.close(0u32.into(), b"duplicate");
                return;
            }
        }
        if let Some(old) = self.links.remove(&peer) {
            old.conn.close(0u32.into(), b"preferred route");
        }
        let stream = lan::run_connection(peer.clone(), conn.clone(), initiated, self.tx.clone());
        self.links.insert(
            peer,
            Link {
                conn,
                stream,
                preferred,
                received: Instant::now(),
            },
        );
    }
    fn local(&mut self, mut v: Value) {
        if !self.enabled {
            return;
        }
        let kind = v["t"].as_str().unwrap_or("").to_string();
        if matches!(kind.as_str(), "motion" | "key" | "button" | "wheel" | "release") {
            // Native capture queues outlive UI callbacks. Bind their input to
            // the capture that produced it, not whichever grant is current by
            // the time the worker dequeues it. Unscoped legacy callers remain
            // compatible; updated native adapters always scope native capture.
            if let Some(scope) = v.get("_control_session") {
                if !self.grant.as_ref().is_some_and(|g| g.source == self.id && scope.as_str() == Some(g.session.as_str())) {
                    return;
                }
                v.as_object_mut().unwrap().remove("_control_session");
            }
        }
        if kind == "local_activity" {
            let id = v["control_id"].as_str().unwrap_or("");
            let valid = self.grant.as_ref().is_some_and(|g| g.session == id && g.source != self.id && self.controlled)
                || (self.grant.is_none() && !id.is_empty() && self.hub_control["target"] == self.id && self.hub_control["control_id"] == id);
            if valid {
                self.cb.hub(json!({"t":"takeover","control_id":id}));
                self.finish(false, false);
                // Legacy receivers have no native grant to finish.
                if self.legacy { self.inject(json!({"t":"leave"})); self.legacy = false; }
            }
            return;
        }
        if kind == "edge" && self.grant.is_none() {
            let Some(me) = self.devices.iter().find(|d| d.id == self.id) else {
                return;
            };
            let dir = v["dir"].as_str().unwrap_or("");
            let pos = v["pos"].as_f64().unwrap_or(0.5).clamp(0.0, 1.0);
            let (x, y) = match dir {
                "left" => (me.x, me.y + pos * me.h),
                "right" => (me.right(), me.y + pos * me.h),
                "up" => (me.x + pos * me.w, me.y),
                _ => (me.x + pos * me.w, me.bottom()),
            };
            let target = model::neighbour(&self.devices, me, dir, x, y).map(|d| d.id.clone());
            if let Some(target) = target {
                if self.strict() && !self.healthy(&target) {
                    self.cb.status("仅直连：目标连接尚未就绪".into());
                    return;
                }
                if self.peers.contains_key(&target) {
                    v["t"] = json!("direct_begin");
                }
            }
            self.cb.hub(v);
            return;
        }
        if !self.grant.as_ref().is_some_and(|g| g.source == self.id) {
            self.cb.hub(v);
            return;
        }
        match kind.as_str() {
            "motion" => self.move_by(
                v["dx"].as_f64().unwrap_or(0.0),
                v["dy"].as_f64().unwrap_or(0.0),
                v["_capture_us"].as_u64(),
            ),
            "release" => self.finish(false, true),
            "key" => {
                let Some(code) = integer_code(&v["code"], 767) else { return };
                let down = v["down"].as_bool().unwrap_or(false);
                if down {
                    self.held_keys.insert(code);
                } else {
                    self.held_keys.remove(&code);
                }
                if code == 1 && down && self.held_keys.contains(&29) && self.held_keys.contains(&56)
                {
                    self.finish(true, true);
                    return;
                }
                v["code"] = json!(self.map_key(code));
                self.action(v);
            }
            "button" => {
                let Some(b) = integer_code(&v["button"], 8) else { return };
                v["button"] = json!(b);
                if v["down"].as_bool().unwrap_or(false) {
                    self.held_buttons.insert(b);
                } else {
                    self.held_buttons.remove(&b);
                }
                self.action(v);
            }
            "wheel" => self.action(v),
            _ => {}
        }
    }
    fn map_key(&self, code: u64) -> u64 {
        let Some(g) = &self.grant else { return code };
        let src = self
            .devices
            .iter()
            .find(|d| d.id == self.id)
            .is_some_and(|d| d.platform == "macos");
        let dst = self
            .devices
            .iter()
            .find(|d| d.id == g.target)
            .is_some_and(|d| d.platform == "macos");
        if self.swap && src != dst {
            match code {
                125 => 29,
                29 => 125,
                126 => 97,
                97 => 126,
                c => c,
            }
        } else {
            code
        }
    }
    fn move_by(&mut self, dx: f64, dy: f64, captured: Option<u64>) {
        if !dx.is_finite() || !dy.is_finite() {
            return;
        }
        let Some(g) = self.grant.clone() else { return };
        let Some(target) = self.devices.iter().find(|d| d.id == g.target).cloned() else {
            self.finish(true, true);
            return;
        };
        self.metrics.captured(captured);
        let (nx, ny) = (g.x + dx, g.y + dy);
        let dir = if nx < target.x {
            Some("left")
        } else if nx >= target.right() {
            Some("right")
        } else if ny < target.y {
            Some("up")
        } else if ny >= target.bottom() {
            Some("down")
        } else {
            None
        };
        if let Some(dir) = dir {
            let gx = nx.clamp(target.x, target.right());
            let gy = ny.clamp(target.y, target.bottom());
            if let Some(next) = model::neighbour(&self.devices, &target, dir, gx, gy).cloned() {
                let (x, y) = if self.auto_center {
                    (next.x + next.w * 0.5, next.y + next.h * 0.5)
                } else {
                    model::entry(&next, dir, gx, gy)
                };
                if next.id == self.id {
                    self.action(json!({"t":"leave"}));
                    let (x, y) = if self.auto_center { (0.5, 0.5) } else { next.norm(x, y) };
                    self.finish_at(Some((x, y)), true);
                    return;
                }
                if self.strict() && !self.healthy(&next.id) {
                    return;
                }
                self.action(json!({"t":"leave"}));
                if let Some(current) = &mut self.grant {
                    current.target = next.id.clone();
                    current.x = x;
                    current.y = y;
                }
                if !self.peers.contains_key(&next.id) {
                    self.cb.hub(json!({"t":"direct_legacy","session":g.session,"target":next.id,"x":x,"y":y}));
                    self.grant = None;
                    self.motion = None;
                    self.last_motion = None;
                    for code in &self.held_keys {
                        self.cb.hub(json!({"t":"key","code":code,"down":true}));
                    }
                    for button in &self.held_buttons {
                        self.cb
                            .hub(json!({"t":"button","button":button,"down":true}));
                    }
                    self.cb.status("中枢转发 · 旧版设备".into());
                    return;
                }
                self.cb.hub(
                    json!({"t":"direct_route","session":g.session,"target":next.id,"x":x,"y":y}),
                );
                self.active_ui(&self.id, &next.id);
                self.action(json!({"t":"enter"}));
                let keys: Vec<u64> = self.held_keys.iter().copied().collect();
                for code in keys {
                    self.action(json!({"t":"key","code":self.map_key(code),"down":true}));
                }
                let buttons: Vec<u64> = self.held_buttons.iter().copied().collect();
                for button in buttons {
                    self.action(json!({"t":"button","button":button,"down":true}));
                }
                return;
            }
        }
        if let Some(g) = &mut self.grant {
            g.x = nx.clamp(target.x, target.right() - 1.0);
            g.y = ny.clamp(target.y, target.bottom() - 1.0);
        }
        self.seq += 1;
        if let Some(g) = &self.grant {
            self.motion = Some(Input {
                session: g.session.clone(),
                source: self.id.clone(),
                target: g.target.clone(),
                seq: self.seq,
                barrier: *self.barriers.get(&g.target).unwrap_or(&0),
                x: g.x,
                y: g.y,
                event: None,
                received_direct: false,
                trace: captured.map(|captured_us| MotionTrace {
                    captured_us,
                    source_queue_us: 0,
                    received_us: 0,
                }),
            });
        }
    }
    fn action(&mut self, event: Value) {
        let Some(g) = &self.grant else { return };
        self.seq += 1;
        self.motion = None;
        self.last_motion = None;
        let barrier = self.barriers.entry(g.target.clone()).or_default();
        *barrier += 1;
        let input = Input {
            session: g.session.clone(),
            source: self.id.clone(),
            target: g.target.clone(),
            seq: self.seq,
            barrier: *barrier,
            x: g.x,
            y: g.y,
            event: Some(event),
            trace: None,
            received_direct: false,
        };
        if self.pending.len() >= 256 {
            self.finish(true, true);
            return;
        }
        let key = (input.session.clone(), input.target.clone(), input.barrier);
        self.send_input(&input);
        self.pending.insert(
            key,
            Pending {
                input,
                created: Instant::now(),
                hub_sent: None,
            },
        );
    }
    fn relay(&self, to: &str, data: &Wire) {
        if self.connected {
            self.cb.hub(json!({"t":"direct_relay","to":to,"data":data}));
        }
    }
    fn send_input(&mut self, input: &Input) {
        let mut outgoing = input.clone();
        if let Some(trace) = &mut outgoing.trace {
            if let Some(queue) = self
                .metrics
                .sample(&input.session, input.seq, trace.captured_us)
            {
                trace.source_queue_us = queue;
            } else {
                outgoing.trace = None;
            }
        }
        let key_event = input.event.as_ref().is_some_and(|v| v["t"] == "key");
        let data = Wire::Input { input: outgoing };
        if self.healthy(&input.target)
            && self.send_wire(&input.target, data.clone(), input.event.is_none())
        {
            if input.event.is_none() {
                self.metrics.motion_direct += 1;
                self.motion_path = Some(true);
                self.motion_failed = false;
            } else {
                self.metrics.action_direct += 1;
                if key_event {
                    self.keyboard.sent(true);
                }
            }
            return;
        }
        if !self.strict() {
            if input.event.is_none() {
                self.metrics.motion_relay += 1;
                self.motion_path = Some(false);
                self.motion_failed = false;
            } else {
                self.metrics.action_relay += 1;
            }
            if key_event {
                self.keyboard.sent(false);
            }
            self.relay(&input.target, &data);
        } else if input.event.is_none() {
            self.metrics.motion_not_sent += 1;
            self.motion_failed = true;
        }
    }
    fn send_wire(&mut self, peer: &str, data: Wire, datagram: bool) -> bool {
        if self.mode == "bluetooth" {
            return self
                .bluetooth
                .as_mut()
                .is_some_and(|bt| bt.send(peer, data));
        }
        let Some(link) = self.links.get(peer) else {
            return false;
        };
        if datagram {
            serde_json::to_vec(&data)
                .is_ok_and(|bytes| link.conn.send_datagram(bytes.into()).is_ok())
        } else {
            link.stream.try_send(data).is_ok()
        }
    }
    fn ack(&mut self, input: &Input) {
        let data = Wire::Ack {
            session: input.session.clone(),
            source: input.source.clone(),
            target: self.id.clone(),
            barrier: input.barrier,
        };
        if self.healthy(&input.source) {
            self.send_wire(&input.source, data.clone(), false);
        }
        if !self.strict() {
            self.relay(&input.source, &data);
        }
    }
    fn receive(&mut self, peer: String, data: Wire, direct: bool) {
        if let Wire::Input { input } = &data {
            if self.retired.contains(&input.session) { return; }
        }
        if !self.enabled
            || (direct
                && (self.mode == "relay"
                    || (self.mode != "bluetooth" && !self.links.contains_key(&peer))))
        {
            return;
        }
        if direct {
            if let Some(l) = self.links.get_mut(&peer) {
                l.received = Instant::now();
            }
        }
        match data {
            Wire::Ping { nonce } => {
                if direct {
                    self.send_wire(&peer, Wire::Pong { nonce }, true);
                }
            }
            Wire::Pong { .. } => {}
            Wire::Ack {
                session,
                source,
                target,
                barrier,
            } => {
                if source == self.id && target == peer {
                    if let Some(p) = self.pending.remove(&(session, target, barrier)) {
                        if p.input.event.as_ref().is_some_and(|v| v["t"] == "key") {
                            self.keyboard.confirmed(p.created.elapsed());
                        }
                    }
                }
            }
            Wire::MotionAck {
                session,
                source,
                target,
                seq,
                receiver_queue_us,
            } => {
                if source == self.id
                    && target == peer
                    && self
                        .grant
                        .as_ref()
                        .is_some_and(|g| g.session == session && g.source == self.id)
                {
                    self.metrics.applied(&session, seq, receiver_queue_us);
                }
            }
            Wire::Input { mut input } => {
                #[cfg(test)]
                if self.drop_next_motion && input.event.is_none() {
                    self.drop_next_motion = false;
                    return;
                }
                input.received_direct = direct;
                if let Some(trace) = &mut input.trace {
                    if trace.received_us == 0 {
                        trace.received_us = metrics::now_us();
                    }
                }
                let allowed = self.grant.as_ref().is_some_and(|g| {
                    g.session == input.session
                        && g.source == peer
                        && input.source == peer
                        && input.target == self.id
                        && self.lease.elapsed() < Duration::from_secs(6)
                });
                if !allowed {
                    if self.grant.is_none() && self.early.len() < 32 {
                        self.early
                            .push((peer, Wire::Input { input }, Instant::now()));
                    }
                    return;
                }
                if !input.x.is_finite() || !input.y.is_finite() {
                    return;
                }
                if input.event.as_ref().is_some_and(|e| !valid_event(e)) {
                    return;
                }
                let receiver = self.receivers.entry(input.session.clone()).or_default();
                if input.event.is_none() && input.seq > receiver.received_motion_seq {
                    receiver.received_motion_seq = input.seq;
                    self.metrics.received_motion();
                }
                if input.event.is_some() {
                    if input.barrier <= receiver.barrier {
                        self.ack(&input);
                        return;
                    }
                    if input.barrier > receiver.barrier + 128 {
                        return;
                    }
                    receiver.actions.entry(input.barrier).or_insert(input);
                } else if input.seq > receiver.position_seq
                    && receiver.motion.as_ref().is_none_or(|m| m.seq < input.seq)
                {
                    receiver.motion = Some(input);
                }
                self.drain();
            }
        }
    }
    fn position(&self, x: f64, y: f64) {
        if let Some(me) = self.devices.iter().find(|d| d.id == self.id) {
            let (x, y) = me.norm(x, y);
            self.inject(json!({"t":"move","x":x,"y":y}));
        }
    }
    fn traced_position(&self, input: &Input) {
        if let Some(me) = self.devices.iter().find(|d| d.id == self.id) {
            let (x, y) = me.norm(input.x, input.y);
            let mut event = json!({"t":"move","x":x,"y":y});
            if let Some(trace) = &input.trace {
                event["_motion_trace"] = json!({"session":input.session,"source":input.source,"seq":input.seq,"received_us":trace.received_us});
            }
            self.inject(event);
        }
    }
    fn applied(&mut self, value: Value) {
        let Some(g) = &self.grant else { return };
        if value["session"] != g.session || value["source"] != g.source || !self.controlled {
            return;
        }
        let Some(seq) = value["seq"].as_u64() else {
            return;
        };
        let received = value["received_us"].as_u64().unwrap_or(0);
        let applied = value["applied_us"].as_u64().unwrap_or(received);
        let data = Wire::MotionAck {
            session: g.session.clone(),
            source: g.source.clone(),
            target: self.id.clone(),
            seq,
            receiver_queue_us: applied.saturating_sub(received),
        };
        if self.healthy(&g.source) {
            let source = g.source.clone();
            self.send_wire(&source, data.clone(), true);
        } else if !self.strict() {
            self.relay(&g.source, &data);
        }
    }
    fn drain(&mut self) {
        let Some(g) = self.grant.clone() else { return };
        let Some(mut receiver) = self.receivers.remove(&g.session) else {
            return;
        };
        while let Some(input) = receiver.actions.remove(&(receiver.barrier + 1)) {
            self.position(input.x, input.y);
            let mut event = input.event.clone().unwrap();
            if event["t"] == "enter" {
                if let Some(me) = self.devices.iter().find(|d| d.id == self.id) {
                    let (x, y) = me.norm(input.x, input.y);
                    event["x"] = json!(x);
                    event["y"] = json!(y);
                }
                self.controlled = true;
                self.active_ui(&g.source, &self.id);
            }
            if event["t"] == "leave" {
                self.controlled = false;
            }
            if event["t"] == "key" {
                self.keyboard.received(input.received_direct);
            }
            self.inject(event);
            receiver.barrier = input.barrier;
            if input.seq > receiver.position_seq {
                receiver.position_seq = input.seq;
                receiver.position = (input.x, input.y);
            } else {
                self.position(receiver.position.0, receiver.position.1);
            }
            self.ack(&input);
        }
        if let Some(input) = receiver.motion.take() {
            if input.barrier <= receiver.barrier {
                if input.seq > receiver.position_seq {
                    receiver.position_seq = input.seq;
                    receiver.position = (input.x, input.y);
                    self.motion_path = Some(input.received_direct);
                    self.traced_position(&input);
                }
            } else {
                receiver.motion = Some(input);
            }
        }
        self.receivers.insert(g.session, receiver);
    }
    fn finish(&mut self, warp: bool, notify: bool) {
        self.finish_at(if warp { Some((0.5, 0.5)) } else { None }, notify);
    }
    fn finish_at(&mut self, position: Option<(f64, f64)>, notify: bool) {
        let Some(g) = self.grant.take() else { return };
        self.retired.push_back(g.session.clone());
        if self.retired.len() > 256 { self.retired.pop_front(); }
        if notify {
            self.cb
                .hub(json!({"t":"direct_stop","session":g.session,"warp":position.is_some()}));
        }
        if g.source == self.id {
            let (x, y) = position.unwrap_or((0.5, 0.5));
            self.inject(json!({"t":"release","x":x,"y":y,"warp":position.is_some(),"_control_session":g.session}));
        }
        if self.controlled {
            self.inject(json!({"t":"leave","_control_session":g.session}));
            self.controlled = false;
        }
        self.pending
            .retain(|(session, _, _), _| session != &g.session);
        self.inject(json!({"t":"direct_active","source":"","target":"","_control_session":g.session}));
        self.motion = None;
        self.last_motion = None;
        self.receivers.clear();
        self.held_keys.clear();
        self.held_buttons.clear();
    }
    fn tick(&mut self) {
        if let Some(text) = self.metrics.report() {
            self.cb.emit(json!({"_lan":"metrics","text":text}));
        }
        if let Some(text) = self.keyboard.report() {
            self.cb.emit(json!({"_lan":"metrics","text":text}));
        }
        if let Some(text) = self.metrics.receiver_report() {
            self.cb.emit(json!({"_lan":"metrics","text":text}));
        }
        if self.grant.is_some() && self.lease.elapsed() > Duration::from_secs(6) {
            self.finish(true, true);
        }
        // Repair the final position if its unreliable update was lost. These
        // bounded repeats never delay new motion and never replay old positions.
        let repair = self
            .last_motion
            .as_mut()
            .filter(|_| self.mode != "bluetooth")
            .and_then(|(input, when, n)| {
                let deadlines = [20u64, 60, 120];
                if *n < 3 && when.elapsed() >= Duration::from_millis(deadlines[*n as usize]) {
                    *n += 1;
                    let mut input = input.clone();
                    input.trace = None;
                    Some(input)
                } else {
                    None
                }
            });
        if let Some(input) = repair {
            if self
                .grant
                .as_ref()
                .is_some_and(|g| g.session == input.session && g.source == self.id)
            {
                self.send_input(&input);
            }
        }
        if self.last_heartbeat.elapsed() > Duration::from_millis(200) {
            self.last_heartbeat = Instant::now();
            let peers: Vec<_> = self.peers.keys().cloned().collect();
            for p in peers {
                self.send_wire(&p, Wire::Ping { nonce: self.seq }, true);
            }
            if let Some(bt) = &mut self.bluetooth {
                bt.tick();
            }
        }
        if self.last_lease.elapsed() > Duration::from_secs(1) {
            self.last_lease = Instant::now();
            if let Some(g) = &self.grant {
                if g.source == self.id {
                    self.cb.hub(json!({"t":"direct_alive","session":g.session}));
                }
            }
        }
        if self.last_retry.elapsed() > Duration::from_millis(200) {
            self.last_retry = Instant::now();
            if self.last_dial.elapsed() > Duration::from_secs(2) {
                self.last_dial = Instant::now();
                if let Some(identity) = &mut self.identity {
                    if identity.refresh_addresses() {
                        self.cb.hub(identity.advert());
                    }
                }
                self.dial();
            }
            let retry: Vec<Input> = self
                .pending
                .values_mut()
                .filter_map(|p| {
                    if p.created.elapsed() > Duration::from_millis(180)
                        && p.hub_sent
                            .is_none_or(|t| t.elapsed() > Duration::from_millis(300))
                    {
                        p.hub_sent = Some(Instant::now());
                        Some(p.input.clone())
                    } else {
                        None
                    }
                })
                .collect();
            for input in retry {
                if !self.strict() {
                    self.relay(
                        &input.target,
                        &Wire::Input {
                            input: input.clone(),
                        },
                    );
                }
            }
            if self
                .pending
                .values()
                .any(|p| p.created.elapsed() > Duration::from_secs(3))
            {
                if self.grant.is_some() {
                    self.finish(true, true);
                }
                self.pending.clear();
            }
        }
        self.early
            .retain(|(_, _, t)| t.elapsed() < Duration::from_secs(1));
        if self.strict()
            && self
                .grant
                .as_ref()
                .is_some_and(|g| g.source == self.id && !self.healthy(&g.target))
        {
            self.finish(true, true);
        }
        if self.last_status.elapsed() > Duration::from_secs(2) {
            self.last_status = Instant::now();
            let active = self.grant.as_ref().map(|g| {
                if g.source == self.id {
                    g.target.as_str()
                } else {
                    g.source.as_str()
                }
            });
            let status = if !self.connected {
                "等待中枢认证".into()
            } else if self.dormant {
                "客户端已关闭 · 仅保留远程唤醒连接".into()
            } else if !self.enabled {
                "全部设备接力已暂停 · 直连监听已关闭".into()
            } else if let Some(peer) = active {
                if self.healthy(peer) {
                    if self.mode == "bluetooth" {
                        "蓝牙直连 · 双向认证".into()
                    } else {
                        let rtt = self.links[peer].conn.rtt().as_millis();
                        format!("局域网直连 · {} ms", rtt)
                    }
                } else if self.strict() {
                    if self.mode == "bluetooth" {
                        "仅蓝牙 · 等待认证连接".into()
                    } else {
                        "仅直连 · 等待连接".into()
                    }
                } else {
                    "中枢转发".into()
                }
            } else if self.mode == "bluetooth" {
                if self.bluetooth.as_ref().is_some_and(|bt| bt.any_healthy()) {
                    "蓝牙直连就绪 · 双向认证".into()
                } else {
                    format!(
                        "仅蓝牙 · {}",
                        self.bluetooth
                            .as_ref()
                            .map(|bt| bt.error.as_str())
                            .unwrap_or("初始化失败")
                    )
                }
            } else if self.legacy {
                "中枢转发".into()
            } else if self.mode == "relay" {
                "仅中转 · 蓝牙和局域网入口已关闭".into()
            } else if self
                .links
                .values()
                .any(|l| l.received.elapsed() < Duration::from_millis(700))
            {
                "局域网直连就绪".into()
            } else if self.strict() {
                if self.mode == "bluetooth" {
                    "仅蓝牙 · 等待认证连接".into()
                } else {
                    "仅直连 · 等待连接".into()
                }
            } else {
                "自动 · 中枢转发就绪".into()
            };
            let pointer = if !self.enabled || !self.connected {
                status.clone()
            } else if let Some(peer) = active {
                if self.motion_failed {
                    "位置发送失败 · 仅直连模式".to_string()
                } else if self.healthy(peer) && self.motion_path != Some(false) {
                    if self.mode == "bluetooth" {
                        "蓝牙直连 · 位置更新".to_string()
                    } else {
                        "局域网直连 · 位置数据报".to_string()
                    }
                } else if self.strict() {
                    "直连不可用 · 位置停止发送".to_string()
                } else {
                    "中枢转发 · 位置更新".to_string()
                }
            } else {
                status.clone()
            };
            let pending_keys = self
                .pending
                .values()
                .filter(|p| p.input.event.as_ref().is_some_and(|v| v["t"] == "key"))
                .count();
            let mut keyboard = if !self.enabled || !self.connected {
                status.clone()
            } else if let Some(peer) = active {
                if self.healthy(peer) && self.keyboard.path != Some(false) {
                    if self.mode == "bluetooth" {
                        "蓝牙直连 · 可靠事件".to_string()
                    } else {
                        "局域网直连 · 可靠流".to_string()
                    }
                } else if self.strict() {
                    "直连不可用 · 等待确认".to_string()
                } else {
                    "中枢转发 · 可靠事件".to_string()
                }
            } else {
                status.clone()
            };
            if active.is_some() {
                if pending_keys > 0 {
                    keyboard.push_str(&format!(" · 待确认 {pending_keys}"));
                } else if let Some(us) = self.keyboard.last_confirm_us {
                    keyboard.push_str(&format!(" · 最近确认 {:.1} ms", us as f64 / 1000.0));
                }
            }
            for (channel, text, previous) in [
                ("pointer", pointer, &mut self.pointer_status),
                ("keyboard", keyboard, &mut self.keyboard_status),
            ] {
                if &text != previous {
                    *previous = text.clone();
                    self.cb
                        .emit(json!({"_lan":"channel_status","channel":channel,"text":text}));
                    if self.connected {
                        self.cb
                            .hub(json!({"t":"direct_status","status":text,"channel":channel}));
                    }
                }
            }
            if status != self.status {
                self.status = status.clone();
                self.cb.status(status.clone());
                if self.connected {
                    self.cb.hub(json!({"t":"direct_status","status":status}));
                }
            }
        }
    }
}

// A Windows listener can also connect to the existing Mac listener. Legacy
// connector/listener adverts keep working; two dual-role peers dial once.
pub(crate) fn bluetooth_can_dial(id: &str, local: &Value, peer: &Peer) -> bool {
    let connector = local["role"] == "connector"
        || (local["role"] == "listener" && local["connector"].as_bool() == Some(true));
    connector && peer.bluetooth.as_ref().is_some_and(|remote| {
        remote["role"] == "listener"
            && (remote["connector"].as_bool() != Some(true) || local["role"] == "connector" || id < peer.id.as_str())
    })
}

// Native adapters must emit integer codes. Accept exactly integral legacy JSON
// floats, but never truncate fractional/negative/out-of-range key/button values.
fn integer_code(value: &Value, max: u64) -> Option<u64> {
    if let Some(n) = value.as_u64() { return (n > 0 && n <= max).then_some(n); }
    let n = value.as_f64()?;
    (n.is_finite() && n >= 1.0 && n <= max as f64 && n.fract() == 0.0).then_some(n as u64)
}

fn valid_event(event: &Value) -> bool {
    match event["t"].as_str() {
        Some("enter" | "leave") => true,
        Some("key") => {
            event["code"].as_u64().is_some_and(|c| c <= 767) && event["down"].is_boolean()
        }
        Some("button") => {
            event["button"]
                .as_u64()
                .is_some_and(|b| (1..=5).contains(&b))
                && event["down"].is_boolean()
        }
        Some("wheel") => ["dx", "dy"].iter().all(|k| {
            event[k]
                .as_f64()
                .is_some_and(|v| v.is_finite() && v.abs() <= 1_000_000.0)
        }),
        _ => false,
    }
}
