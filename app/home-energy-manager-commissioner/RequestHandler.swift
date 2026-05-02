import MatterSupport
import OSLog

class RequestHandler: MatterAddDeviceExtensionRequestHandler {
    
    enum PairingError: Error {
        case invalidCredentials
        case pairingFailed
    }
    
    private let logger = Logger(subsystem: "com.yourcompany.matterapp", category: "DeviceSetup")
    
    nonisolated override init() {
        super.init()
        logger.debug("MatterAddDeviceExtensionRequestHandler initialized")
    }
    
    override func commissionDevice(in home: MatterAddDeviceRequest.Home?, onboardingPayload: String, commissioningID: UUID) async throws {
        logger.debug("Commissioning device in home '\(String(describing: home?.displayName))' with payload: \(onboardingPayload).")
        
        do {
            // Parse the onboarding payload and commission the device to your app using the Matter framework APIs.
            logger.info("Successfully commissioned device with ID: \(commissioningID)")
            
            
        } catch {
            logger.error("Failed to commission device: \(error.localizedDescription)")
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
