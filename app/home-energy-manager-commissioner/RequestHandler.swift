import MatterSupport
import OSLog

class RequestHandler: MatterAddDeviceExtensionRequestHandler {

    enum PairingError: Error {
        case hubNotConfigured
        case pairingFailed
    }

    private let logger = Logger(subsystem: "com.yourcompany.matterapp", category: "DeviceSetup")

    nonisolated override init() {
        super.init()
        logger.debug("MatterAddDeviceExtensionRequestHandler initialized")
    }

    override func commissionDevice(in home: MatterAddDeviceRequest.Home?, onboardingPayload: String, commissioningID: UUID) async throws {
        logger.debug("commissionDevice payload=\(onboardingPayload)")

        let url = URL(string: "http://home-energy-manager.local")
        
        let client = ESP32Client(baseURL: url!)
        do {
            try await client.commissionDevice(onboardingPayload: onboardingPayload)
            logger.info("Commissioning complete for ID \(commissioningID)")
        } catch {
            logger.error("Commissioning failed: \(error.localizedDescription)")
            throw PairingError.pairingFailed
        }
    }
    
    override func rooms(in home: MatterAddDeviceRequest.Home?) async -> [MatterAddDeviceRequest.Room] {
        logger.debug("Returning empty room list.")
        return []
    }
    
    override func configureDevice(named name: String, in room: MatterAddDeviceRequest.Room?) async {
        logger.debug("Configuring device '\(name)' in room: \(String(describing: room?.displayName))")
        logger.info("Device '\(name)' successfully configured")
    }
    
    
    override func validateDeviceCredential(_ deviceCredential: MatterAddDeviceExtensionRequestHandler.DeviceCredential) async throws {
        logger.debug("Validating device credential")
    }
    
    
    // Override this method to select a specific Wi-Fi network or to ask the Matter framework to select the default WiFi network.
    override func selectWiFiNetwork(from wifiScanResults: [MatterAddDeviceExtensionRequestHandler.WiFiScanResult]) async throws -> MatterAddDeviceExtensionRequestHandler.WiFiNetworkAssociation {
        logger.debug("Using default WiFi network from")
        return .defaultSystemNetwork
    }
    
    
    // Override this method to select a specific Thread network or to ask the Matter framework to select the default Thread network.
    override func selectThreadNetwork(from threadScanResults: [MatterAddDeviceExtensionRequestHandler.ThreadScanResult]) async throws -> MatterAddDeviceExtensionRequestHandler.ThreadNetworkAssociation {
        logger.debug("Using default Thread network")
        return .defaultSystemNetwork
    }
}
