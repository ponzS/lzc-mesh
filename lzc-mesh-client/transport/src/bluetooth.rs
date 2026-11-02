//! TLS over native RFCOMM bytes. Native adapters never see plaintext inputs.
//! Only account certificates supplied by the authenticated hub are trusted.
use crate::{
    model::{Peer, Wire},
    Callback,
};
use base64::{engine::general_purpose::STANDARD, Engine as _};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName};
use serde_json::{json, Value};
use std::{
    collections::{HashMap, VecDeque},
    io::{Read, Write},
    sync::Arc,
    time::{Duration, Instant},
};

struct Link {
    tls: rustls::Connection,
    peer: Option<String>,
    certificate: Option<String>,
    expected: Option<String>,
    input: Vec<u8>,
    pending: VecDeque<Wire>,
    writing: bool,
    created: Instant,
    received: Instant,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        ffi::{c_char, c_void, CStr},
        sync::Mutex,
    };
    type Events = Mutex<Vec<Value>>;
    extern "C" fn record(context: *mut c_void, text: *const c_char) {
        let events = unsafe { &*(context as *const Events) };
        let value = serde_json::from_slice(unsafe { CStr::from_ptr(text) }.to_bytes()).unwrap();
        events.lock().unwrap().push(value);
    }
    fn engine(events: &Events) -> Bluetooth {
        Bluetooth::new(Callback {
            function: record,
            context: events as *const Events as usize,
        })
        .unwrap()
    }
    fn peer(id: &str, bt: &Bluetooth) -> Peer {
        Peer {
            id: id.into(),
            certificate: STANDARD.encode(&bt.cert),
            addresses: vec![],
            bluetooth: None,
        }
    }
    fn exchange(
        a: &mut Bluetooth,
        b: &mut Bluetooth,
        ae: &Events,
        be: &Events,
        ap: &HashMap<String, Peer>,
        bp: &HashMap<String, Peer>,
    ) -> Vec<(String, Wire)> {
        let mut output = vec![];
        for _ in 0..100 {
            let mut progress = false;
            for direction in 0..2 {
                let (src, dst, events, peers) = if direction == 0 {
                    (&mut *a, &mut *b, ae, bp)
                } else {
                    (&mut *b, &mut *a, be, ap)
                };
                let batch = std::mem::take(&mut *events.lock().unwrap());
                for event in batch {
                    if event["_lan"] == "bt_send" {
                        progress = true;
                        output.extend(dst.event(
                            &json!({"t":"bt_data","connection":"c","data":event["data"]}),
                            peers,
                        ));
                        src.event(
                            &json!({"t":"bt_written","connection":"c"}),
                            if std::ptr::eq(events, ae) { ap } else { bp },
                        );
                    }
                }
            }
            if !progress {
                return output;
            }
        }
        panic!("TLS did not settle")
    }
    #[test]
    fn native_bluetooth_bytes_require_mutual_account_certificates() {
        let ae = Events::default();
        let be = Events::default();
        let mut a = engine(&ae);
        let mut b = engine(&be);
        let ap = HashMap::from([("b".into(), peer("b", &b))]);
        let bp = HashMap::from([("a".into(), peer("a", &a))]);
        a.event(&json!({"t":"bt_open","connection":"c"}), &ap);
        b.event(&json!({"t":"bt_open","connection":"c","peer":"a"}), &bp);
        exchange(&mut a, &mut b, &ae, &be, &ap, &bp);
        assert!(a.healthy("b") && b.healthy("a"));
        assert!(a.send("b", Wire::Ping { nonce: 42 }));
        let received = exchange(&mut a, &mut b, &ae, &be, &ap, &bp);
        assert!(matches!(received.as_slice(),[(id,Wire::Ping{nonce:42})] if id=="a"));
        let position = |seq, barrier, event: Option<Value>| {
            let mut input = json!({"session":"test","source":"a","target":"b","seq":seq,"barrier":barrier,"x":seq,"y":0});
            if let Some(event) = event {
                input["event"] = event;
            }
            serde_json::from_value(json!({"kind":"input","input":input})).unwrap()
        };
        // Hold the first write in flight, then submit many newer positions and
        // an action boundary. Old pending positions disappear; the click stays.
        for seq in 1..=100 {
            assert!(a.send("b", position(seq, 0, None)));
        }
        assert!(a.send(
            "b",
            position(101, 1, Some(json!({"t":"button","button":1,"down":true})))
        ));
        for seq in 102..=111 {
            assert!(a.send("b", position(seq, 1, None)));
        }
        let received = exchange(&mut a, &mut b, &ae, &be, &ap, &bp);
        let input: Vec<_> = received
            .into_iter()
            .filter_map(|(_, w)| {
                if let Wire::Input { input } = w {
                    Some(input)
                } else {
                    None
                }
            })
            .collect();
        assert_eq!(
            input.iter().map(|i| i.seq).collect::<Vec<_>>(),
            vec![1, 100, 101, 111]
        );
        assert!(input[2].event.is_some());
        assert_eq!(input[3].barrier, 1);
        a.revoke(&HashMap::new());
        assert!(!a.healthy("b"));
        assert!(ae.lock().unwrap().iter().any(|v| v["_lan"] == "bt_close"));
    }
    #[test]
    fn a_socket_without_account_trust_is_closed_before_tls() {
        let events = Events::default();
        let mut bt = engine(&events);
        bt.event(
            &json!({"t":"bt_open","connection":"untrusted"}),
            &HashMap::new(),
        );
        assert!(bt.links.is_empty());
        assert!(events
            .lock()
            .unwrap()
            .iter()
            .any(|v| v["_lan"] == "bt_close" && v["connection"] == "untrusted"));
    }

    #[test]
    fn foreign_bluetooth_client_cannot_complete_tls() {
        let ae = Events::default();
        let be = Events::default();
        let te = Events::default();
        let mut a = engine(&ae);
        let mut b = engine(&be);
        let trusted = engine(&te);
        let ap = HashMap::from([("trusted".into(), peer("trusted", &trusted))]);
        let bp = HashMap::from([("a".into(), peer("a", &a))]);
        a.event(&json!({"t":"bt_open","connection":"c"}), &ap);
        b.event(&json!({"t":"bt_open","connection":"c","peer":"a"}), &bp);
        let received = exchange(&mut a, &mut b, &ae, &be, &ap, &bp);
        assert!(received.is_empty());
        assert!(!a.any_healthy());
        assert!(a.links.is_empty());
    }
}

