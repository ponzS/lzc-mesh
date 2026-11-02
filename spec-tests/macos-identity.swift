import Foundation
import Security

@main struct IdentityProbe {
    static func main() {
        let args=CommandLine.arguments
        guard args.count>=2, args[1].hasPrefix("cloud.lazycat.lzcmesh.fixture.") else { exit(2) }
        let service=args[1]
        if args.count>2 && args[2]=="delete" {
            let query:[String:Any]=[kSecClass as String:kSecClassGenericPassword,
                kSecAttrService as String:service,kSecAttrAccount as String:"device-key-v1",kSecAttrSynchronizable as String:false]
            let status=SecItemDelete(query as CFDictionary)
            guard status==errSecSuccess || status==errSecItemNotFound else { exit(3) }
            return
        }
        do {
            let key=try DeviceIdentity(service:service)
            print(key.fingerprint);print(key.publicKey)
            if args.count>2 {print(key.proof(user:"fixture-account",deviceID:"fixture-macos",nonce:args[2]))}
        } catch {
            FileHandle.standardError.write(Data("Fixture Keychain error: \(error)\n".utf8));exit(1)
        }
    }
}
