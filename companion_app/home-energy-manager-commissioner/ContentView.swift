import SwiftUI
import MatterSupport

struct ContentView: View {

    @State private var hubIP = "192.168.1.100"
    @State private var commissionedNodeId: UInt64? = nil
    @State private var showNameSheet = false
    @State private var deviceName = ""

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
            .sheet(isPresented: $showNameSheet) {
                DeviceNameSheet(deviceName: $deviceName) {
                    Task { await saveDeviceName() }
                } onSkip: {
                    showNameSheet = false
                }
            }
        }
    }

    private func commissionDevice() async {
        let homes = [MatterAddDeviceRequest.Home(displayName: "My Home")]
        let topology = MatterAddDeviceRequest.Topology(ecosystemName: "MyEcosystemName", homes: homes)
        let request = MatterAddDeviceRequest(topology: topology)

        do {
            try await request.perform()
        } catch {
            print("Failed to set up device with error: \(error.localizedDescription).")
            return
        }

        guard let client = esp32Client else { return }
        do {
            let nodeId = try await client.commissionDevice(onboardingPayload: "")
            commissionedNodeId = nodeId
            deviceName = ""
            showNameSheet = true
        } catch {
            print("Commission request failed: \(error.localizedDescription).")
        }
    }

    private func saveDeviceName() async {
        guard let nodeId = commissionedNodeId, let client = esp32Client, !deviceName.isEmpty else {
            showNameSheet = false
            return
        }
        do {
            try await client.setDeviceName(nodeId: nodeId, name: deviceName)
        } catch {
            print("Failed to save device name: \(error.localizedDescription).")
        }
        showNameSheet = false
    }
}

struct DeviceNameSheet: View {
    @Binding var deviceName: String
    let onSave: () -> Void
    let onSkip: () -> Void

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("e.g. Solar Inverter", text: $deviceName)
                        .autocorrectionDisabled()
                } header: {
                    Text("Give this device a name")
                } footer: {
                    Text("You can change this later.")
                }
            }
            .navigationTitle("Name Device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Skip") { onSkip() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { onSave() }
                        .disabled(deviceName.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }
}

#Preview {
    ContentView()
}