pub struct Bluetooth {
    cert: CertificateDer<'static>,
    key: Vec<u8>,
    links: HashMap<String, Link>,
    pub local: Option<Value>,
    pub error: String,
    cb: Callback,
}
impl Bluetooth {
    pub fn new(cb: Callback) -> Result<Self, String> {
        let certified = rcgen::generate_simple_self_signed(vec!["lzc-mesh.local".into()])
            .map_err(|e| e.to_string())?;
        Ok(Self {
            cert: certified.cert.der().clone(),
            key: certified.signing_key.serialize_der(),
            links: HashMap::new(),
            local: None,
            error: "等待蓝牙适配器".into(),
            cb,
        })
    }
    pub fn advert(&self) -> Value {
        json!({"t":"direct_hello","version":1,"certificate":STANDARD.encode(&self.cert),"addresses":[],"bluetooth":self.local})
    }
    pub fn healthy(&self, peer: &str) -> bool {
        self.links.values().any(|l| {
            l.peer.as_deref() == Some(peer) && l.received.elapsed() < Duration::from_millis(1500)
        })
    }
    pub fn any_healthy(&self) -> bool {
        self.links
            .values()
            .any(|l| l.peer.is_some() && l.received.elapsed() < Duration::from_millis(1500))
    }
    pub fn connected(&self, peer: &str) -> bool {
        self.links
            .values()
            .any(|l| l.peer.as_deref() == Some(peer) || l.expected.as_deref() == Some(peer))
    }
    pub fn close(&mut self, id: &str) {
        self.links.remove(id);
        // Also reject native sockets for which TLS setup never succeeded (for
        // example an empty account trust set), so they cannot occupy slots.
        if !id.is_empty() {
            self.cb.emit(json!({"_lan":"bt_close","connection":id}));
        }
    }
    pub fn stop(&mut self) {
        self.links.clear();
        self.cb.emit(json!({"_lan":"bt_stop"}));
    }
    pub fn revoke(&mut self, peers: &HashMap<String, Peer>) {
        let ids: Vec<_> = self
            .links
            .iter()
            .filter(|(_, l)| {
                if let Some(p) = &l.peer {
                    !peers
                        .get(p)
                        .is_some_and(|p| Some(&p.certificate) == l.certificate.as_ref())
                } else {
                    true
                } // also revoke handshakes built with an obsolete trust set
            })
            .map(|(id, _)| id.clone())
            .collect();
        for id in ids {
            self.close(&id);
        }
    }
    fn open(
        &mut self,
        id: String,
        expected: Option<String>,
        peers: &HashMap<String, Peer>,
    ) -> Result<(), String> {
        if self.links.len() >= 8 || self.links.contains_key(&id) {
            return Err("蓝牙连接数量限制".into());
        }
        let mut roots = rustls::RootCertStore::empty();
        for p in peers
            .values()
            .filter(|p| expected.as_ref().is_none_or(|id| id == &p.id))
        {
            if let Ok(der) = STANDARD.decode(&p.certificate) {
                let _ = roots.add(CertificateDer::from(der));
            }
        }
        if roots.is_empty() {
            return Err("等待同账号设备证书".into());
        }
        let key = PrivatePkcs8KeyDer::from(self.key.clone()).into();
        let tls = if expected.is_some() {
            let mut config = rustls::ClientConfig::builder()
                .with_root_certificates(roots)
                .with_client_auth_cert(vec![self.cert.clone()], key)
                .map_err(|e| e.to_string())?;
            config.alpn_protocols = vec![b"lzc-mesh/bt/1".to_vec()];
            rustls::Connection::Client(
                rustls::ClientConnection::new(
                    Arc::new(config),
                    ServerName::try_from("lzc-mesh.local").unwrap(),
                )
                .map_err(|e| e.to_string())?,
            )
        } else {
            let verifier = rustls::server::WebPkiClientVerifier::builder(Arc::new(roots))
                .build()
                .map_err(|e| e.to_string())?;
            let mut config = rustls::ServerConfig::builder()
                .with_client_cert_verifier(verifier)
                .with_single_cert(vec![self.cert.clone()], key)
                .map_err(|e| e.to_string())?;
            config.alpn_protocols = vec![b"lzc-mesh/bt/1".to_vec()];
            rustls::Connection::Server(
                rustls::ServerConnection::new(Arc::new(config)).map_err(|e| e.to_string())?,
            )
        };
        let now = Instant::now();
        self.links.insert(
            id.clone(),
            Link {
                tls,
                peer: None,
                certificate: None,
                expected,
                input: vec![],
                pending: VecDeque::new(),
                writing: false,
                created: now,
                received: now,
            },
        );
        self.flush(&id)?;
        Ok(())
    }
    fn flush(&mut self, id: &str) -> Result<(), String> {
        let Some(l) = self.links.get_mut(id) else {
            return Ok(());
        };
        if l.writing {
            return Ok(());
        }
        // Coalesce only plaintext positions. Never discard encrypted TLS records.
        if !l.tls.is_handshaking() {
            if let Some(w) = l.pending.pop_front() {
                let mut bytes = serde_json::to_vec(&w).map_err(|e| e.to_string())?;
                bytes.push(b'\n');
                l.tls
                    .writer()
                    .write_all(&bytes)
                    .map_err(|e| e.to_string())?;
            }
        }
        let mut bytes = vec![];
        while l.tls.wants_write() {
            l.tls.write_tls(&mut bytes).map_err(|e| e.to_string())?;
        }
        if bytes.len() > 65536 {
            return Err("蓝牙输出缓冲超限".into());
        }
        if !bytes.is_empty() {
            l.writing = true;
            self.cb
                .emit(json!({"_lan":"bt_send","connection":id,"data":STANDARD.encode(bytes)}));
        }
        Ok(())
    }
    pub fn send(&mut self, peer: &str, wire: Wire) -> bool {
        let Some(id) = self
            .links
            .iter()
            .find(|(_, l)| l.peer.as_deref() == Some(peer))
            .map(|(id, _)| id.clone())
        else {
            return false;
        };
        let l = self.links.get_mut(&id).unwrap();
        let replace = matches!((&wire,l.pending.back()),(Wire::Input{input:a},Some(Wire::Input{input:b})) if a.event.is_none() && b.event.is_none() && a.session==b.session && a.target==b.target && a.barrier==b.barrier);
        if replace {
            l.pending.pop_back();
        }
        if l.pending.len() >= 128 {
            self.close(&id);
            return false;
        }
        l.pending.push_back(wire);
        if let Err(e) = self.flush(&id) {
            self.error = e;
            self.close(&id);
            return false;
        }
        true
    }
    pub fn event(&mut self, value: &Value, peers: &HashMap<String, Peer>) -> Vec<(String, Wire)> {
        let id = value["connection"].as_str().unwrap_or("");
        let result = (|| -> Result<Vec<(String, Wire)>, String> {
            match value["t"].as_str().unwrap_or("") {
                "bt_local" => {
                    self.local = value.get("bluetooth").filter(|v| !v.is_null()).cloned();
                    self.error = value["error"].as_str().unwrap_or("等待蓝牙连接").into();
                }
                "bt_open" => self.open(
                    id.into(),
                    value["peer"]
                        .as_str()
                        .filter(|s| !s.is_empty())
                        .map(str::to_string),
                    peers,
                )?,
                "bt_closed" => {
                    self.links.remove(id);
                    self.error = value["error"].as_str().unwrap_or("蓝牙连接已断开").into();
                }
                "bt_written" => {
                    if let Some(l) = self.links.get_mut(id) {
                        l.writing = false;
                    }
                    self.flush(id)?;
                }
                "bt_data" => {
                    let bytes = STANDARD
                        .decode(value["data"].as_str().unwrap_or(""))
                        .map_err(|e| e.to_string())?;
                    if bytes.len() > 65536 {
                        return Err("蓝牙帧超限".into());
                    }
                    let Some(l) = self.links.get_mut(id) else {
                        return Ok(vec![]);
                    };
                    let mut cursor = std::io::Cursor::new(bytes);
                    while cursor.position() < cursor.get_ref().len() as u64 {
                        if l.tls.read_tls(&mut cursor).map_err(|e| e.to_string())? == 0 {
                            break;
                        }
                        let io = l.tls.process_new_packets().map_err(|e| e.to_string())?;
                        if io.peer_has_closed() {
                            return Err("蓝牙 TLS 已关闭".into());
                        }
                    }
                    if !l.tls.is_handshaking() && l.peer.is_none() {
                        if l.tls.alpn_protocol() != Some(b"lzc-mesh/bt/1".as_slice()) {
                            return Err("蓝牙协议不匹配".into());
                        }
                        let cert = l
                            .tls
                            .peer_certificates()
                            .and_then(|cs| cs.first())
                            .map(|c| STANDARD.encode(c))
                            .ok_or("蓝牙设备证书缺失")?;
                        let peer = peers
                            .values()
                            .find(|p| {
                                p.certificate == cert
                                    && l.expected.as_ref().is_none_or(|id| id == &p.id)
                            })
                            .ok_or("蓝牙设备未认证")?;
                        l.peer = Some(peer.id.clone());
                        l.certificate = Some(cert);
                        self.error.clear();
                    }
                    let mut buf = [0u8; 4096];
                    loop {
                        match l.tls.reader().read(&mut buf) {
                            Ok(0) => break,
                            Ok(n) => l.input.extend_from_slice(&buf[..n]),
                            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
                            Err(e) => return Err(e.to_string()),
                        }
                        if l.input.len() > 65536 {
                            return Err("蓝牙明文缓冲超限".into());
                        }
                    }
                    let mut received = vec![];
                    while let Some(end) = l.input.iter().position(|b| *b == b'\n') {
                        if end > 16384 {
                            return Err("蓝牙输入超限".into());
                        }
                        let frame: Vec<_> = l.input.drain(..=end).collect();
                        let wire =
                            serde_json::from_slice::<Wire>(&frame).map_err(|e| e.to_string())?;
                        let peer = l.peer.clone().ok_or("蓝牙握手尚未完成")?;
                        l.received = Instant::now();
                        received.push((peer, wire));
                    }
                    self.flush(id)?;
                    return Ok(received);
                }
                _ => {}
            }
            Ok(vec![])
        })();
        match result {
            Ok(v) => v,
            Err(e) => {
                self.error = e;
                self.close(id);
                vec![]
            }
        }
    }
    pub fn tick(&mut self) {
        let expired: Vec<_> = self
            .links
            .iter()
            .filter(|(_, l)| {
                if l.peer.is_none() {
                    l.created.elapsed() > Duration::from_secs(8)
                } else {
                    l.received.elapsed() > Duration::from_secs(4)
                }
            })
            .map(|(id, _)| id.clone())
            .collect();
        for id in expired {
            self.close(&id);
        }
    }
}
