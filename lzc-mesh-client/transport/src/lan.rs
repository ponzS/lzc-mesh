//! Mutually authenticated QUIC. Certificates are exchanged only through the
//! authenticated, account-scoped hub connection; no accept-any verifier exists.
use crate::{
    model::{Peer, Wire},
    Command,
};
use base64::{engine::general_purpose::STANDARD, Engine as _};
use quinn::{Connection, Endpoint, TransportConfig};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer};
use std::{
    collections::HashMap,
    net::{Ipv4Addr, SocketAddr},
    sync::Arc,
    time::Duration,
};
use tokio::{
    io::{AsyncBufReadExt, AsyncReadExt, BufReader},
    sync::mpsc,
};

pub struct Identity {
    pub endpoint: Endpoint,
    pub cert: CertificateDer<'static>,
    key: Vec<u8>,
    pub addresses: Vec<String>,
}

fn transport() -> Arc<TransportConfig> {
    let mut c = TransportConfig::default();
    c.keep_alive_interval(Some(Duration::from_millis(250)));
    c.max_idle_timeout(Some(Duration::from_secs(2).try_into().unwrap()));
    c.datagram_send_buffer_size(2048);
    c.datagram_receive_buffer_size(Some(8192));
    c.max_concurrent_uni_streams(0u32.into());
    c.max_concurrent_bidi_streams(2u32.into());
    Arc::new(c)
}

fn discover(port: u16) -> Vec<String> {
    let mut addresses: Vec<String> = if_addrs::get_if_addrs()
        .unwrap_or_default()
        .into_iter()
        .filter_map(|iface| match iface.ip() {
            std::net::IpAddr::V4(ip) if ip.is_private() && !ip.is_loopback() => {
                Some(SocketAddr::from((ip, port)).to_string())
            }
            _ => None,
        })
        .collect();
    if cfg!(test) {
        addresses.push(SocketAddr::from((Ipv4Addr::LOCALHOST, port)).to_string());
    }
    addresses.sort();
    addresses.dedup();
    addresses
}

impl Identity {
    pub fn new() -> Result<Self, String> {
        let certified = rcgen::generate_simple_self_signed(vec!["lzc-mesh.local".into()])
            .map_err(|e| e.to_string())?;
        let cert = certified.cert.der().clone();
        let key = certified.signing_key.serialize_der();
        let endpoint = Endpoint::client(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)))
            .map_err(|e| e.to_string())?;
        let port = endpoint.local_addr().map_err(|e| e.to_string())?.port();
        let addresses = discover(port);
        Ok(Self {
            endpoint,
            cert,
            key,
            addresses,
        })
    }
    pub fn refresh_addresses(&mut self) -> bool {
        let Ok(addr) = self.endpoint.local_addr() else {
            return false;
        };
        let addresses = discover(addr.port());
        if addresses == self.addresses {
            return false;
        }
        self.addresses = addresses;
        true
    }
    pub fn advert(&self) -> serde_json::Value {
        serde_json::json!({"t":"direct_hello","version":1,"certificate":STANDARD.encode(&self.cert),"addresses":self.addresses})
    }
    pub fn configure(&self, peers: &HashMap<String, Peer>) -> Result<(), String> {
        let mut roots = rustls::RootCertStore::empty();
        for peer in peers.values() {
            if let Ok(der) = STANDARD.decode(&peer.certificate) {
                let _ = roots.add(CertificateDer::from(der));
            }
        }
        if roots.is_empty() {
            self.endpoint.set_server_config(None);
            return Ok(());
        }
        let verifier = rustls::server::WebPkiClientVerifier::builder(Arc::new(roots))
            .build()
            .map_err(|e| e.to_string())?;
        let mut tls = rustls::ServerConfig::builder()
            .with_client_cert_verifier(verifier)
            .with_single_cert(
                vec![self.cert.clone()],
                PrivatePkcs8KeyDer::from(self.key.clone()).into(),
            )
            .map_err(|e| e.to_string())?;
        tls.alpn_protocols = vec![b"lzc-mesh/1".to_vec()];
        let crypto =
            quinn::crypto::rustls::QuicServerConfig::try_from(tls).map_err(|e| e.to_string())?;
        let mut config = quinn::ServerConfig::with_crypto(Arc::new(crypto));
        config.transport = transport();
        self.endpoint.set_server_config(Some(config));
        Ok(())
    }
    fn client_config(&self, peer: &Peer) -> Result<quinn::ClientConfig, String> {
        let der = STANDARD
            .decode(&peer.certificate)
            .map_err(|e| e.to_string())?;
        let mut roots = rustls::RootCertStore::empty();
        roots
            .add(CertificateDer::from(der))
            .map_err(|e| e.to_string())?;
        let mut tls = rustls::ClientConfig::builder()
            .with_root_certificates(roots)
            .with_client_auth_cert(
                vec![self.cert.clone()],
                PrivatePkcs8KeyDer::from(self.key.clone()).into(),
            )
            .map_err(|e| e.to_string())?;
        tls.alpn_protocols = vec![b"lzc-mesh/1".to_vec()];
        let crypto =
            quinn::crypto::rustls::QuicClientConfig::try_from(tls).map_err(|e| e.to_string())?;
        let mut c = quinn::ClientConfig::new(Arc::new(crypto));
        c.transport_config(transport());
        Ok(c)
    }
    pub fn dial(&self, peer: Peer, tx: mpsc::UnboundedSender<Command>) {
        let Ok(config) = self.client_config(&peer) else {
            return;
        };
        for address in &peer.addresses {
            let Ok(addr) = address.parse::<SocketAddr>() else {
                continue;
            };
            // Only LAN IPv4 addresses from authenticated discovery are dialled.
            let std::net::IpAddr::V4(ip) = addr.ip() else {
                continue;
            };
            if !ip.is_private() && !(cfg!(test) && ip.is_loopback()) {
                continue;
            }
            let Ok(connecting) = self
                .endpoint
                .connect_with(config.clone(), addr, "lzc-mesh.local")
            else {
                continue;
            };
            let tx = tx.clone();
            let peer = peer.id.clone();
            tokio::spawn(async move {
                if let Ok(Ok(conn)) =
                    tokio::time::timeout(Duration::from_millis(900), connecting).await
                {
                    let _ = tx.send(Command::Connected {
                        peer,
                        conn,
                        initiated: true,
                    });
                }
            });
        }
    }
}

