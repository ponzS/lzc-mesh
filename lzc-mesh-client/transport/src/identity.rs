//! Device identity is proved once per management connection. Input packets keep
//! the existing QUIC/TLS authentication and local session/replay checks.
use base64::{engine::general_purpose::STANDARD, Engine as _};
use ring::{digest, rand::{SecureRandom, SystemRandom}, signature::{self, KeyPair}};
use std::ffi::{c_char, CStr, CString};
use zeroize::Zeroize;

pub struct DeviceIdentity {
    key: signature::Ed25519KeyPair,
    pkcs8: Vec<u8>,
}
impl DeviceIdentity {
    pub fn generate() -> Result<Self, ()> {
        let key=signature::Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).map_err(|_|())?;
        Self::from_bytes(key.as_ref())
    }
    fn from_bytes(bytes: &[u8]) -> Result<Self, ()> {
        Ok(Self {key:signature::Ed25519KeyPair::from_pkcs8(bytes).map_err(|_|())?,pkcs8:bytes.to_vec()})
    }
    pub fn from_stored(text: &str) -> Result<Self, ()> {
        if text.len()>1024 {return Err(())}
        let bytes=zeroize::Zeroizing::new(STANDARD.decode(text).map_err(|_|())?);
        Self::from_bytes(&bytes)
    }
    pub fn export(&self) -> String {STANDARD.encode(&self.pkcs8)}
    pub fn public_key(&self) -> String {STANDARD.encode(self.key.public_key().as_ref())}
    pub fn fingerprint(&self) -> String {fingerprint(&self.public_key()).unwrap()}
    pub fn proof(&self,user:&str,id:&str,nonce:&str) -> String {
        STANDARD.encode(self.key.sign(&proof_message(user,id,&self.public_key(),nonce)).as_ref())
    }
}
impl Drop for DeviceIdentity {fn drop(&mut self) {self.pkcs8.zeroize();}}

fn public_bytes(key:&str) -> Option<Vec<u8>> {
    if key.len()!=44 {return None}
    let bytes=STANDARD.decode(key).ok()?;
    (bytes.len()==32 && STANDARD.encode(&bytes)==key).then_some(bytes)
}
pub fn fingerprint(key:&str) -> Option<String> {
    Some(digest::digest(&digest::SHA256,&public_bytes(key)?).as_ref().iter().map(|v|format!("{v:02x}")).collect())
}
#[allow(dead_code)] // Shared hub helper; native clients only answer challenges.
pub fn nonce() -> Result<String,()> {
    let mut bytes=[0u8;32];SystemRandom::new().fill(&mut bytes).map_err(|_|())?;Ok(STANDARD.encode(bytes))
}
fn proof_message(user:&str,id:&str,key:&str,nonce:&str) -> Vec<u8> {
    serde_json::to_vec(&("lzc-mesh/device-proof/v1",user,id,key,nonce)).unwrap()
}
#[allow(dead_code)] // Shared hub helper; exercised by the cross-platform tests.
pub fn verify(key:&str,user:&str,id:&str,nonce:&str,proof:&str) -> bool {
    let Some(key_bytes)=public_bytes(key) else {return false};
    if nonce.len()!=44 || proof.len()!=88 {return false}
    let Ok(bytes)=STANDARD.decode(proof) else {return false};
    signature::UnparsedPublicKey::new(&signature::ED25519,key_bytes)
        .verify(&proof_message(user,id,key,nonce),&bytes).is_ok()
}
fn text<'a>(value:*const c_char) -> Option<&'a str> {
    if value.is_null() {None} else {unsafe {CStr::from_ptr(value)}.to_str().ok()}
}
fn output(value:String) -> *mut c_char {CString::new(value).unwrap().into_raw()}

#[no_mangle]
pub extern "C" fn mesh_identity_new(stored:*const c_char) -> *mut DeviceIdentity {
    let identity=if stored.is_null(){DeviceIdentity::generate()}else{match text(stored){Some("")=>DeviceIdentity::generate(),Some(v)=>DeviceIdentity::from_stored(v),None=>Err(())}};
    identity.map(|v|Box::into_raw(Box::new(v))).unwrap_or(std::ptr::null_mut())
}
#[no_mangle]
pub extern "C" fn mesh_identity_free(identity:*mut DeviceIdentity) {if !identity.is_null(){drop(unsafe {Box::from_raw(identity)})}}
#[no_mangle]
pub extern "C" fn mesh_identity_export(identity:*const DeviceIdentity) -> *mut c_char {
    if identity.is_null(){return std::ptr::null_mut()} output(unsafe {&*identity}.export())
}
#[no_mangle]
pub extern "C" fn mesh_identity_public(identity:*const DeviceIdentity) -> *mut c_char {
    if identity.is_null(){return std::ptr::null_mut()} output(unsafe {&*identity}.public_key())
}
#[no_mangle]
pub extern "C" fn mesh_identity_fingerprint(identity:*const DeviceIdentity) -> *mut c_char {
    if identity.is_null(){return std::ptr::null_mut()} output(unsafe {&*identity}.fingerprint())
}
#[no_mangle]
pub extern "C" fn mesh_identity_proof(identity:*const DeviceIdentity,user:*const c_char,id:*const c_char,nonce:*const c_char) -> *mut c_char {
    let (Some(user),Some(id),Some(nonce))=(text(user),text(id),text(nonce)) else{return std::ptr::null_mut()};
    if identity.is_null() || user.len()>128 || id.len()>64 || nonce.len()!=44{return std::ptr::null_mut()}
    output(unsafe {&*identity}.proof(user,id,nonce))
}
#[no_mangle]
pub extern "C" fn mesh_identity_string_free(value:*mut c_char) {
    if !value.is_null(){let mut bytes=unsafe {CString::from_raw(value)}.into_bytes_with_nul();bytes.zeroize();}
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn persistent_identity_and_bound_proof() {
        let key=DeviceIdentity::generate().unwrap();
        let restored=DeviceIdentity::from_stored(&key.export()).unwrap();
        assert_eq!(key.public_key(),restored.public_key());assert_eq!(key.fingerprint().len(),64);
        let nonce=nonce().unwrap();let proof=restored.proof("account","device",&nonce);
        assert!(verify(&key.public_key(),"account","device",&nonce,&proof));
        assert!(!verify(&key.public_key(),"other","device",&nonce,&proof));
        assert!(!verify(&key.public_key(),"account","other",&nonce,&proof));
        assert!(!verify(&key.public_key(),"account","device",&super::nonce().unwrap(),&proof));
        let other=DeviceIdentity::generate().unwrap();assert!(!verify(&other.public_key(),"account","device",&nonce,&proof));
    }
    #[test] fn invalid_utf8_does_not_generate_a_replacement_key() {
        let bytes=CString::new(vec![0xff]).unwrap();
        assert!(mesh_identity_new(bytes.as_ptr()).is_null());
    }
    #[test] fn invalid_keys_fail_closed() {
        for key in ["bad", "", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="] {
            assert!(DeviceIdentity::from_stored(key).is_err());
        }
        assert!(fingerprint("id-is-not-a-key").is_none());
    }
}
