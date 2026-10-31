//! HTTP API for shared files. Every route is behind lzc-ingress login; the
//! account comes from `X-HC-User-ID`, so each account only sees its own files.
//!
//!   GET    /api/files                 list
//!   PUT    /api/files/{name}          upload (raw body, streamed to disk)
//!   GET    /api/files/{name}          download
//!   DELETE /api/files/{name}          delete

use axum::body::Body;
use axum::extract::{Path, State};
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::Json;
use futures_util::StreamExt;
use tokio::io::AsyncWriteExt;
use tokio_util::io::ReaderStream;

use crate::files::{safe_name, MAX_FILE_BYTES};
use crate::{user_of, App};

fn err(code: StatusCode, msg: &str) -> Response {
    (code, Json(serde_json::json!({ "error": msg }))).into_response()
}

async fn notify(app: &App, user: &str) {
    let files = app.files.list(user);
    app.hub.room(user).await.lock().await.files_changed(files);
}

pub async fn list(State(app): State<App>, headers: HeaderMap) -> Response {
    let Some(user) = user_of(&headers) else { return err(StatusCode::UNAUTHORIZED, "login required") };
    Json(serde_json::json!({ "files": app.files.list(&user), "max_bytes": MAX_FILE_BYTES })).into_response()
}

pub async fn upload(State(app): State<App>, headers: HeaderMap, Path(raw): Path<String>, body: Body) -> Response {
    let Some(user) = user_of(&headers) else { return err(StatusCode::UNAUTHORIZED, "login required") };
    let Some(name) = safe_name(&raw) else { return err(StatusCode::BAD_REQUEST, "invalid file name") };
    let declared = headers.get(header::CONTENT_LENGTH).and_then(|v| v.to_str().ok()).and_then(|v| v.parse::<u64>().ok());
    if declared.is_some_and(|n| n > MAX_FILE_BYTES) {
        return err(StatusCode::PAYLOAD_TOO_LARGE, "file too large");
    }
    let dir = app.files.dir(&user);
    if let Err(e) = tokio::fs::create_dir_all(&dir).await {
        tracing::warn!("create {}: {e}", dir.display());
        return err(StatusCode::INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    // Write to a hidden temp file first so a broken upload never shows up as a file.
    let tmp = dir.join(format!(".upload-{}", unique_suffix()));
    let mut file = match tokio::fs::File::create(&tmp).await {
        Ok(f) => f,
        Err(e) => {
            tracing::warn!("create {}: {e}", tmp.display());
            return err(StatusCode::INTERNAL_SERVER_ERROR, "storage unavailable");
        }
    };
    let mut written: u64 = 0;
    let mut stream = body.into_data_stream();
    while let Some(chunk) = stream.next().await {
        let ok = match chunk {
            Ok(bytes) => {
                written += bytes.len() as u64;
                written <= MAX_FILE_BYTES && file.write_all(&bytes).await.is_ok()
            }
            Err(_) => false,
        };
        if !ok {
            drop(file);
            let _ = tokio::fs::remove_file(&tmp).await;
            return if written > MAX_FILE_BYTES { err(StatusCode::PAYLOAD_TOO_LARGE, "file too large") } else { err(StatusCode::BAD_REQUEST, "upload interrupted") };
        }
    }
    if file.flush().await.is_err() || file.sync_all().await.is_err() {
        let _ = tokio::fs::remove_file(&tmp).await;
        return err(StatusCode::INTERNAL_SERVER_ERROR, "write failed");
    }
    drop(file);
    let final_name = app.files.unique_name(&user, &name);
    if tokio::fs::rename(&tmp, dir.join(&final_name)).await.is_err() {
        let _ = tokio::fs::remove_file(&tmp).await;
        return err(StatusCode::INTERNAL_SERVER_ERROR, "write failed");
    }
    let uploader = headers
        .get("x-lzc-mesh-device")
        .and_then(|v| v.to_str().ok())
        .map(|v| percent_decode(v))
        .filter(|v| !v.trim().is_empty())
        .unwrap_or_else(|| "浏览器".to_string());
    app.files.set_uploader(&user, &final_name, &uploader);
    tracing::info!(user, file = final_name, bytes = written, "uploaded");
    notify(&app, &user).await;
    Json(serde_json::json!({ "name": final_name, "size": written })).into_response()
}

pub async fn download(State(app): State<App>, headers: HeaderMap, Path(name): Path<String>) -> Response {
    let Some(user) = user_of(&headers) else { return err(StatusCode::UNAUTHORIZED, "login required") };
    let Some(path) = app.files.path(&user, &name) else { return err(StatusCode::BAD_REQUEST, "invalid file name") };
    let Ok(file) = tokio::fs::File::open(&path).await else { return err(StatusCode::NOT_FOUND, "not found") };
    let len = file.metadata().await.map(|m| m.len()).unwrap_or(0);
    let mut res = Body::from_stream(ReaderStream::with_capacity(file, 256 * 1024)).into_response();
    let h = res.headers_mut();
    h.insert(header::CONTENT_TYPE, HeaderValue::from_static("application/octet-stream"));
    h.insert(header::CONTENT_LENGTH, HeaderValue::from(len));
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    h.insert("x-content-type-options", HeaderValue::from_static("nosniff"));
    // RFC 6266: ASCII fallback plus the UTF-8 name.
    let ascii: String = name.chars().map(|c| if c.is_ascii_alphanumeric() || ".-_ ()".contains(c) { c } else { '_' }).collect();
    let disposition = format!("attachment; filename=\"{ascii}\"; filename*=UTF-8''{}", percent_encode(&name));
    if let Ok(v) = HeaderValue::from_str(&disposition) {
        h.insert(header::CONTENT_DISPOSITION, v);
    }
    res
}

pub async fn remove(State(app): State<App>, headers: HeaderMap, Path(name): Path<String>) -> Response {
    let Some(user) = user_of(&headers) else { return err(StatusCode::UNAUTHORIZED, "login required") };
    if !app.files.remove(&user, &name) {
        return err(StatusCode::NOT_FOUND, "not found");
    }
    tracing::info!(user, file = name, "deleted");
    notify(&app, &user).await;
    StatusCode::NO_CONTENT.into_response()
}

fn unique_suffix() -> u64 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(1);
    NEXT.fetch_add(1, Ordering::Relaxed)
}

fn percent_encode(s: &str) -> String {
    s.bytes()
        .map(|b| if b.is_ascii_alphanumeric() || b"-._~".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") })
        .collect()
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            if let Ok(v) = u8::from_str_radix(std::str::from_utf8(&bytes[i + 1..i + 3]).unwrap_or(""), 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_encoding_round_trips_unicode_names() {
        assert_eq!(percent_decode(&percent_encode("Mac 的报告 (2).pdf")), "Mac 的报告 (2).pdf");
        assert_eq!(percent_decode("abc%"), "abc%");
        assert_eq!(percent_decode("%E4%B8%AD"), "中");
    }
}
