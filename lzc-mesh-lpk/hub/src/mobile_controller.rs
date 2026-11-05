//! Ephemeral account-manager input, routed through the existing relay/handoff.
//! A browser is never registered as a trusted native device or advertised as a peer.
use super::*;
use std::time::{Duration, Instant};

// ':' is excluded from native device IDs, so a device cannot impersonate a browser.
pub(super) fn web_source(conn: u64) -> String { format!("web:{conn}") }
const LEASE: Duration = Duration::from_secs(4);

impl Room {
    fn web_error(&self, conn: u64, message: &str) {
        self.reply(conn, HubMsg::Error { message: message.into() });
    }

    pub(super) fn web_control(&mut self, conn: u64, target: &str) {
        self.expire_web_control();
        if !self.conns.get(&conn).is_some_and(|c| c.manager) {
            self.web_error(conn, "请使用已登录的管理页面控制设备"); return;
        }
        if !self.snapshot.settings.input_enabled {
            self.web_error(conn, "键鼠接力已暂停，请先在首页恢复"); return;
        }
        if matches!(self.snapshot.settings.transport_mode, TransportMode::Direct | TransportMode::Bluetooth) {
            self.web_error(conn, "手机触控板需要中枢转发，请在首页选择自动或仅中枢转发"); return;
        }
        if !self.sharing_ids().contains(target) {
            self.web_error(conn, "设备不可用：请选择同账号已批准、在线且开启接力的设备"); return;
        }
        let source = web_source(conn);
        if self.active.as_ref().is_some_and(|a| a.source != source) {
            self.web_error(conn, "其他控制端正在使用设备，请先结束原控制"); return;
        }
        let Some(control_id) = fresh_control_id() else { return };
        self.stop(false);
        let d = &self.snapshot.devices[target];
        self.active = Some(Active {
            source, target: target.into(), x: d.x + d.w / 2.0, y: d.y + d.h / 2.0,
            keys: HashSet::new(), buttons: HashSet::new(), src_keys: HashSet::new(),
            session: None, control_id,
        });
        self.web_lease = Some(Instant::now() + LEASE);
        // State precedes Enter, as expected by the receivers' scoped takeover logic.
        self.broadcast();
        self.send_to(target, HubMsg::Enter { x: 0.5, y: 0.5 });
    }

    pub(super) fn web_alive(&mut self, conn: u64, control_id: &str) {
        self.expire_web_control();
        if self.conns.get(&conn).is_some_and(|c| c.manager)
            && self.active.as_ref().is_some_and(|a| a.source == web_source(conn) && a.control_id == control_id)
        { self.web_lease = Some(Instant::now() + LEASE); }
    }

    pub fn expire_web_control(&mut self) {
        if self.web_lease.is_some_and(|until| Instant::now() >= until) { self.stop(false); }
    }

    pub(super) fn web_text(&mut self, conn: u64, text: String) {
        self.expire_web_control();
        if !self.conns.get(&conn).is_some_and(|c| c.manager) { return; }
        let Some(a) = self.active.as_ref().filter(|a| a.source == web_source(conn)) else { return };
        if !self.snapshot.settings.clipboard {
            self.web_error(conn, "非按键文本需要共享剪贴板，请在首页开启；也可切换远端输入法后使用英文键盘"); return;
        }
        if text.is_empty() || text.len() > MAX_CLIPBOARD_BYTES { return; }
        // Old receivers support clipboard and evdev, not Unicode text injection.
        // Do NOT synthesize an immediate paste: clipboard updates can be asynchronous.
        self.send_to(&a.target, HubMsg::Clipboard { text, from: "手机键盘".into() });
    }
}
