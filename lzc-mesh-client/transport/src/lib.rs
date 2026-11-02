//! LZC Mesh's shared native transport. All protocol state lives on one worker;
//! native callbacks are queued onto the platform's UI loop by the adapters.
mod bluetooth;
mod lan;
mod metrics;
mod model;
mod router;

use model::Wire;
use serde_json::{json, Value};
use std::{
    ffi::{c_char, c_void, CStr, CString},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};
use tokio::sync::mpsc;

pub enum Command {
    Hub(Value),
    Local(Value),
    Applied(Value),
    Connected {
        peer: String,
        conn: quinn::Connection,
        initiated: bool,
    },
    Closed {
        peer: String,
        connection: usize,
    },
    Received {
        peer: String,
        data: Wire,
        direct: bool,
    },
    Disconnect,
    Shutdown,
    #[cfg(test)]
    DropLan,
    #[cfg(test)]
    RestoreLan,
    #[cfg(test)]
    DropNextMotion,
}

impl Command {
    pub fn received(peer: String, mut data: Wire, direct: bool) -> Self {
        if let Wire::Input { input } = &mut data {
            if let Some(trace) = &mut input.trace {
                trace.received_us = metrics::now_us();
            }
        }
        Self::Received { peer, data, direct }
    }
}

#[derive(Clone, Copy)]
pub struct Callback {
    function: extern "C" fn(*mut c_void, *const c_char),
    context: usize,
}
impl Callback {
    pub fn emit(&self, value: Value) {
        if let Ok(text) = CString::new(value.to_string()) {
            (self.function)(self.context as *mut c_void, text.as_ptr());
        }
    }
    pub fn inject(&self, value: Value) {
        self.emit(json!({"_lan":"inject","message":value}));
    }
    pub fn hub(&self, value: Value) {
        self.emit(json!({"_lan":"hub","message":value}));
    }
    pub fn status(&self, text: String) {
        self.emit(json!({"_lan":"status","text":text}));
    }
}

pub struct Handle {
    tx: mpsc::UnboundedSender<Command>,
    available: Arc<AtomicBool>,
    runtime: tokio::runtime::Runtime,
}

/// Callbacks never borrow the supplied JSON beyond the call. The native adapter
/// must copy it and dispatch it to its UI loop. The context must outlive Handle.
#[no_mangle]
pub extern "C" fn mesh_direct_new(
    id: *const c_char,
    callback: extern "C" fn(*mut c_void, *const c_char),
    context: *mut c_void,
) -> *mut Handle {
    if id.is_null() {
        return std::ptr::null_mut();
    }
    let id = unsafe { CStr::from_ptr(id) }.to_string_lossy().into_owned();
    let Ok(runtime) = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
    else {
        return std::ptr::null_mut();
    };
    let (tx, rx) = mpsc::unbounded_channel();
    let available = Arc::new(AtomicBool::new(false));
    let cb = Callback {
        function: callback,
        context: context as usize,
    };
    runtime.spawn(router::run(id, cb, tx.clone(), rx, available.clone()));
    Box::into_raw(Box::new(Handle {
        tx,
        available,
        runtime,
    }))
}

fn parse(text: *const c_char) -> Option<Value> {
    if text.is_null() {
        return None;
    }
    let bytes = unsafe { CStr::from_ptr(text) }.to_bytes();
    if bytes.len() > 1024 * 1024 {
        return None;
    }
    serde_json::from_slice(bytes).ok()
}

#[no_mangle]
pub extern "C" fn mesh_direct_hub(handle: *mut Handle, text: *const c_char) {
    if handle.is_null() {
        return;
    }
    if let Some(v) = parse(text) {
        let _ = unsafe { &*handle }.tx.send(Command::Hub(v));
    }
}

/// Return true only when the new hub has advertised the v1 direct protocol.
/// Older hubs and clients continue to use the original input protocol.
#[no_mangle]
pub extern "C" fn mesh_direct_input(handle: *mut Handle, text: *const c_char) -> bool {
    if handle.is_null() {
        return false;
    }
    let h = unsafe { &*handle };
    if !h.available.load(Ordering::Acquire) {
        return false;
    }
    let Some(v) = parse(text) else { return false };
    if !matches!(
        v["t"].as_str(),
        Some("edge" | "motion" | "button" | "key" | "wheel" | "release")
    ) {
        return false;
    }
    h.tx.send(Command::Local(v)).is_ok()
}

#[no_mangle]
pub extern "C" fn mesh_direct_disconnect(handle: *mut Handle) {
    if !handle.is_null() {
        let h = unsafe { &*handle };
        h.available.store(false, Ordering::Release);
        let _ = h.tx.send(Command::Disconnect);
    }
}

#[no_mangle]
pub extern "C" fn mesh_direct_free(handle: *mut Handle) {
    if handle.is_null() {
        return;
    }
    let h = unsafe { Box::from_raw(handle) };
    let _ = h.tx.send(Command::Shutdown);
    h.runtime
        .shutdown_timeout(std::time::Duration::from_millis(250));
}

#[cfg(test)]
mod tests;

/// Shared process-local clock for native capture/submission timestamps.
#[no_mangle]
pub extern "C" fn mesh_direct_now_us() -> u64 {
    metrics::now_us()
}

/// Native adapters call this after submitting a traced position to the OS.
#[no_mangle]
pub extern "C" fn mesh_direct_applied(handle: *mut Handle, text: *const c_char) {
    if handle.is_null() {
        return;
    }
    if let Some(mut value) = parse(text) {
        value["applied_us"] = json!(metrics::now_us());
        let _ = unsafe { &*handle }.tx.send(Command::Applied(value));
    }
}
