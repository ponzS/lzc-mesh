use super::*;
use super::tests_support::{room, hello, drain};

fn manager(r: &mut Room, conn: u64) -> mpsc::UnboundedReceiver<HubMsg> {
    let (tx, rx) = mpsc::unbounded_channel(); r.attach(conn, tx);
    r.handle(conn, ClientMsg::Hello { id:String::new(), name:String::new(), platform:Platform::Web, w:0.0, h:0.0, version:String::new() }); rx
}
fn start(r: &mut Room, conn: u64, target: &str) { r.handle(conn, ClientMsg::WebControl {target:target.into()}); }
fn held(r: &mut Room, conn: u64) {
    r.handle(conn, ClientMsg::Key {code:64,down:true});
    r.handle(conn, ClientMsg::Button {button:1,down:true});
}
fn released(rx: &mut mpsc::UnboundedReceiver<HubMsg>) {
    let messages=drain(rx);
    assert!(messages.iter().any(|m|matches!(m,HubMsg::Key{code:64,down:false})),"{messages:?}");
    assert!(messages.iter().any(|m|matches!(m,HubMsg::Button{button:1,down:false})),"{messages:?}");
    assert!(messages.iter().any(|m|matches!(m,HubMsg::Leave)),"{messages:?}");
}

#[test]
fn browser_controls_existing_receivers_and_hands_off_without_a_persistent_screen() {
    let mut r=room(); let _web=manager(&mut r,3);
    let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    let mut b=hello(&mut r,2,"b",Platform::Windows,1000.0,800.0);
    start(&mut r,3,"a");
    assert_eq!(r.devices().len(),2); assert_eq!(r.active.as_ref().unwrap().source,"web:3");
    assert!(drain(&mut a).iter().any(|m|matches!(m,HubMsg::Enter{x:0.5,y:0.5})));
    held(&mut r,3); drain(&mut a); drain(&mut b);
    let id=r.active.as_ref().unwrap().control_id.clone();
    r.handle(3,ClientMsg::Motion{dx:600.0,dy:0.0});
    assert_eq!(r.active.as_ref().unwrap().target,"b");
    assert_eq!(r.active.as_ref().unwrap().control_id,id); released(&mut a);
    r.handle(3,ClientMsg::Key{code:30,down:true});
    assert!(drain(&mut b).iter().any(|m|matches!(m,HubMsg::Key{code:30,down:true})));
    r.handle(3,ClientMsg::Release); assert!(r.active.is_none());
}

#[test]
fn browser_disconnect_release_takeover_pause_and_dormancy_release_held_input() {
    for stop in ["release","disconnect","takeover","pause","sharing","dormancy"] {
        let mut r=room(); let _web=manager(&mut r,3);
        let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
        start(&mut r,3,"a"); held(&mut r,3); drain(&mut a);
        let id=r.active.as_ref().unwrap().control_id.clone();
        match stop {
            "release"=>r.handle(3,ClientMsg::Release),
            "disconnect"=>r.detach(3),
            "takeover"=>r.handle(1,ClientMsg::Takeover{control_id:id}),
            "pause"=>r.handle(0,ClientMsg::Settings{swap_cmd_ctrl:None,clipboard:None,transport_mode:None,input_enabled:Some(false)}),
            "sharing"=>r.handle(1,ClientMsg::Sharing{enabled:false}),
            _=>r.handle(0,ClientMsg::DeviceEnabled{id:"a".into(),enabled:false}),
        }
        assert!(r.active.is_none(),"{stop}"); released(&mut a);
        r.handle(3,ClientMsg::Motion{dx:10.0,dy:10.0}); assert!(r.active.is_none());
    }
}

#[test]
fn browser_control_requires_manager_available_target_and_relay_permission() {
    let mut r=room(); let mut web=manager(&mut r,3);
    let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    start(&mut r,1,"a"); assert!(r.active.is_none()); // even trusted natives cannot claim manager-only input
    for target in ["other-account","web:3"] {start(&mut r,3,target); assert!(r.active.is_none());}
    for mode in [TransportMode::Direct,TransportMode::Bluetooth] {
        r.snapshot.settings.transport_mode=mode; start(&mut r,3,"a"); assert!(r.active.is_none());
    }
    r.snapshot.settings.transport_mode=TransportMode::Auto;
    r.snapshot.settings.input_enabled=false; start(&mut r,3,"a"); assert!(r.active.is_none());
    r.snapshot.settings.input_enabled=true;
    r.snapshot.bindings.get_mut("a").unwrap().approved=false; start(&mut r,3,"a"); assert!(r.active.is_none());
    r.snapshot.bindings.get_mut("a").unwrap().approved=true;
    r.handle(1,ClientMsg::Sharing{enabled:false}); start(&mut r,3,"a"); assert!(r.active.is_none());
    r.handle(1,ClientMsg::Sharing{enabled:true});
    r.snapshot.devices.get_mut("a").unwrap().enabled=false; start(&mut r,3,"a"); assert!(r.active.is_none());
    r.snapshot.devices.get_mut("a").unwrap().enabled=true;
    r.detach(1); start(&mut r,3,"a"); assert!(r.active.is_none());
    assert!(!drain(&mut a).iter().any(|m|matches!(m,HubMsg::Enter{..})));
    assert!(drain(&mut web).iter().any(|m|matches!(m,HubMsg::Error{..})));
}

