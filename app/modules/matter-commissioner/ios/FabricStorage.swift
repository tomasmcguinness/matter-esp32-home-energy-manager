import Foundation
import Matter

/// Persists Matter fabric credentials using UserDefaults.
/// Keys from root CA cert to fabric index counters are stored here.
/// Sensitive key material (root CA private key) is stored in the Keychain instead.
@available(iOS 16.4, *)
class FabricStorage: NSObject, MTRStorage {
    private let defaults = UserDefaults.standard
    private let keyPrefix = "hem_matter_storage_"

    func storageData(forKey key: String) -> Data? {
        defaults.data(forKey: keyPrefix + key)
    }

    func setStorageData(_ value: Data, forKey key: String) -> Bool {
        defaults.set(value, forKey: keyPrefix + key)
        return true
    }

    func removeStorageData(forKey key: String) -> Bool {
        defaults.removeObject(forKey: keyPrefix + key)
        return true
    }
}
