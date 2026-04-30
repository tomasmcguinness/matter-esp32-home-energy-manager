import Foundation
import Matter
import CryptoKit
import Security

/// Wraps a CryptoKit P-256 private key as an MTRKeypair.
/// Used to sign the root CA certificate and all NOCs issued to commissioned devices.
@available(iOS 16.4, *)
class RootCAKeyPair: NSObject, MTRKeypair {
    private let privateKey: P256.Signing.PrivateKey
    private let _publicKey: SecKey

    init(privateKey: P256.Signing.PrivateKey) throws {
        self.privateKey = privateKey

        // Convert CryptoKit public key to SecKey using uncompressed X9.63 representation
        let x963 = privateKey.publicKey.x963Representation
        var cfError: Unmanaged<CFError>?
        guard let secKey = SecKeyCreateWithData(
            x963 as CFData,
            [kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom,
             kSecAttrKeyClass: kSecAttrKeyClassPublic] as CFDictionary,
            &cfError
        ) else {
            throw cfError!.takeRetainedValue() as Error
        }
        self._publicKey = secKey
        super.init()
    }

    // MARK: - MTRKeypair

    var publicKey: SecKey { _publicKey }

    func signMessageECDSA_DER(_ message: Data) -> Data? {
        // Matter SDK passes the raw message; CryptoKit hashes with SHA-256 internally
        return try? privateKey.signature(for: message).derRepresentation
    }

    // MARK: - Keychain persistence

    private static let keychainAccount = "hem_matter_root_ca_key"

    static func loadOrCreate() throws -> RootCAKeyPair {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: keychainAccount,
            kSecReturnData as String: true,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
        ]
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)

        if status == errSecSuccess, let data = result as? Data {
            let key = try P256.Signing.PrivateKey(rawRepresentation: data)
            return try RootCAKeyPair(privateKey: key)
        }

        // First run — generate a new key pair and persist it in the Keychain
        let newKey = P256.Signing.PrivateKey()
        let addQuery: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrAccount as String: keychainAccount,
            kSecValueData as String: newKey.rawRepresentation,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
        ]
        let addStatus = SecItemAdd(addQuery as CFDictionary, nil)
        guard addStatus == errSecSuccess else {
            throw NSError(domain: NSOSStatusErrorDomain, code: Int(addStatus))
        }
        return try RootCAKeyPair(privateKey: newKey)
    }
}
