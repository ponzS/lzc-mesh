use super::*;
use crate::identity::DeviceIdentity;
use super::tests_support::{hello,room};

fn read(rx:&mut mpsc::UnboundedReceiver<HubMsg>)->Vec<HubMsg>{let mut out=vec![];while let Ok(v)=rx.try_recv(){out.push(v)}out}
fn register(r:&mut Room,conn:u64,id:&str,key:&DeviceIdentity)->mpsc::UnboundedReceiver<HubMsg>{
    let(tx,rx)=mpsc::unbounded_channel();r.attach(conn,tx);
    r.handle(conn,ClientMsg::DeviceHello{id:id.into(),name:id.into(),platform:Platform::Windows,w:1000.0,h:800.0,version:"0.2.0".into(),public_key:key.public_key()});
    let nonce=r.conns[&conn].challenge.as_ref().unwrap().nonce.clone();
    r.handle(conn,ClientMsg::DeviceProof{signature:key.proof(&r.user,id,&nonce)});rx
}
fn trust(r:&mut Room,id:&str,key:&DeviceIdentity,approved:bool){r.handle(0,ClientMsg::DeviceTrust{id:id.into(),fingerprint:key.fingerprint(),approved});}
fn token(rx:&mut mpsc::UnboundedReceiver<HubMsg>)->String{read(rx).into_iter().filter_map(|v|if let HubMsg::DeviceAuth{token,..}=v{token}else{None}).last().unwrap()}

#[test] fn unapproved_has_no_input_peers_clipboard_files_or_management_privilege(){
    let mut r=room();let mut owner=hello(&mut r,1,"mac",Platform::Macos,1000.0,800.0);
    let key=DeviceIdentity::generate().unwrap();let mut pending=register(&mut r,2,"pending",&key);
    assert!(!r.device_enabled("pending"));assert!(!r.trusted_conn(&r.conns[&2]));
    for msg in read(&mut pending){if let HubMsg::State{devices,..}=msg{assert_eq!(devices.len(),1);assert_eq!(devices[0].id,"pending");}}
    read(&mut owner);
    r.handle(2,ClientMsg::DirectHello{version:1,certificate:"not authorized".into(),addresses:vec![],bluetooth:None});
    r.handle(2,ClientMsg::Edge{dir:Dir::Left,pos:0.5});
    r.handle(2,ClientMsg::Clipboard{text:"blocked".into()});
    r.handle(1,ClientMsg::Clipboard{text:"private".into()});
    r.files_changed(vec![]);
    assert!(r.active.is_none() && r.conns[&2].direct.is_none());
    assert!(!read(&mut owner).iter().any(|v|matches!(v,HubMsg::Clipboard{..})));
    assert!(!read(&mut pending).iter().any(|v|matches!(v,HubMsg::Clipboard{..}|HubMsg::Files{..}|HubMsg::DirectPeers{..})));
    assert!(r.file_lease("pending",&"a".repeat(44)).is_none());
    r.handle(2,ClientMsg::DeviceTrust{id:"pending".into(),fingerprint:key.fingerprint(),approved:true});
    assert!(!r.snapshot.bindings["pending"].approved);
    trust(&mut r,"pending",&key,true);let token=token(&mut pending);
    assert!(r.file_lease("pending",&token).is_some());
}

