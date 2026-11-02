//! Low-rate pointer diagnostics. Every duration is measured on one process's
//! monotonic clock; clocks on the two computers never need synchronisation.
use std::{
    collections::BTreeMap,
    sync::OnceLock,
    time::{Duration, Instant},
};

pub fn now_us() -> u64 {
    static START: OnceLock<Instant> = OnceLock::new();
    START
        .get_or_init(Instant::now)
        .elapsed()
        .as_micros()
        .min(u64::MAX as u128) as u64
}

#[derive(Default)]
pub struct Metrics {
    sent: BTreeMap<(String, u64), (u64, u64)>,
    source: Vec<u64>,
    receiver: Vec<u64>,
    roundtrip: Vec<u64>,
    last_report: Option<Instant>,
    last_sample: Option<Instant>,
    pub generated: u64,
    capture_last: Option<u64>,
    capture_gaps: Vec<u64>,
    pub motion_direct: u64,
    pub motion_relay: u64,
    pub action_direct: u64,
    pub action_relay: u64,
    pub datagram_errors: u64,
    pub motion_not_sent: u64,
    rx_last: Option<u64>,
    rx_since: Option<Instant>,
    rx_gaps: Vec<u64>,
}
impl Metrics {
    pub fn captured(&mut self, when: Option<u64>) {
        self.generated += 1;
        if let Some(when) = when {
            if let Some(last) = self.capture_last {
                self.capture_gaps.push(when.saturating_sub(last));
            }
            self.capture_last = Some(when);
        }
    }
    pub fn sample(&mut self, session: &str, seq: u64, captured: u64) -> Option<u64> {
        if self
            .last_sample
            .is_some_and(|t| t.elapsed() < Duration::from_millis(125))
        {
            return None;
        }
        self.last_sample = Some(Instant::now());
        let queue = now_us().saturating_sub(captured);
        self.sent.insert((session.into(), seq), (captured, queue));
        while self.sent.len() > 64 {
            self.sent.pop_first();
        }
        Some(queue)
    }
    pub fn applied(&mut self, session: &str, seq: u64, receiver_us: u64) {
        let Some((captured, source_us)) = self.sent.remove(&(session.into(), seq)) else {
            return;
        };
        self.source.push(source_us);
        self.receiver.push(receiver_us);
        self.roundtrip.push(now_us().saturating_sub(captured));
    }
    pub fn report(&mut self) -> Option<String> {
        let last = self.last_report.get_or_insert_with(Instant::now);
        if last.elapsed() < Duration::from_secs(3) || self.source.len() < 8 {
            return None;
        }
        *last = Instant::now();
        fn summary(v: &mut Vec<u64>) -> (f64, f64) {
            v.sort_unstable();
            let n = v.len();
            let out = (
                v[n / 2] as f64 / 1000.0,
                v[((n * 95) / 100).min(n - 1)] as f64 / 1000.0,
            );
            v.clear();
            out
        }
        let n = self.source.len();
        let (s50, s95) = summary(&mut self.source);
        let (r50, r95) = summary(&mut self.receiver);
        let (a50, a95) = summary(&mut self.roundtrip);
        let capture = if self.capture_gaps.is_empty() {
            String::new()
        } else {
            self.capture_gaps.sort_unstable();
            let n = self.capture_gaps.len();
            let result=format!(" capture_interval_ms_p50={:.3} capture_interval_ms_p95={:.3} capture_interval_ms_max={:.3}",self.capture_gaps[n/2] as f64/1000.0,self.capture_gaps[((n*95)/100).min(n-1)] as f64/1000.0,*self.capture_gaps.last().unwrap() as f64/1000.0);
            self.capture_gaps.clear();
            result
        };
        let paths=format!(" motion_generated={} motion_direct={} motion_relay={} action_direct={} action_relay={} datagram_errors={} motion_not_sent={}",self.generated,self.motion_direct,self.motion_relay,self.action_direct,self.action_relay,self.datagram_errors,self.motion_not_sent);
        self.generated = 0;
        self.motion_direct = 0;
        self.motion_relay = 0;
        self.action_direct = 0;
        self.action_relay = 0;
        self.datagram_errors = 0;
        self.motion_not_sent = 0;
        Some(format!("pointer_metrics n={n} source_queue_ms_p50={s50:.3} source_queue_ms_p95={s95:.3} receiver_submit_queue_ms_p50={r50:.3} receiver_submit_queue_ms_p95={r95:.3} applied_ack_rtt_ms_p50={a50:.3} applied_ack_rtt_ms_p95={a95:.3}{paths}{capture}"))
    }
    pub fn received_motion(&mut self) {
        let now = now_us();
        if let Some(last) = self.rx_last {
            self.rx_gaps.push(now.saturating_sub(last));
        }
        self.rx_last = Some(now);
        self.rx_since.get_or_insert_with(Instant::now);
    }
    pub fn receiver_report(&mut self) -> Option<String> {
        if !self
            .rx_since
            .is_some_and(|t| t.elapsed() >= Duration::from_secs(3))
            || self.rx_gaps.len() < 8
        {
            return None;
        }
        self.rx_since = Some(Instant::now());
        self.rx_gaps.sort_unstable();
        let n = self.rx_gaps.len();
        let p50 = self.rx_gaps[n / 2] as f64 / 1000.0;
        let p95 = self.rx_gaps[((n * 95) / 100).min(n - 1)] as f64 / 1000.0;
        let max = *self.rx_gaps.last().unwrap() as f64 / 1000.0;
        let over50 = self.rx_gaps.iter().filter(|v| **v > 50_000).count();
        self.rx_gaps.clear();
        Some(format!("pointer_rx_metrics n={n} interval_ms_p50={p50:.3} interval_ms_p95={p95:.3} interval_ms_max={max:.3} gaps_over_50ms={over50}"))
    }
}

