//! 懒猫设备接力 (LZC Mesh) hub. Runs inside the LPK, behind lzc-ingress which authenticates
//! every request and injects `X-HC-User-ID`. Native clients reach it through
//! the LazyCat network using the session cookie of the local LazyCat client.

#[path = "../../../lzc-mesh-client/transport/src/identity.rs"]
mod identity;
mod files;
mod files_api;
mod hub;
mod layout;
mod protocol;
mod store;

use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Duration;

use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::extract::DefaultBodyLimit;
use axum::routing::get;
use axum::{Json, Router};
use axum::serve::ListenerExt;
use futures_util::{SinkExt, StreamExt};
use tokio::sync::mpsc;
use tower_http::services::ServeDir;

use files::Files;
use hub::Hub;
use protocol::{ClientMsg, HubMsg};
use store::Store;

#[cfg(test)]
mod origin_tests;

static NEXT_CONN: AtomicU64 = AtomicU64::new(1);

#[derive(Clone)]
pub struct App {
    pub hub: Hub,
    pub files: Files,
}

pub fn user_of(headers: &HeaderMap) -> Option<String> {
    let user = headers.get("x-hc-user-id")?.to_str().ok()?.trim();
    (!user.is_empty() && user.len() <= 128).then(|| user.to_string())
}

async fn whoami(headers: HeaderMap) -> Response {
    match user_of(&headers) {
        Some(user) => Json(serde_json::json!({ "user": user, "version": env!("CARGO_PKG_VERSION"), "device_auth_version": 1 })).into_response(),
        None => StatusCode::UNAUTHORIZED.into_response(),
    }
}

async fn ws(State(app): State<App>, headers: HeaderMap, upgrade: WebSocketUpgrade) -> Response {
    let Some(user) = user_of(&headers) else {
        return (StatusCode::UNAUTHORIZED, "login required").into_response();
    };
    if !same_origin(&headers) { return (StatusCode::FORBIDDEN, "origin rejected").into_response(); }
    upgrade.max_message_size(64 * 1024).on_upgrade(move |socket| session(app, user, socket))
}

pub fn same_origin(headers: &HeaderMap) -> bool {
    let Some(origin) = headers.get("origin") else { return true };
    let (Ok(origin), Some(host)) = (origin.to_str(), headers.get("host").and_then(|v|v.to_str().ok())) else { return false };
    let (Ok(uri),Ok(host)) = (origin.parse::<axum::http::Uri>(),host.parse::<axum::http::uri::Authority>()) else { return false };
    let Some(authority)=uri.authority() else{return false};
    let local=matches!(host.host(),"127.0.0.1"|"localhost"|"[::1]");
    let port=match uri.scheme_str(){Some("https")=>443,Some("http") if local=>80,_=>return false};
    !authority.as_str().contains('@') && !host.as_str().contains('@')
        && authority.host().eq_ignore_ascii_case(host.host())
        && authority.port_u16().unwrap_or(port)==host.port_u16().unwrap_or(port)
        && !(authority.port().is_some() && authority.port_u16().is_none())
        && !(host.port().is_some() && host.port_u16().is_none())
        && uri.path().trim_matches('/').is_empty() && uri.query().is_none()
}

async fn session(app: App, user: String, socket: WebSocket) {
    let conn = NEXT_CONN.fetch_add(1, Ordering::Relaxed);
    let room = app.hub.room(&user).await;
    let (tx, mut rx) = mpsc::unbounded_channel::<HubMsg>();
    let (mut sink, mut stream) = socket.split();
    {
        let mut r = room.lock().await;
        r.attach(conn, tx);
        r.files_changed(app.files.list(&user));
    }
    tracing::info!(user, conn, "connected");

    let writer = tokio::spawn(async move {
        let mut ping = tokio::time::interval(Duration::from_secs(20));
        loop {
            tokio::select! {
                msg = rx.recv() => {
                    let Some(msg) = msg else { break };
                    let text = serde_json::to_string(&msg).unwrap_or_default();
                    if sink.send(Message::Text(text.into())).await.is_err() { break }
                }
                _ = ping.tick() => {
                    if sink.send(Message::Ping(Default::default())).await.is_err() { break }
                }
            }
        }
    });

    // Browser control has a short renewable lease even if TCP remains half-open.
    let mut lease_tick = tokio::time::interval(Duration::from_secs(1));
    let idle = tokio::time::sleep(Duration::from_secs(60));
    tokio::pin!(idle);
    loop {
        let frame = tokio::select! {
            _ = lease_tick.tick() => { room.lock().await.expire_web_control(); continue; }
            _ = &mut idle => break,
            frame = stream.next() => frame,
        };
        let Some(Ok(frame)) = frame else { break };
        idle.as_mut().reset(tokio::time::Instant::now() + Duration::from_secs(60));
        match frame {
            Message::Text(text) => match serde_json::from_str::<ClientMsg>(&text) {
                Ok(msg) => {
                    let refresh_files = matches!(&msg, ClientMsg::Hello { .. } | ClientMsg::DeviceProof { .. } | ClientMsg::DeviceTrust { approved: true, .. } | ClientMsg::DeviceEnabled { enabled: true, .. });
                    let mut r = room.lock().await;
                    r.handle(conn, msg);
                    if refresh_files { r.files_changed(app.files.list(&user)); }
                },
                Err(err) => tracing::debug!(conn, "bad frame: {err}"),
            },
            Message::Close(_) => break,
            _ => {}
        }
    }
    room.lock().await.detach(conn);
    writer.abort();
    tracing::info!(user, conn, "disconnected");
}

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt()
        .with_env_filter(std::env::var("RUST_LOG").unwrap_or_else(|_| "info".into()))
        .init();
    let data = PathBuf::from(std::env::var("LZC_MESH_DATA").unwrap_or_else(|_| "/lzcapp/var/layouts".into()));
    let web = PathBuf::from(std::env::var("LZC_MESH_WEB").unwrap_or_else(|_| "/lzcapp/pkg/content/web".into()));
    let addr: SocketAddr = std::env::var("LZC_MESH_LISTEN").unwrap_or_else(|_| "0.0.0.0:8080".into()).parse().expect("listen address");

    let files_dir = PathBuf::from(std::env::var("LZC_MESH_FILES").unwrap_or_else(|_| "/lzcapp/var/files".into()));
    let app = App { hub: Hub::new(Store::new(data)), files: Files::new(files_dir) };
    let router = Router::new()
        .route("/api/whoami", get(whoami))
        .route("/api/ws", get(ws))
        .route("/api/health", get(|| async { "ok" }))
        .route("/api/files", get(files_api::list))
        .route(
            "/api/files/{name}",
            get(files_api::download).put(files_api::upload).delete(files_api::remove).layer(DefaultBodyLimit::disable()),
        )
        .fallback_service(ServeDir::new(web).append_index_html_on_directories(true))
        .with_state(app);
    let listener = tokio::net::TcpListener::bind(addr).await.expect("bind").tap_io(|stream| {
        if let Err(err) = stream.set_nodelay(true) { tracing::warn!(%err, "TCP_NODELAY unavailable"); }
    });
    tracing::info!("lzc-mesh hub listening on {addr}");
    axum::serve(listener, router)
        .with_graceful_shutdown(async {
            let _ = tokio::signal::ctrl_c().await;
        })
        .await
        .expect("serve");
}