#[test] fn forged_id_and_replayed_proofs_cannot_touch_a_live_device(){
    let mut r=room();let _source=hello(&mut r,1,"mac",Platform::Macos,1000.0,800.0);
    let key=DeviceIdentity::generate().unwrap();let mut real=register(&mut r,2,"win",&key);trust(&mut r,"win",&key,true);read(&mut real);
    r.handle(0,ClientMsg::Layout{id:"win".into(),x:1000.0,y:0.0});
    r.handle(1,ClientMsg::Edge{dir:Dir::Right,pos:0.5});assert!(r.active.is_some());
    let geometry=(r.devices()["win"].x,r.devices()["win"].y);
    let foreign=DeviceIdentity::generate().unwrap();let mut forged=register(&mut r,9,"win",&foreign);
    assert!(read(&mut forged).iter().any(|v|matches!(v,HubMsg::DeviceAuth{status,..} if status=="key_mismatch")));
    assert!(r.active.is_some());assert!(r.conns[&9].device.is_none());assert!(r.trusted_conn(&r.conns[&2]));
    assert_eq!(geometry,(r.devices()["win"].x,r.devices()["win"].y));
    let(tx,mut rx)=mpsc::unbounded_channel();r.attach(10,tx);
    r.handle(10,ClientMsg::DeviceHello{id:"win".into(),name:"spoof".into(),platform:Platform::Windows,w:1000.0,h:800.0,version:String::new(),public_key:key.public_key()});
    let nonce=r.conns[&10].challenge.as_ref().unwrap().nonce.clone();
    let wrong=key.proof("other-account","win",&nonce);
    r.handle(10,ClientMsg::DeviceProof{signature:wrong});
    r.handle(10,ClientMsg::DeviceProof{signature:key.proof(&r.user,"win",&nonce)});
    assert!(r.conns[&10].device.is_none());assert!(r.active.is_some());
    assert!(read(&mut rx).iter().any(|v|matches!(v,HubMsg::Error{..})));
    let(tx,_rx)=mpsc::unbounded_channel();r.attach(11,tx);
    r.handle(11,ClientMsg::DeviceHello{id:"win".into(),name:"expired".into(),platform:Platform::Windows,w:1000.0,h:800.0,version:String::new(),public_key:key.public_key()});
    let p=r.conns.get_mut(&11).unwrap().challenge.as_mut().unwrap();p.created=std::time::Instant::now()-std::time::Duration::from_secs(31);
    let nonce=p.nonce.clone();r.handle(11,ClientMsg::DeviceProof{signature:key.proof(&r.user,"win",&nonce)});
    assert!(r.conns[&11].device.is_none());assert!(r.active.is_some());
}

#[test] fn revocation_invalidates_tokens_and_persists_without_deleting_layout(){
    let mut r=room();let _source=hello(&mut r,1,"mac",Platform::Macos,1000.0,800.0);
    let key=DeviceIdentity::generate().unwrap();let mut rx=register(&mut r,2,"win",&key);trust(&mut r,"win",&key,true);
    let old=token(&mut rx);let lease=r.file_lease("win",&old).unwrap();
    r.handle(0,ClientMsg::Layout{id:"win".into(),x:1000.0,y:0.0});
    r.handle(1,ClientMsg::Edge{dir:Dir::Right,pos:0.5});assert!(r.active.is_some());
    trust(&mut r,"win",&key,false);
    assert!(r.active.is_none() && !*lease.borrow() && lease.has_changed().unwrap());
    assert!(r.file_lease("win",&old).is_none());assert!(!r.store.load("u").bindings["win"].approved);
    assert_eq!(r.snapshot.devices["win"].x,1000.0);
    r.handle(2,ClientMsg::DeviceEnabled{id:"win".into(),enabled:true});assert!(!r.device_enabled("win"));
    r.detach(2);r.snapshot=r.store.load("u");let mut rx=register(&mut r,3,"win",&key);
    assert!(!read(&mut rx).iter().any(|v|matches!(v,HubMsg::DeviceAuth{token:Some(_),..})));
    trust(&mut r,"win",&key,true);let new=token(&mut rx);assert_ne!(new,old);
    assert!(r.file_lease("win",&old).is_none() && r.file_lease("win",&new).is_some());
    r.detach(3);let mut rx=register(&mut r,4,"win",&key);let newest=token(&mut rx);
    assert!(r.file_lease("win",&new).is_none() && r.file_lease("win",&newest).is_some());
    assert_eq!(r.snapshot.devices["win"].x,1000.0);
}

#[test] fn account_and_management_boundaries_are_explicit(){
    let mut a=room();let key=DeviceIdentity::generate().unwrap();let mut rx=register(&mut a,1,"win",&key);trust(&mut a,"win",&key,true);let value=token(&mut rx);
    let mut b=room();b.user="other".into();
    assert!(b.file_lease("win",&value).is_none());trust(&mut b,"win",&key,true);assert!(b.snapshot.bindings.is_empty());
    let(tx,mut unknown)=mpsc::unbounded_channel();a.attach(7,tx);a.files_changed(vec![]);
    assert!(!read(&mut unknown).iter().any(|v|matches!(v,HubMsg::Files{..}|HubMsg::State{..})));
    a.handle(7,ClientMsg::Hello{id:"win".into(),name:"legacy spoof".into(),platform:Platform::Windows,w:1000.0,h:800.0,version:String::new()});
    assert!(a.conns[&7].device.is_none());assert!(a.trusted_conn(&a.conns[&1]));
}
