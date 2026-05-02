class TestStorage: NSObject, MTRStorage {
      func storageData(forKey key: String) -> Data? { /* read from UserDefaults/Keychain */ }
      func setStorageData(_ value: Data, forKey key: String) -> Bool { /* write */ }
      func removeStorageData(forKey key: String) -> Bool { /* delete */ }
}
