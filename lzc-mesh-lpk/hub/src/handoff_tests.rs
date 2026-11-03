use super::*;
use super::tests_support::{room,hello,drain};

#[test]
fn all_six_platform_directions_can_take_over_release_and_reverse() {
    for source in [Platform::Macos,Platform::Windows,Platform::Linux] {
        for target in [Platform::Macos,Platform::Windows,Platform::Linux] {
            if source==target {continue;}
            let mut r=room();
            let mut a=hello(&mut r,1,"a",source,1000.0,800.0);
            let mut b=hello(&mut r,2,"b",target,1000.0,800.0);
            r.handle(1,ClientMsg::Edge{dir:Dir::Right,pos:0.5});
            let id=r.active.as_ref().unwrap().control_id.clone();
            r.handle(1,ClientMsg::Key{code:64,down:true});
            r.handle(1,ClientMsg::Button{button:1,down:true});
            drain(&mut a);drain(&mut b);
            r.handle(2,ClientMsg::Takeover{control_id:id.clone()});
            assert!(r.active.is_none(),"{source:?}->{target:?}");
            let released=drain(&mut b);
            assert!(released.iter().any(|m|matches!(m,HubMsg::Key{code:64,down:false})));
            assert!(released.iter().any(|m|matches!(m,HubMsg::Button{button:1,down:false})));
            assert!(drain(&mut a).iter().any(|m|matches!(m,HubMsg::Release{warp:true,..})));
            r.handle(2,ClientMsg::Edge{dir:Dir::Left,pos:0.5});
            assert_eq!(r.active.as_ref().unwrap().source,"b");
            assert_ne!(r.active.as_ref().unwrap().control_id,id);
            // The old target is now the source. A stale request arriving at the
            // new receiver must not cancel this independent control session.
            r.handle(1,ClientMsg::Takeover{control_id:id});
            assert!(r.active.is_some());
            drain(&mut a);r.handle(2,ClientMsg::Key{code:64,down:true});
            assert!(drain(&mut a).iter().any(|m|matches!(m,HubMsg::Key{code:64,down:true})));
            r.handle(2,ClientMsg::Motion{dx:20.0,dy:0.0});
            assert!(r.active.is_none());
        }
    }
}

#[test]
fn takeover_is_current_receiver_only_and_works_in_strict_direct_mode() {
    let mut r=room();
    let mut a=hello(&mut r,1,"a",Platform::Macos,1000.0,800.0);
    let mut b=hello(&mut r,2,"b",Platform::Windows,1000.0,800.0);
    let _c=hello(&mut r,3,"c",Platform::Linux,1000.0,800.0);
    for conn in [1,2,3] {r.handle(conn,ClientMsg::DirectHello{version:1,certificate:"fixture".into(),addresses:vec![],bluetooth:None});}
    r.snapshot.settings.transport_mode=TransportMode::Direct;
    r.handle(1,ClientMsg::DirectBegin{dir:Dir::Right,pos:0.5});
    let id=r.active.as_ref().unwrap().control_id.clone();
    for conn in [0,1,3] {r.handle(conn,ClientMsg::Takeover{control_id:id.clone()});assert!(r.active.is_some());}
    r.handle(2,ClientMsg::Takeover{control_id:"stale-or-forged".into()});assert!(r.active.is_some());
    drain(&mut a);drain(&mut b);
    r.handle(2,ClientMsg::Takeover{control_id:id});assert!(r.active.is_none());
    assert!(drain(&mut a).iter().any(|m|matches!(m,HubMsg::DirectEnd{warp:true,..})));
    assert!(drain(&mut b).iter().any(|m|matches!(m,HubMsg::DirectEnd{..})));
}
