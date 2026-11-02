//! Real loopback QUIC and FFI exercises. The small test coordinator supplies
//! already authenticated hub grants; it never substitutes a network transport.
use super::*;
use serde_json::{json, Value};
use std::{
    sync::Mutex,
    thread,
    time::{Duration, Instant},
};

type Events = Mutex<Vec<Value>>;
extern "C" fn record(ctx: *mut c_void, text: *const c_char) {
    let events = unsafe { &*(ctx as *const Events) };
    let text = unsafe { CStr::from_ptr(text) };
    if let Ok(v) = serde_json::from_slice(text.to_bytes()) {
        events.lock().unwrap().push(v);
    }
}
struct Pair {
    handles: [*mut Handle; 2],
    callbacks: [Arc<Events>; 2],
    contexts: [*const Events; 2],
    history: [Vec<Value>; 2],
    adverts: [Option<Value>; 2],
    relay: bool,
}
impl Pair {
    fn new(mode: &str) -> Self {
        let callbacks = [Arc::new(Mutex::new(vec![])), Arc::new(Mutex::new(vec![]))];
        let contexts = [
            Arc::into_raw(callbacks[0].clone()),
            Arc::into_raw(callbacks[1].clone()),
        ];
        let handles = [
            mesh_direct_new(c"a".as_ptr(), record, contexts[0] as *mut c_void),
            mesh_direct_new(c"b".as_ptr(), record, contexts[1] as *mut c_void),
        ];
        assert!(handles.iter().all(|p| !p.is_null()));
        let p = Self {
            handles,
            callbacks,
            contexts,
            history: [vec![], vec![]],
            adverts: [None, None],
            relay: true,
        };
        for n in 0..2 {
            p.hub(n, json!({"t":"hub_connected"}));
            p.settings(n, mode, true);
        }
        p
    }
    fn hub(&self, n: usize, v: Value) {
        let s = CString::new(v.to_string()).unwrap();
        mesh_direct_hub(self.handles[n], s.as_ptr());
    }
    fn local(&self, n: usize, v: Value) {
        let s = CString::new(v.to_string()).unwrap();
        assert!(mesh_direct_input(self.handles[n], s.as_ptr()));
    }
    fn settings(&self, n: usize, mode: &str, enabled: bool) {
        self.hub(n,json!({"t":"state","settings":{"transport_mode":mode,"input_enabled":enabled,"swap_cmd_ctrl":true},"devices":[
            {"id":"a","platform":"macos","x":0.0,"y":0.0,"w":1000.0,"h":800.0,"online":true,"trusted":true},
            {"id":"b","platform":"linux","x":1000.0,"y":0.0,"w":1000.0,"h":800.0,"online":true,"trusted":true}
        ]}));
    }
    fn pump(&mut self) {
        let mut changed = false;
        for n in 0..2 {
            let batch = std::mem::take(&mut *self.callbacks[n].lock().unwrap());
            for e in batch {
                if e["_lan"] == "hub" {
                    let m = &e["message"];
                    match m["t"].as_str().unwrap_or("") {
                        "direct_hello" => {
                            self.adverts[n] = Some(m.clone());
                            changed = true;
                        }
                        "direct_relay" if self.relay => {
                            let target = if m["to"] == "a" { 0 } else { 1 };
                            self.hub(target,json!({"t":"direct_relay","from":if n==0{"a"}else{"b"},"data":m["data"]}));
                        }
                        "direct_alive" => {
                            for n in 0..2 {
                                self.hub(n, json!({"t":"direct_lease","session":m["session"]}));
                            }
                        }
                        "direct_stop" => {
                            for n in 0..2 {
                                self.hub(n,json!({"t":"direct_end","session":m["session"],"warp":m["warp"]}));
                            }
                        }
                        _ => {}
                    }
                }
                if e["_lan"] == "inject" && !e["message"]["_motion_trace"].is_null() {
                    let text = CString::new(e["message"]["_motion_trace"].to_string()).unwrap();
                    mesh_direct_applied(self.handles[n], text.as_ptr());
                }
                self.history[n].push(e);
            }
        }
        if changed && self.adverts.iter().all(Option::is_some) {
            let peers:Vec<Value>=self.adverts.iter().enumerate().map(|(n,a)|{
                let a=a.as_ref().unwrap();json!({"id":if n==0{"a"}else{"b"},"certificate":a["certificate"],"addresses":a["addresses"]})
            }).collect();
            for n in 0..2 {
                self.hub(n, json!({"t":"direct_peers","peers":peers}));
            }
        }
    }
    fn wait(&mut self, timeout: Duration, predicate: impl Fn(&Self) -> bool) {
        let until = Instant::now() + timeout;
        loop {
            self.pump();
            if predicate(self) {
                return;
            }
            assert!(
                Instant::now() < until,
                "condition timed out; history: {:?}",
                self.history
            );
            thread::sleep(Duration::from_millis(5));
        }
    }
    fn ready(&mut self) {
        self.wait(Duration::from_secs(8), |p| {
            p.history.iter().all(|events| {
                events.iter().any(|e| {
                    e["_lan"] == "status" && e["text"].as_str().unwrap_or("").contains("直连就绪")
                })
            })
        });
    }
    fn begin(&mut self) {
        for n in [1, 0] {
            self.hub(n,json!({"t":"direct_session","session":"test-session-1","source":"a","target":"b","x":1002.0,"y":80.0}));
        }
        self.wait(Duration::from_secs(3), |p| {
            p.injected(1, "enter").len() == 1
        });
    }
    fn injected(&self, n: usize, kind: &str) -> Vec<&Value> {
        self.history[n]
            .iter()
            .filter(|e| e["_lan"] == "inject" && e["message"]["t"] == kind)
            .map(|e| &e["message"])
            .collect()
    }
}
impl Drop for Pair {
    fn drop(&mut self) {
        for h in self.handles {
            mesh_direct_free(h)
        }
        for ctx in self.contexts {
            unsafe {
                drop(Arc::from_raw(ctx));
            }
        }
    }
}

