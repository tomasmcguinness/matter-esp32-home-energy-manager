import Matter

class TestStorage: NSObject, MTRStorage {
      private var store: [String: Data] = [:]

      func storageData(forKey key: String) -> Data? {
          return store[key]
      }

      func setStorageData(_ value: Data, forKey key: String) -> Bool {
          store[key] = value
          return true
      }

      func removeStorageData(forKey key: String) -> Bool {
          store.removeValue(forKey: key)
          return true
      }
  }
