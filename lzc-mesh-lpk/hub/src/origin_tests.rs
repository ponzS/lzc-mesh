use super::*;
#[test]
fn browser_origin_requires_the_management_origin_and_secure_transport() {
    let check=|host:&str,origin:&str|{
        let mut h=HeaderMap::new();h.insert("host",host.parse().unwrap());h.insert("origin",origin.parse().unwrap());same_origin(&h)
    };
    assert!(check("mesh.example","https://mesh.example"));
    assert!(check("mesh.example:443","https://mesh.example"));
    assert!(check("mesh.example","https://mesh.example:443"));
    assert!(!check("mesh.example","https://untrusted.example"));
    assert!(!check("mesh.example","http://mesh.example"));
    assert!(!check("mesh.example","https://mesh.example:8443"));
    assert!(!check("mesh.example","null"));
    assert!(!check("mesh.example","https://user@mesh.example"));
    assert!(!check("mesh.example","https://mesh.example/?query"));
    assert!(check("127.0.0.1:12345","http://127.0.0.1:12345"));
    assert!(same_origin(&HeaderMap::new()),"non-browser requests still require account authentication at their routes");
}