#[test]
fn real_quic_coalesces_positions_and_preserves_click_position_and_key_mapping() {
    let mut p = Pair::new("auto");
    p.ready();
    p.relay = false;
    p.begin();
    for _ in 0..100 {
        p.local(0, json!({"t":"motion","dx":0.25,"dy":0.0}));
    }
    p.local(0, json!({"t":"button","button":1,"down":true}));
    p.local(0, json!({"t":"motion","dx":5.0,"dy":0.0}));
    p.local(0, json!({"t":"button","button":1,"down":false}));
    p.local(0, json!({"t":"key","code":125,"down":true}));
    p.local(0, json!({"t":"key","code":125,"down":false}));
    p.local(
        0,
        json!({"t":"wheel","dx":1.5,"dy":120.0,"source":"continuous","phase":"changed"}),
    );
    p.local(0,json!({"t":"wheel","dx":0.0,"dy":0.0,"source":"continuous","phase":"ended","stop_x":true,"stop_y":true}));
    p.wait(Duration::from_secs(3), |p| {
        p.injected(1, "wheel").len() == 2
    });
    let moves = p.injected(1, "move");
    assert!(moves.len() < 100, "obsolete motions were replayed");
    assert!((moves.last().unwrap()["x"].as_f64().unwrap() - 0.032).abs() < 0.00001);
    let buttons = p.injected(1, "button");
    assert_eq!(buttons.len(), 2);
    assert_eq!(buttons[0]["down"], true);
    assert_eq!(buttons[1]["down"], false);
    let keys = p.injected(1, "key");
    assert_eq!(keys.len(), 2);
    assert_eq!(keys[0]["code"], 29);
}

