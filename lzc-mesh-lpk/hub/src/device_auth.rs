//! Account management authority approves a proved device key, not a claimed ID.
use super::*;
use crate::{identity, store::DeviceBinding};
use std::time::{Duration, Instant};
use subtle::ConstantTimeEq;

pub(super) struct Challenge {
    pub id:String, pub name:String, pub platform:Platform, pub w:f64, pub h:f64, pub version:String,
    pub public_key:String, pub nonce:String, pub created:Instant,
}
impl Room {
    pub(super) fn trusted_conn(&self,c:&Conn) -> bool {
        c.device.as_ref().and_then(|id|self.snapshot.bindings.get(id))
            .is_some_and(|b|b.approved && c.verified_key.as_deref()==Some(&b.public_key))
    }
    pub(super) fn manager_or_trusted(&self,conn:u64) -> bool {
        self.conns.get(&conn).is_some_and(|c|c.manager || self.trusted_conn(c))
    }
    pub(super) fn auth_status(&self,id:&str) -> (&str,String) {
        self.snapshot.bindings.get(id).map(|b| {
            (if b.approved{"approved"}else if b.revoked{"revoked"}else{"pending"}, identity::fingerprint(&b.public_key).unwrap_or_default())
        }).unwrap_or(("upgrade_required",String::new()))
    }
    pub(super) fn update_lease(&self,id:&str) {
        if let Some(w)=self.device_watches.get(id) {w.send_replace(self.device_enabled(id));}
    }
    #[allow(clippy::too_many_arguments)]
    pub(super) fn device_hello(&mut self,conn:u64,id:String,name:String,platform:Platform,w:f64,h:f64,version:String,public_key:String) {
        let Some(c)=self.conns.get_mut(&conn) else{return};
        // A registered connection cannot switch identities or request fresh proofs
        // to reset its state. Reconnect explicitly to obtain another challenge.
        if c.manager || c.native || c.device.is_some() || c.challenge.is_some() {return;}
        c.native=true;
        if platform==Platform::Web || id.is_empty() || id.len()>64 || !id.chars().all(|v|v.is_ascii_alphanumeric() || v=='-' || v=='_')
            || !(w.is_finite() && h.is_finite() && (100.0..100_000.0).contains(&w) && (100.0..100_000.0).contains(&h))
            || identity::fingerprint(&public_key).is_none() {
            self.reply(conn,HubMsg::Error{message:"invalid device identity".into()});return;
        }
        let Ok(nonce)=identity::nonce() else{return};
        c.challenge=Some(Challenge{id,name:name.chars().take(64).collect(),platform,w,h,version:version.chars().take(32).collect(),public_key,nonce:nonce.clone(),created:Instant::now()});
        self.reply(conn,HubMsg::DeviceChallenge{user:self.user.clone(),nonce});
    }
    pub(super) fn device_proof(&mut self,conn:u64,signature:String) {
        let Some(p)=self.conns.get_mut(&conn).and_then(|c|c.challenge.take()) else {
            self.reply(conn,HubMsg::Error{message:"device challenge missing or already consumed".into()});return;
        };
        if p.created.elapsed()>Duration::from_secs(30) || !identity::verify(&p.public_key,&self.user,&p.id,&p.nonce,&signature) {
            self.reply(conn,HubMsg::Error{message:"device proof rejected".into()});return;
        }
        let fingerprint=identity::fingerprint(&p.public_key).unwrap();
        if self.snapshot.bindings.get(&p.id).is_some_and(|b|b.public_key!=p.public_key) {
            self.reply(conn,HubMsg::DeviceAuth{status:"key_mismatch".into(),fingerprint,token:None});return;
        }
        let previous=self.snapshot.clone();
        self.snapshot.bindings.entry(p.id.clone()).or_insert(DeviceBinding{public_key:p.public_key.clone(),approved:false,revoked:false});
        // Only a matching proof may touch existing device metadata or control.
        if let Some(c)=self.conns.get_mut(&conn) {c.verified_key=Some(p.public_key);}
        if !self.hello(conn,p.id.clone(),p.name,p.platform,p.w,p.h,p.version) {
            self.snapshot=previous;
            if let Some(c)=self.conns.get_mut(&conn){c.verified_key=None;}
            return;
        }
        self.issue_device_auth(conn);
        self.update_lease(&p.id);
        self.broadcast_peers();
        self.broadcast();
    }
    pub(super) fn issue_device_auth(&mut self,conn:u64) {
        let Some(id)=self.conns.get(&conn).and_then(|c|c.device.clone()) else{return};
        let (status,fingerprint)=self.auth_status(&id);
        let status=status.to_string();
        let token=if status=="approved" {identity::nonce().ok()}else{None};
        if let Some(c)=self.conns.get_mut(&conn) {c.file_token=token.clone();}
        self.reply(conn,HubMsg::DeviceAuth{status,fingerprint,token});
    }
    pub(super) fn device_trust(&mut self,conn:u64,id:String,fingerprint:String,approved:bool) {
        if !self.conns.get(&conn).is_some_and(|c|c.manager) {
            self.reply(conn,HubMsg::Error{message:"account management approval required".into()});return;
        }
        let Some(b)=self.snapshot.bindings.get_mut(&id) else {
            self.reply(conn,HubMsg::Error{message:"no proved device key in this account".into()});return;
        };
        if identity::fingerprint(&b.public_key).as_deref()!=Some(&fingerprint) {
            self.reply(conn,HubMsg::Error{message:"device fingerprint changed; refresh before approval".into()});return;
        }
        if b.approved==approved && b.revoked==!approved {return;}
        let previous=b.clone();
        b.approved=approved; b.revoked=!approved;
        if !self.store.save(&self.user,&self.snapshot) {
            self.snapshot.bindings.insert(id.clone(),previous);
            self.reply(conn,HubMsg::Error{message:"cannot persist device approval".into()});return;
        }
        self.drop_device_from_active(&id);
        let conns:Vec<u64>=self.conns.iter().filter(|(_,c)|c.device.as_deref()==Some(&id)).map(|(n,_)|*n).collect();
        for n in conns {
            if let Some(c)=self.conns.get_mut(&n) {c.direct=None;c.transport_status.clear();c.channel_status.clear();}
            self.issue_device_auth(n);
        }
        self.update_lease(&id);
        self.broadcast_peers();self.broadcast();
    }
    pub fn file_lease(&mut self,id:&str,token:&str) -> Option<watch::Receiver<bool>> {
        if token.len()!=44 || !self.device_enabled(id) {return None}
        let allowed=self.conns.values().any(|c|c.device.as_deref()==Some(id) && self.trusted_conn(c)
            && c.file_token.as_ref().is_some_and(|v|bool::from(v.as_bytes().ct_eq(token.as_bytes()))));
        if !allowed{return None}
        self.device_watch(id)
    }
}
