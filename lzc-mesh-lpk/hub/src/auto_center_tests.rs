use super::*;
use crate::protocol::Settings;
use super::tests_support::{room, hello, drain};

fn settings(r: &mut Room, center: bool) {
    r.handle(0, serde_json::from_value(serde_json::json!({"t":"settings","auto_center":center})).unwrap());
}

#[test]
fn setting_defaults_on_and_false_persists_across_reload_and_broadcasts() {
    assert!(Settings::default().auto_center);
    let old: Snapshot=serde_json::from_str(r#"{"settings":{"swap_cmd_ctrl":true,"clipboard":false}}"#).unwrap();
    assert!(old.settings.auto_center);
    let mut r=room();
    let (tx,mut viewer)=mpsc::unbounded_channel();r.attach(10,tx);
    r.handle(10,ClientMsg::Hello{id:String::new(),name:String::new(),platform:Platform::Web,w:0.0,h:0.0,version:String::new()});
    settings(&mut r,false);
    assert!(!r.snapshot.settings.auto_center);
    assert!(!r.store.load(&r.user).settings.auto_center);
    let mut reported=false;
    while let Ok(message)=viewer.try_recv() {
        let value=serde_json::to_value(message).unwrap();
        if value["t"]=="state" {assert_eq!(value["settings"]["auto_center"],false);reported=true;}
    }
    assert!(reported);
    settings(&mut r,true);
    assert!(r.store.load(&r.user).settings.auto_center);
}

#[test]
fn relay_entry_movement_third_screen_and_return_share_the_same_center_or_edge() {
    for center in [false,true] {
        let mut r=room();
        let mut a=hello(&mut r,1,"a",Platform::Macos,1000.0,800.0);
        let mut b=hello(&mut r,2,"b",Platform::Windows,1000.0,800.0);
        let mut c=hello(&mut r,3,"c",Platform::Linux,1000.0,800.0);
        settings(&mut r,center);drain(&mut a);drain(&mut b);drain(&mut c);
        r.handle(1,ClientMsg::Edge{dir:Dir::Right,pos:0.1});
        let enter=drain(&mut b);
        assert!(enter.iter().any(|m|matches!(m,HubMsg::Enter{x,y} if *x==if center{0.5}else{0.002} && *y==if center{0.5}else{0.1})));
        r.handle(1,ClientMsg::Motion{dx:5.0,dy:0.0});
        assert!(drain(&mut b).iter().any(|m|matches!(m,HubMsg::Move{x,y} if (*x-if center{0.505}else{0.007}).abs()<0.000001 && *y==if center{0.5}else{0.1})));
        r.handle(1,ClientMsg::Motion{dx:1100.0,dy:0.0});
        assert_eq!(r.active.as_ref().unwrap().target,"c");
        assert!(drain(&mut c).iter().any(|m|matches!(m,HubMsg::Enter{x,y} if *x==if center{0.5}else{0.002} && *y==if center{0.5}else{0.1})));
        r.handle(1,ClientMsg::Motion{dx:-1100.0,dy:0.0});
        assert_eq!(r.active.as_ref().unwrap().target,"b");
        r.handle(1,ClientMsg::Motion{dx:-1100.0,dy:0.0});
        assert!(r.active.is_none());
        assert!(drain(&mut a).iter().any(|m|matches!(m,HubMsg::Release{x,y,warp:true} if *x==if center{0.5}else{0.998} && *y==if center{0.5}else{0.1})));
    }
}

#[test]
fn direct_grant_and_legacy_fallback_choose_the_same_authoritative_position() {
    for center in [false,true] {
        for native_target in [false,true] {
            let mut r=room();
            let _a=hello(&mut r,1,"a",Platform::Macos,1000.0,800.0);
            let _b=hello(&mut r,2,"b",Platform::Linux,1000.0,800.0);
            r.handle(1,ClientMsg::DirectHello{version:1,certificate:"fixture".into(),addresses:vec![],bluetooth:None});
            if native_target {r.handle(2,ClientMsg::DirectHello{version:1,certificate:"fixture".into(),addresses:vec![],bluetooth:None});}
            settings(&mut r,center);
            r.handle(1,ClientMsg::DirectBegin{dir:Dir::Right,pos:0.1});
            let active=r.active.as_ref().unwrap();
            assert_eq!(active.x,if center{1500.0}else{1002.0});
            assert_eq!(active.y,if center{400.0}else{80.0});
            assert_eq!(active.session.is_some(),native_target);
        }
    }
}