#[test]
fn mode_switch_releases_udp_port_and_relay_continues_the_same_session() {
    let mut p = Pair::new("auto");
    p.ready();
    p.begin();
    let old_ports: Vec<u16> = p
        .adverts
        .iter()
        .filter_map(|a| a.as_ref())
        .flat_map(|a| a["addresses"].as_array().unwrap())
        .filter_map(|a| {
            a.as_str()?
                .parse::<std::net::SocketAddr>()
                .ok()
                .map(|a| a.port())
        })
        .collect();
    p.settings(0, "relay", true);
    p.settings(1, "relay", true);
    p.wait(Duration::from_secs(3), |p| {
        p.adverts
            .iter()
            .all(|a| a.as_ref().is_some_and(|a| a["certificate"] == ""))
    });
    let until = Instant::now() + Duration::from_secs(3);
    loop {
        if old_ports
            .iter()
            .all(|port| std::net::UdpSocket::bind((std::net::Ipv4Addr::UNSPECIFIED, *port)).is_ok())
        {
            break;
        }
        assert!(
            Instant::now() < until,
            "relay mode did not release its UDP listeners"
        );
        p.pump();
        thread::sleep(Duration::from_millis(10));
    }
    p.local(0, json!({"t":"button","button":1,"down":true}));
    p.local(0, json!({"t":"button","button":1,"down":false}));
    p.wait(Duration::from_secs(2), |p| {
        p.injected(1, "button").len() == 2
    });
    assert!(p.history[0]
        .iter()
        .any(|e| e["message"]["t"] == "direct_relay" && e["message"]["data"]["kind"] == "input"));
    p.settings(0, "auto", true);
    p.settings(1, "auto", true);
    p.wait(Duration::from_secs(5), |p| {
        p.adverts.iter().all(|a| {
            a.as_ref()
                .is_some_and(|a| a["certificate"].as_str().is_some_and(|s| !s.is_empty()))
        })
    });
}

#[test]
fn global_pause_releases_control_and_rejects_followup_input() {
    let mut p = Pair::new("relay");
    p.wait(Duration::from_secs(2), |p| {
        p.adverts.iter().all(Option::is_some)
    });
    p.begin();
    p.local(0, json!({"t":"button","button":1,"down":true}));
    p.wait(Duration::from_secs(2), |p| {
        p.injected(1, "button").len() == 1
    });
    p.settings(0, "relay", false);
    p.settings(1, "relay", false);
    p.wait(Duration::from_secs(2), |p| {
        !p.injected(0, "release").is_empty() && !p.injected(1, "leave").is_empty()
    });
    p.local(0, json!({"t":"key","code":30,"down":true}));
    for _ in 0..10 {
        p.pump();
        thread::sleep(Duration::from_millis(5));
    }
    assert!(p.injected(1, "key").is_empty());
}

#[test]
fn stale_session_and_non_input_actions_cannot_be_injected() {
    let mut p = Pair::new("relay");
    p.wait(Duration::from_secs(2), |p| {
        p.adverts.iter().all(Option::is_some)
    });
    p.begin();
    for (session, event) in [
        ("expired", json!({"t":"key","code":30,"down":true})),
        (
            "test-session-1",
            json!({"t":"clipboard","text":"forbidden"}),
        ),
    ] {
        p.hub(1,json!({"t":"direct_relay","from":"a","data":{"kind":"input","input":{"session":session,"source":"a","target":"b","seq":2,"barrier":2,"x":1002.0,"y":80.0,"event":event}}}));
    }
    for _ in 0..10 {
        p.pump();
        thread::sleep(Duration::from_millis(5));
    }
    assert!(p.injected(1, "key").is_empty());
    assert!(p.injected(1, "clipboard").is_empty());
}

#[test]
fn interrupted_direct_link_falls_back_then_recovers_without_repeating_actions() {
    let mut p = Pair::new("auto");
    p.ready();
    p.begin();
    // Close the real UDP endpoints while leaving the authenticated hub and
    // control session alive. New input must now arrive through relay.
    for h in p.handles {
        unsafe { &*h }.tx.send(Command::DropLan).unwrap();
    }
    p.local(0, json!({"t":"button","button":1,"down":true}));
    p.local(0, json!({"t":"motion","dx":20.0,"dy":0.0}));
    p.local(0, json!({"t":"button","button":1,"down":false}));
    p.wait(Duration::from_secs(2), |p| {
        p.injected(1, "button").len() == 2
    });
    for h in p.handles {
        unsafe { &*h }.tx.send(Command::RestoreLan).unwrap();
    }
    p.wait(Duration::from_secs(6), |p| {
        p.history[0].iter().any(|e| {
            e["_lan"] == "status" && e["text"].as_str().unwrap_or("").starts_with("局域网直连 ·")
        })
    });
    p.relay = false;
    p.local(0, json!({"t":"key","code":30,"down":true}));
    p.local(0, json!({"t":"key","code":30,"down":false}));
    p.wait(Duration::from_secs(2), |p| p.injected(1, "key").len() == 2);
    assert_eq!(p.injected(1, "button").len(), 2);
}