pub fn certificate(conn: &Connection) -> Option<String> {
    let certs = conn
        .peer_identity()?
        .downcast::<Vec<CertificateDer<'static>>>()
        .ok()?;
    certs.first().map(|cert| STANDARD.encode(cert))
}

/// One ordered stream per connection. Datagrams carry only replaceable positions.
pub fn run_connection(
    peer: String,
    conn: Connection,
    initiated: bool,
    tx: mpsc::UnboundedSender<Command>,
) -> mpsc::Sender<Wire> {
    let (out, mut rx) = mpsc::channel::<Wire>(128);
    let c = conn.clone();
    let p = peer.clone();
    let t = tx.clone();
    tokio::spawn(async move {
        let streams = if initiated {
            c.open_bi().await
        } else {
            c.accept_bi().await
        };
        let Ok((mut send, recv)) = streams else {
            let _ = t.send(Command::Closed {
                peer: p,
                connection: c.stable_id(),
            });
            return;
        };
        // Force opening the stream on the wire before either side has input.
        if send
            .write_all(b"{\"kind\":\"ping\",\"nonce\":0}\n")
            .await
            .is_err()
        {
            return;
        }
        let t2 = t.clone();
        let p2 = p.clone();
        let reader = tokio::spawn(async move {
            let mut reader = BufReader::new(recv);
            let mut line = Vec::new();
            loop {
                line.clear();
                // Limit frames before parsing. All direct messages are small inputs.
                let n = match (&mut reader).take(16385).read_until(b'\n', &mut line).await {
                    Ok(n) => n,
                    Err(_) => break,
                };
                if n == 0 || n > 16384 {
                    break;
                }
                if let Ok(data) = serde_json::from_slice::<Wire>(&line) {
                    let _ = t2.send(Command::received(p2.clone(), data, true));
                }
            }
        });
        while let Some(data) = rx.recv().await {
            let Ok(mut bytes) = serde_json::to_vec(&data) else {
                continue;
            };
            bytes.push(b'\n');
            if send.write_all(&bytes).await.is_err() {
                break;
            }
        }
        reader.abort();
        let _ = t.send(Command::Closed {
            peer: p,
            connection: c.stable_id(),
        });
    });
    let p = peer;
    let c = conn;
    tokio::spawn(async move {
        while let Ok(bytes) = c.read_datagram().await {
            if bytes.len() > 4096 {
                continue;
            }
            if let Ok(data) = serde_json::from_slice::<Wire>(&bytes) {
                // An unreliable datagram can never carry key/button/scroll actions.
                if matches!(&data,Wire::Input {input} if input.event.is_none())
                    || matches!(
                        &data,
                        Wire::Ping { .. } | Wire::Pong { .. } | Wire::MotionAck { .. }
                    )
                {
                    let _ = tx.send(Command::received(p.clone(), data, true));
                }
            }
        }
        let _ = tx.send(Command::Closed {
            peer: p,
            connection: c.stable_id(),
        });
    });
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn unregistered_client_certificate_is_rejected_by_tls() {
        let server = Identity::new().unwrap();
        let trusted = Identity::new().unwrap();
        let attacker = Identity::new().unwrap();
        let p = Peer {
            id: "trusted".into(),
            certificate: STANDARD.encode(&trusted.cert),
            addresses: vec![],
            bluetooth: None,
        };
        server
            .configure(&HashMap::from([("trusted".into(), p)]))
            .unwrap();
        let target = Peer {
            id: "server".into(),
            certificate: STANDARD.encode(&server.cert),
            addresses: vec![],
            bluetooth: None,
        };
        let address = SocketAddr::from((
            Ipv4Addr::LOCALHOST,
            server.endpoint.local_addr().unwrap().port(),
        ));
        let server_task = tokio::spawn(async move {
            let incoming = server.endpoint.accept().await.unwrap();
            let refused = tokio::time::timeout(Duration::from_secs(2), incoming).await;
            assert!(
                matches!(refused, Ok(Err(_))),
                "TLS admitted an unregistered client certificate"
            );
        });
        let connecting = attacker
            .endpoint
            .connect_with(
                attacker.client_config(&target).unwrap(),
                address,
                "lzc-mesh.local",
            )
            .unwrap();
        if let Ok(Ok(conn)) = tokio::time::timeout(Duration::from_secs(2), connecting).await {
            // TLS 1.3 may let the client complete locally before the server has
            // verified the client's Certificate message. It must then close.
            tokio::time::timeout(Duration::from_secs(2), conn.closed())
                .await
                .unwrap();
        }
        server_task.await.unwrap();
    }
}