#[test]
fn other_managers_cannot_steal_input_release_or_renew_a_browser_session() {
    let mut r=room(); let _web=manager(&mut r,3); let _other=manager(&mut r,4);
    let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    start(&mut r,3,"a"); drain(&mut a);
    let id=r.active.as_ref().unwrap().control_id.clone();
    let lease=r.web_lease;
    start(&mut r,4,"a"); r.handle(4,ClientMsg::Release); held(&mut r,4);
    r.handle(4,ClientMsg::WebAlive{control_id:id.clone()});
    assert_eq!(r.web_lease,lease); assert_eq!(r.active.as_ref().unwrap().source,"web:3");
    assert!(!drain(&mut a).iter().any(|m|matches!(m,HubMsg::Key{..}|HubMsg::Button{..}|HubMsg::Leave)));
    r.handle(3,ClientMsg::Release);
    start(&mut r,4,"a"); assert_ne!(r.active.as_ref().unwrap().control_id,id);
}

#[test]
fn browser_cannot_steal_native_control_and_revocation_ends_browser_authorization() {
    let mut r=room(); let _web=manager(&mut r,3);
    let _a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    let _b=hello(&mut r,2,"b",Platform::Windows,1000.0,800.0);
    r.handle(1,ClientMsg::Edge{dir:Dir::Right,pos:0.5});
    start(&mut r,3,"a"); assert_eq!(r.active.as_ref().unwrap().source,"a");
    r.handle(1,ClientMsg::Release); start(&mut r,3,"a");
    let (_,fingerprint)=r.auth_status("a");
    r.handle(0,ClientMsg::DeviceTrust{id:"a".into(),fingerprint,approved:false});
    assert!(r.active.is_none()); assert!(r.web_lease.is_none());
    start(&mut r,3,"a"); assert!(r.active.is_none());
}

#[test]
fn stale_browser_lease_cannot_renew_a_new_control_on_the_same_connection() {
    let mut r=room(); let _web=manager(&mut r,3);
    let _a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    start(&mut r,3,"a"); let old=r.active.as_ref().unwrap().control_id.clone();
    r.handle(3,ClientMsg::Release); start(&mut r,3,"a"); let lease=r.web_lease;
    r.handle(3,ClientMsg::WebAlive{control_id:old}); assert_eq!(r.web_lease,lease);
}

#[test]
fn stalled_browser_lease_expires_before_late_input_or_renewal_can_revive_it() {
    let mut r=room(); let _web=manager(&mut r,3);
    let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    start(&mut r,3,"a"); held(&mut r,3); drain(&mut a);
    let id=r.active.as_ref().unwrap().control_id.clone();
    r.web_lease=Some(std::time::Instant::now()-std::time::Duration::from_secs(1));
    r.handle(3,ClientMsg::WebAlive{control_id:id});
    assert!(r.active.is_none()); assert!(r.web_lease.is_none()); released(&mut a);
}

#[test]
fn mode_changes_stop_browser_and_manual_target_switch_releases_old_input() {
    let mut r=room(); let _web=manager(&mut r,3);
    let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    let mut b=hello(&mut r,2,"b",Platform::Windows,1000.0,800.0);
    start(&mut r,3,"a"); held(&mut r,3); drain(&mut a);
    start(&mut r,3,"b"); released(&mut a);
    held(&mut r,3); drain(&mut b);
    r.handle(0,ClientMsg::Settings{swap_cmd_ctrl:None,clipboard:None,transport_mode:Some(TransportMode::Direct),input_enabled:None});
    assert!(r.active.is_none()); released(&mut b);
}

#[test]
fn committed_text_is_target_only_and_respects_clipboard_setting() {
    let mut r=room(); let _web=manager(&mut r,3);
    let mut a=hello(&mut r,1,"a",Platform::Linux,1000.0,800.0);
    let mut b=hello(&mut r,2,"b",Platform::Windows,1000.0,800.0);
    start(&mut r,3,"a"); drain(&mut a); drain(&mut b);
    r.handle(3,ClientMsg::WebText{text:"你好".into()});
    assert!(drain(&mut a).iter().any(|m|matches!(m,HubMsg::Clipboard{text,..} if text=="你好")));
    assert!(!drain(&mut b).iter().any(|m|matches!(m,HubMsg::Clipboard{..})));
    r.snapshot.settings.clipboard=false;
    r.handle(3,ClientMsg::WebText{text:"不可共享".into()});
    assert!(!drain(&mut a).iter().any(|m|matches!(m,HubMsg::Clipboard{..})));
}