#[derive(Default)]
pub struct KeyboardMetrics {
    pub path: Option<bool>,
    pub last_confirm_us: Option<u64>,
    direct: u64,
    relay: u64,
    received: u64,
    confirmations: Vec<u64>,
    since: Option<Instant>,
}
impl KeyboardMetrics {
    pub fn sent(&mut self, direct: bool) {
        self.path = Some(direct);
        if direct {
            self.direct += 1
        } else {
            self.relay += 1
        };
        self.since.get_or_insert_with(Instant::now);
    }
    pub fn received(&mut self, direct: bool) {
        self.path = Some(direct);
        self.received += 1;
        self.since.get_or_insert_with(Instant::now);
    }
    pub fn confirmed(&mut self, elapsed: Duration) {
        let us = elapsed.as_micros().min(u64::MAX as u128) as u64;
        self.last_confirm_us = Some(us);
        self.confirmations.push(us);
    }
    pub fn report(&mut self) -> Option<String> {
        if !self
            .since
            .is_some_and(|t| t.elapsed() >= Duration::from_secs(3))
        {
            return None;
        }
        if self.direct + self.relay + self.received == 0 && self.confirmations.is_empty() {
            return None;
        }
        self.since = Some(Instant::now());
        self.confirmations.sort_unstable();
        let n = self.confirmations.len();
        let (p50, p95) = if n > 0 {
            (
                self.confirmations[n / 2] as f64 / 1000.0,
                self.confirmations[((n * 95) / 100).min(n - 1)] as f64 / 1000.0,
            )
        } else {
            (0.0, 0.0)
        };
        let result=format!("keyboard_metrics sent_direct={} sent_relay={} received={} confirmed={n} confirm_rtt_ms_p50={p50:.3} confirm_rtt_ms_p95={p95:.3}",self.direct,self.relay,self.received);
        self.direct = 0;
        self.relay = 0;
        self.received = 0;
        self.confirmations.clear();
        Some(result)
    }
}