#[test]
fn traced_motion_reports_submission_latency_without_synchronised_clocks() {
    let mut p = Pair::new("auto");
    p.ready();
    p.relay = false;
    p.begin();
    for _ in 0..100 {
        p.local(
            0,
            json!({"t":"motion","dx":0.25,"dy":0.0,"_capture_us":mesh_direct_now_us()}),
        );
        p.pump();
        thread::sleep(Duration::from_millis(16));
    }
    p.wait(Duration::from_secs(5), |p| {
        p.history[0].iter().any(|v| {
            v["_lan"] == "metrics"
                && v["text"]
                    .as_str()
                    .is_some_and(|s| s.starts_with("pointer_metrics "))
        })
    });
    let report = p.history[0]
        .iter()
        .find(|v| {
            v["_lan"] == "metrics"
                && v["text"]
                    .as_str()
                    .is_some_and(|s| s.starts_with("pointer_metrics "))
        })
        .unwrap()["text"]
        .as_str()
        .unwrap();
    assert!(report.contains("source_queue_ms_p95="));
    assert!(report.contains("receiver_submit_queue_ms_p95="));
    assert!(report.contains("applied_ack_rtt_ms_p95="));
    let moves = p.injected(1, "move");
    let last = moves.last().unwrap();
    assert!((last["x"].as_f64().unwrap() - 0.027).abs() < 0.00001);
}

#[test]
fn lost_final_position_is_repaired_without_requiring_another_user_event() {
    let mut p = Pair::new("auto");
    p.ready();
    p.relay = false;
    p.begin();
    unsafe { &*p.handles[1] }
        .tx
        .send(Command::DropNextMotion)
        .unwrap();
    p.local(0, json!({"t":"motion","dx":5.0,"dy":0.0}));
    p.wait(Duration::from_secs(2), |p| {
        p.injected(1, "move")
            .iter()
            .any(|v| (v["x"].as_f64().unwrap() - 0.007).abs() < 0.00001)
    });
    assert!(p.injected(1, "button").is_empty());
}

#[test]
fn dormant_receiver_closes_native_endpoint_and_does_not_replay_old_input_on_wake() {
    let mut p = Pair::new("auto");
    p.ready(); p.begin();
    p.local(0, json!({"t":"key","code":30,"down":true}));
    p.wait(Duration::from_secs(2), |p| p.injected(1,"key").len()==1);
    p.hub(1,json!({"t":"state","settings":{"transport_mode":"auto","input_enabled":true},"devices":[
        {"id":"a","platform":"macos","x":0.0,"y":0.0,"w":1000.0,"h":800.0,"online":true,"enabled":true,"trusted":true},
        {"id":"b","platform":"windows","x":1000.0,"y":0.0,"w":1000.0,"h":800.0,"online":true,"enabled":false,"trusted":true}
    ]}));
    p.wait(Duration::from_secs(2), |p| !p.injected(1,"leave").is_empty() && p.adverts[1].as_ref().is_some_and(|a|a["certificate"]==""));
    // The real hub also revokes the source's grant when either end sleeps.
    p.hub(0,json!({"t":"direct_end","session":"test-session-1","warp":false}));
    p.wait(Duration::from_secs(2), |p| !p.injected(0,"release").is_empty());
    p.hub(1,json!({"t":"direct_relay","from":"a","data":{"kind":"input","input":{"session":"test-session-1","source":"a","target":"b","seq":99,"barrier":3,"x":1002.0,"y":80.0,"event":{"t":"key","code":31,"down":true}}}}));
    p.settings(1,"auto",true);
    for _ in 0..30 {p.pump();thread::sleep(Duration::from_millis(10));}
    assert_eq!(p.injected(1,"key").len(),1,"replayed input from a dormant lifecycle");
}

