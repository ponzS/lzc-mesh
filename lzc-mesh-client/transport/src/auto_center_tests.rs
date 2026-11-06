use super::*;

fn settings(p: &Pair, side: usize, mode: &str, center: bool) {
    p.hub(side, json!({"t":"state","settings":{"transport_mode":mode,"input_enabled":true,"auto_center":center},"devices":[
        {"id":"a","platform":"macos","x":0,"y":0,"w":1000,"h":800,"online":true,"trusted":true},
        {"id":"b","platform":"windows","x":1000,"y":0,"w":1000,"h":800,"online":true,"trusted":true}
    ]}));
}

#[test]
fn centered_and_edge_entry_keep_followup_actions_and_return_on_the_same_coordinates() {
    for mode in ["auto", "relay"] {
        for center in [false, true] {
            let mut p = Pair::new(mode);
            if mode == "auto" { p.ready(); p.relay = false; }
            for side in 0..2 { settings(&p, side, mode, center); }
            p.begin();
            let enter = p.injected(1, "enter")[0];
            assert_eq!(enter["x"], if center { 0.5 } else { 0.002 });
            assert_eq!(enter["y"], if center { 0.5 } else { 0.1 });
            p.local(0, json!({"t":"motion","dx":5,"dy":0}));
            p.local(0, json!({"t":"button","button":1,"down":true}));
            p.local(0, json!({"t":"button","button":1,"down":false}));
            p.wait(Duration::from_secs(3), |p| p.injected(1, "button").len() == 2);
            let moves = p.injected(1, "move");
            let last = moves.last().unwrap();
            assert!((last["x"].as_f64().unwrap() - if center { 0.505 } else { 0.007 }).abs() < 0.000001);
            assert_eq!(last["y"], if center { 0.5 } else { 0.1 });
            p.local(0, json!({"t":"motion","dx":-700,"dy":0}));
            p.wait(Duration::from_secs(3), |p| !p.injected(0, "release").is_empty());
            let releases = p.injected(0, "release");
            let release = releases.last().unwrap();
            assert_eq!(release["warp"], true);
            assert_eq!(release["x"], if center { 0.5 } else { 0.998 });
            assert_eq!(release["y"], if center { 0.5 } else { 0.1 });
        }
    }
}

#[test]
fn receiver_never_recenters_sender_positions_even_during_setting_sync() {
    let mut p = Pair::new("relay");
    settings(&p, 0, "relay", false);
    settings(&p, 1, "relay", true);
    p.begin();
    let enter = p.injected(1, "enter")[0];
    assert_eq!(enter["x"], 0.002);
    assert_eq!(enter["y"], 0.1);
    p.local(0, json!({"t":"key","code":64,"down":true}));
    p.local(0, json!({"t":"key","code":64,"down":false}));
    p.wait(Duration::from_secs(3), |p| p.injected(1, "key").len() == 2);
    assert!(p.injected(1,"move").iter().all(|m| m["x"] == 0.002 && m["y"] == 0.1));
}

#[test]
fn centering_applies_again_after_third_screen_handoff_without_duplicate_actions() {
    let mut p = Pair::new("relay");
    // Presence in direct_peers selects native relay rather than legacy fallback.
    // Wait for both advertisements; grant delivery alone doesn't synchronize it.
    p.wait(Duration::from_secs(3), |p| p.adverts.iter().all(Option::is_some));
    let devices = json!([
        {"id":"a","x":0,"y":0,"w":1000,"h":800,"online":true,"trusted":true},
        {"id":"b","x":1000,"y":0,"w":1000,"h":800,"online":true,"trusted":true},
        {"id":"c","x":2000,"y":0,"w":1000,"h":800,"online":true,"trusted":true}
    ]);
    for side in 0..2 { p.hub(side,json!({"t":"state","settings":{"transport_mode":"relay","auto_center":true},"devices":devices})); }
    // Side b represents a receiver reached later in a three-screen session.
    for side in [1,0] { p.hub(side,json!({"t":"direct_session","session":"three-screen","source":"a","target":"c","x":2002,"y":80})); }
    p.wait(Duration::from_secs(3), |p| !p.injected(0,"capture").is_empty());
    // Sender computes c's centre, then crosses left into b. Only enter at b
    // owns reception; the original grant's target remains c on the receiver.
    p.local(0,json!({"t":"motion","dx":-600,"dy":0}));
    p.wait(Duration::from_secs(3), |p| !p.injected(1,"enter").is_empty());
    assert_eq!(p.injected(1,"enter")[0]["x"],0.5);
    assert_eq!(p.injected(1,"enter")[0]["y"],0.5);
    p.local(0,json!({"t":"motion","dx":5,"dy":0}));
    p.local(0,json!({"t":"button","button":2,"down":true}));
    p.local(0,json!({"t":"button","button":2,"down":false}));
    p.wait(Duration::from_secs(3), |p| p.injected(1,"button").len()==2);
    assert_eq!(p.injected(1,"move").last().unwrap()["x"],0.505);
}
