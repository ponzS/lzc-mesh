use super::*;

#[test]
fn all_platform_pairs_route_keyboard_drag_and_reverse_over_real_quic() {
    for source in ["macos","windows","linux"] {
        for target in ["macos","windows","linux"] {
            if source==target {continue;}
            let mut p=Pair::new("auto");p.ready();
            for side in 0..2 {p.hub(side,json!({"t":"state","active":null,"settings":{"transport_mode":"auto","input_enabled":true,"swap_cmd_ctrl":false},"devices":[
                {"id":"a","platform":source,"x":0,"y":0,"w":1000,"h":800,"online":true,"trusted":true},
                {"id":"b","platform":target,"x":1000,"y":0,"w":1000,"h":800,"online":true,"trusted":true}
            ]}));}
            p.relay=false;p.begin();
            p.local(0,json!({"t":"key","code":64,"down":true}));
            p.local(0,json!({"t":"key","code":64,"down":false}));
            p.local(0,json!({"t":"button","button":1,"down":true}));
            p.local(0,json!({"t":"motion","dx":10,"dy":0}));
            p.local(0,json!({"t":"button","button":1,"down":false}));
            p.wait(Duration::from_secs(3),|p|p.injected(1,"key").len()==2 && p.injected(1,"button").len()==2);
            assert!(p.injected(1,"key").iter().all(|e|e["_control_session"]=="test-session-1"));
            p.local(0,json!({"t":"motion","dx":-30,"dy":0}));
            p.wait(Duration::from_secs(3),|p|!p.injected(0,"release").is_empty());
            for side in [0,1] {p.hub(side,json!({"t":"direct_session","session":"reverse-session","source":"b","target":"a","x":998,"y":80}));}
            p.wait(Duration::from_secs(3),|p|p.injected(0,"enter").len()==1);
            p.local(1,json!({"t":"key","code":64,"down":true}));p.local(1,json!({"t":"key","code":64,"down":false}));
            p.local(1,json!({"t":"button","button":1,"down":true}));p.local(1,json!({"t":"button","button":1,"down":false}));
            p.wait(Duration::from_secs(3),|p|p.injected(0,"key").len()==2 && p.injected(0,"button").len()==2);
            assert_eq!(p.injected(0,"key")[0]["code"],64,"{target}->{source}");
        }
    }
}

#[test]
fn local_takeover_retires_old_grant_and_cannot_cancel_a_new_reverse_grant() {
    let mut p=Pair::new("auto");p.ready();p.begin();
    p.local(0,json!({"t":"key","code":64,"down":true}));
    p.local(0,json!({"t":"button","button":1,"down":true}));
    p.wait(Duration::from_secs(3),|p|p.injected(1,"key").len()==1);
    p.local(1,json!({"t":"local_activity","control_id":"test-session-1"}));
    p.wait(Duration::from_secs(3),|p|!p.injected(1,"leave").is_empty() && !p.injected(0,"release").is_empty());
    // A late authenticated grant for the canceled session must not resurrect it.
    for side in [1,0] {p.hub(side,json!({"t":"direct_session","session":"test-session-1","source":"a","target":"b","x":1002,"y":80}));}
    for side in [0,1] {p.hub(side,json!({"t":"direct_session","session":"fresh-reverse","source":"b","target":"a","x":998,"y":80}));}
    p.wait(Duration::from_secs(3),|p|p.injected(0,"enter").len()==1);
    p.local(0,json!({"t":"local_activity","control_id":"test-session-1"}));
    p.local(1,json!({"t":"key","code":64,"down":true}));p.local(1,json!({"t":"key","code":64,"down":false}));
    p.wait(Duration::from_secs(3),|p|p.injected(0,"key").len()==2);
    assert_eq!(p.injected(1,"enter").len(),1);
    assert_eq!(p.injected(1,"capture").len(),1);
}

#[test]
fn queued_source_input_from_a_retired_capture_cannot_move_a_fresh_session() {
    let mut p=Pair::new("auto");p.ready();p.begin();
    p.hub(0,json!({"t":"direct_session","session":"fresh-same-direction","source":"a","target":"b","x":1002,"y":80}));
    p.hub(1,json!({"t":"direct_session","session":"fresh-same-direction","source":"a","target":"b","x":1002,"y":80}));
    p.wait(Duration::from_secs(3),|p|p.injected(1,"enter").len()==2);
    // This was captured before the old source role ended, but reaches the
    // worker after the new grant. It must not warp/return the fresh session.
    for event in [
        json!({"t":"motion","dx":-5000,"dy":700}),json!({"t":"key","code":42,"down":true}),
        json!({"t":"button","button":1,"down":true}),json!({"t":"wheel","dy":120}),json!({"t":"release"}),
    ] {
        let mut stale=event;stale["_control_session"]=json!("test-session-1");p.local(0,stale);
    }
    p.local(0,json!({"t":"key","code":64,"down":true,"_control_session":"fresh-same-direction"}));
    p.local(0,json!({"t":"key","code":64,"down":false,"_control_session":"fresh-same-direction"}));
    p.wait(Duration::from_secs(3),|p|p.injected(1,"key").len()==2);
    assert_eq!(p.injected(0,"release").len(),1,"only replacement of the old grant releases capture");
    assert!(p.injected(1,"button").is_empty() && p.injected(1,"wheel").is_empty());
    assert!(p.injected(1,"key").iter().all(|e|e["code"]==64));
}

#[test]
fn integral_native_codes_and_right_buttons_survive_but_fractional_codes_do_not() {
    let mut p=Pair::new("auto");p.ready();p.begin();
    for code in [json!(0),json!(-1),json!(64.5),json!(768)] {p.local(0,json!({"t":"key","code":code,"down":true}));}
    for button in [json!(0),json!(2.5),json!(9)] {p.local(0,json!({"t":"button","button":button,"down":true}));}
    p.local(0,json!({"t":"key","code":64.0,"down":true}));p.local(0,json!({"t":"key","code":64.0,"down":false}));
    p.local(0,json!({"t":"button","button":2.0,"down":true}));p.local(0,json!({"t":"button","button":2.0,"down":false}));
    p.wait(Duration::from_secs(3),|p|p.injected(1,"key").len()==2 && p.injected(1,"button").len()==2);
    assert_eq!(p.injected(1,"key")[0]["code"],64);
    assert_eq!(p.injected(1,"button")[0]["button"],2);
}

#[test]
fn receiver_reached_after_another_target_can_still_take_over() {
    let mut p=Pair::new("auto");p.ready();
    // On a three-device route, receivers retain the original grant's target;
    // an authenticated enter, not that initial target field, owns reception.
    p.hub(1,json!({"t":"direct_session","session":"test-session-1","source":"a","target":"previous-target","x":1002,"y":80}));
    p.hub(0,json!({"t":"direct_session","session":"test-session-1","source":"a","target":"b","x":1002,"y":80}));
    p.wait(Duration::from_secs(3),|p|!p.injected(1,"enter").is_empty());
    p.local(1,json!({"t":"local_activity","control_id":"test-session-1"}));
    p.wait(Duration::from_secs(3),|p|!p.injected(0,"release").is_empty());
}
