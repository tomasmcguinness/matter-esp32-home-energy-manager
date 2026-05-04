import SwiftUI
import MatterSupport

struct ContentView: View {
    
    @State private var hubIP        = "192.168.1.100"
    
    private var esp32Client: ESP32Client? {
        guard let url = URL(string: "http://\(hubIP)") else { return nil }
        return ESP32Client(baseURL: url)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Commission Device") {
                    
                    Button("Commission") {
                        Task {
                            await commissionDevice()
                        }
                    }
                    
                }
            }
            .navigationTitle("Home Energy Manager")
        }
    }

    
    private func commissionDevice() async -> Void {
        
        let homes = [MatterAddDeviceRequest.Home(displayName: "My Home")]
        let topology = MatterAddDeviceRequest.Topology(ecosystemName: "MyEcosystemName", homes: homes)
        let request = MatterAddDeviceRequest(topology: topology)
        
        do {
            try await request.perform()
            print("Successfully set up device!")
        } catch {
            print("Failed to set up device with error: \(error.localizedDescription).")
        }
    }
}

#Preview {
    ContentView()
}
