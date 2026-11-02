import Foundation
import Security

/// Long-lived device key, separate from the existing device ID and preferences.
/// A failed Keychain read is an error, never permission to replace a binding.
final class DeviceIdentity {
    enum IdentityError: Error { case keychain(OSStatus), invalidStoredKey, nativeFailure }
    private let handle: OpaquePointer
    let publicKey: String
    let fingerprint: String

    init(service: String = "cloud.lazycat.lzcmesh.device-identity") throws {
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service, kSecAttrAccount as String: "device-key-v1",
            kSecAttrSynchronizable as String: false]
        var lookup = query
        lookup[kSecReturnData as String] = true
        lookup[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: CFTypeRef?
        let status = SecItemCopyMatching(lookup as CFDictionary, &result)
        var key: OpaquePointer?
        if status == errSecSuccess {
            guard let data = result as? Data, !data.isEmpty,
                  let stored = String(data: data, encoding: .utf8), !stored.contains("\0") else { throw IdentityError.invalidStoredKey }
            key = stored.withCString { mesh_identity_new($0) }
        } else if status == errSecItemNotFound {
            key = mesh_identity_new(nil)
            guard let key else { throw IdentityError.nativeFailure }
            guard let exported = mesh_identity_export(key) else { mesh_identity_free(key); throw IdentityError.nativeFailure }
            var item = query
            item[kSecValueData as String] = Data(bytes: exported, count: strlen(exported))
            item[kSecAttrLabel as String] = "懒猫设备接力 · 设备身份"
            let added = SecItemAdd(item as CFDictionary, nil)
            mesh_identity_string_free(exported)
            if added != errSecSuccess { mesh_identity_free(key); throw IdentityError.keychain(added) }
        } else { throw IdentityError.keychain(status) }
        guard let key else { throw IdentityError.invalidStoredKey }
        handle = key
        publicKey = Self.take(mesh_identity_public(key))
        fingerprint = Self.take(mesh_identity_fingerprint(key))
    }
    deinit { mesh_identity_free(handle) }
    func proof(user: String, deviceID: String, nonce: String) -> String {
        user.withCString { user in deviceID.withCString { id in nonce.withCString { nonce in
            Self.take(mesh_identity_proof(handle, user, id, nonce))
        } } }
    }
    private static func take(_ value: UnsafeMutablePointer<CChar>?) -> String {
        guard let value else { return "" }
        defer { mesh_identity_string_free(value) }
        return String(cString: value)
    }
}
