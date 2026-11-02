use super::*;
use super::tests_support::{drain, hello, room};

fn control(r: &mut Room, id: &str, enabled: bool) {
    if !r.conns.contains_key(&99) {let (tx,_rx)=mpsc::unbounded_channel();r.attach(99,tx);}
    r.handle(99,ClientMsg::Hello{id:String::new(),name:String::new(),platform:Platform::Web,w:0.0,h:0.0,version:String::new()});
    r.handle(99, ClientMsg::DeviceEnabled { id: id.into(), enabled });
}

#[test]
fn dormancy_releases_control_and_stops_both_clipboard_directions() {
    let mut r = room();
    let mut mac = hello(&mut r, 1, "mac", Platform::Macos, 1000.0, 800.0);
    let mut win = hello(&mut r, 2, "win", Platform::Windows, 1000.0, 800.0);
    let mut linux = hello(&mut r, 3, "linux", Platform::Linux, 1000.0, 800.0);
    r.handle(1, ClientMsg::Layout { id: "win".into(), x: 1000.0, y: 0.0 });
    r.handle(1, ClientMsg::Edge { dir: Dir::Right, pos: 0.5 });
    r.handle(1, ClientMsg::Key { code: 30, down: true });
    r.handle(1, ClientMsg::Button { button: 1, down: true });
    drain(&mut mac); drain(&mut win); drain(&mut linux);
    control(&mut r, "win", false);
    assert!(r.active.is_none());
    let released = drain(&mut win);
    assert!(released.iter().any(|m| matches!(m, HubMsg::Key { down:false, .. })));
    assert!(released.iter().any(|m| matches!(m, HubMsg::Button { down:false, .. })));
    assert!(!r.sharing_ids().contains("win") && r.is_online("win"));
    r.handle(1, ClientMsg::Clipboard { text: "new copy".into() });
    assert!(!drain(&mut win).iter().any(|m| matches!(m, HubMsg::Clipboard { .. })));
    assert!(drain(&mut linux).iter().any(|m| matches!(m, HubMsg::Clipboard { .. })));
    drain(&mut mac);
    r.handle(2, ClientMsg::Clipboard { text: "dormant copy".into() });
    assert!(!drain(&mut mac).iter().any(|m| matches!(m, HubMsg::Clipboard { .. })));
    r.files_changed(vec![]);
    assert!(!drain(&mut win).iter().any(|m| matches!(m, HubMsg::Files { .. })));
    assert!(drain(&mut linux).iter().any(|m| matches!(m, HubMsg::Files { .. })));
    control(&mut r, "win", true);
    r.handle(2, ClientMsg::Clipboard { text: "copy after wake".into() });
    assert!(drain(&mut mac).iter().any(|m| matches!(m, HubMsg::Clipboard { .. })));
}

#[test]
fn saved_dormancy_survives_reconnect_and_old_local_keyboard_preference() {
    let mut r = room();
    let _ = hello(&mut r, 1, "win", Platform::Windows, 1000.0, 800.0);
    let before = (r.snapshot.devices["win"].x, r.snapshot.devices["win"].y);
    control(&mut r, "win", false);
    assert!(!r.store.load("u").devices["win"].enabled);
    r.detach(1);
    let _ = hello(&mut r, 2, "win", Platform::Windows, 1000.0, 800.0);
    r.handle(2, ClientMsg::Sharing { enabled: true });
    assert!(!r.device_enabled("win") && !r.sharing_ids().contains("win"));
    assert_eq!(before, (r.snapshot.devices["win"].x, r.snapshot.devices["win"].y));
    r.detach(2);
    control(&mut r, "win", true);
    let _ = hello(&mut r, 3, "win", Platform::Windows, 1000.0, 800.0);
    assert!(r.device_enabled("win") && r.sharing_ids().contains("win"));
}

#[test]
fn file_lease_is_revoked_even_when_closed_and_reopened_quickly() {
    let mut r = room();
    let _ = hello(&mut r, 1, "win", Platform::Windows, 1000.0, 800.0);
    let lease = r.device_watch("win").unwrap();
    assert!(*lease.borrow() && !lease.has_changed().unwrap());
    control(&mut r, "win", false); control(&mut r, "win", true);
    assert!(*lease.borrow() && lease.has_changed().unwrap());
    let fresh = r.device_watch("win").unwrap();
    assert!(*fresh.borrow() && !fresh.has_changed().unwrap());
}

#[test]
fn another_account_or_unknown_device_cannot_be_toggled() {
    let mut owner = room();
    let _ = hello(&mut owner, 1, "win", Platform::Windows, 1000.0, 800.0);
    let mut other = room();
    let (tx, mut rx) = mpsc::unbounded_channel(); other.attach(99, tx);
    control(&mut other, "win", false);
    assert!(owner.device_enabled("win"));
    assert!(drain(&mut rx).iter().any(|m| matches!(m, HubMsg::Error { .. })));
    assert!(other.snapshot.devices.is_empty());
}