#[test]
fn revoked_trust_blocks_an_existing_quic_session_and_its_old_packets() {
    let mut p=Pair::new("auto");p.ready();p.begin();
    p.local(0,json!({"t":"key","code":30,"down":true}));
    p.wait(Duration::from_secs(2),|p|p.injected(1,"key").len()==1);
    p.hub(1,json!({"t":"state","settings":{"transport_mode":"auto","input_enabled":true},"devices":[
        {"id":"a","platform":"macos","x":0.0,"y":0.0,"w":1000.0,"h":800.0,"online":true,"trusted":true},
        {"id":"b","platform":"windows","x":1000.0,"y":0.0,"w":1000.0,"h":800.0,"online":true,"enabled":true,"trusted":false}
    ]}));
    p.wait(Duration::from_secs(2),|p|!p.injected(1,"leave").is_empty() && p.adverts[1].as_ref().is_some_and(|a|a["certificate"]==""));
    let stale=json!({"t":"direct_relay","from":"a","data":{"kind":"input","input":{"session":"test-session-1","source":"a","target":"b","seq":99,"barrier":3,"x":1002.0,"y":80.0,"event":{"t":"key","code":31,"down":true}}}});
    p.hub(1,stale.clone());
    for _ in 0..20{p.pump();thread::sleep(Duration::from_millis(5));}
    assert_eq!(p.injected(1,"key").len(),1);
    p.settings(1,"auto",true);p.hub(1,stale);
    for _ in 0..20{p.pump();thread::sleep(Duration::from_millis(5));}
    assert_eq!(p.injected(1,"key").len(),1,"reapproval replayed a revoked session");
}

#[test]
fn windows_dual_role_bluetooth_interoperates_with_existing_native_roles() {
    let mac = json!({"role":"listener","address":"AA:BB:CC:DD:EE:01","channel":13});
    let linux = json!({"role":"connector"});
    let windows = json!({"role":"listener","connector":true,"address":"AA:BB:CC:DD:EE:02","channel":7});
    let peer = |id: &str, bt: Value| model::Peer {
        id:id.into(),certificate:String::new(),addresses:vec![],bluetooth:Some(bt),
    };
    assert!(router::bluetooth_can_dial("windows", &windows, &peer("mac", mac.clone())));
    assert!(router::bluetooth_can_dial("linux", &linux, &peer("windows", windows.clone())));
    assert!(router::bluetooth_can_dial("linux", &linux, &peer("mac", mac.clone())));
    assert!(!router::bluetooth_can_dial("mac", &mac, &peer("windows", windows.clone())));
    assert!(router::bluetooth_can_dial("a", &windows, &peer("b", windows.clone())));
    assert!(!router::bluetooth_can_dial("b", &windows, &peer("a", windows.clone())));
    assert!(!router::bluetooth_can_dial("windows", &windows, &peer("linux", linux)));
    assert!(!router::bluetooth_can_dial("windows", &Value::Null, &peer("mac", mac)));
}

#[test]
fn bluetooth_only_never_advertises_udp_or_falls_back_to_relay() {
    let mut pair = Pair::new("bluetooth");
    pair.wait(Duration::from_secs(2), |p| {
        p.adverts.iter().all(Option::is_some)
    });
    for advert in pair.adverts.iter().flatten() {
        assert_eq!(advert["addresses"], json!([]));
        assert!(!advert["certificate"].as_str().unwrap().is_empty());
    }
    pair.local(0, json!({"t":"edge","dir":"right","pos":0.1}));
    pair.wait(Duration::from_secs(2), |p| {
        p.history[0].iter().any(|v| {
            v["_lan"] == "status"
                && v["text"]
                    .as_str()
                    .unwrap_or("")
                    .contains("目标连接尚未就绪")
        })
    });
    assert!(!pair.history[0]
        .iter()
        .any(|v| v["message"]["t"] == "direct_begin" || v["message"]["t"] == "direct_relay"));
    for n in 0..2 {
        pair.settings(n, "relay", true);
    }
    pair.wait(Duration::from_secs(2), |p| {
        p.history
            .iter()
            .all(|events| events.iter().any(|v| v["_lan"] == "bt_stop"))
    });
    pair.wait(Duration::from_secs(2), |p| {
        p.adverts.iter().flatten().all(|a| a["certificate"] == "")
    });
}
