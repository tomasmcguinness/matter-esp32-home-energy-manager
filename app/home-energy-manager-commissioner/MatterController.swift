import Foundation
import Matter

enum MatterControllerError: Error {
    case error
}

class MatterController {
    var controller: MTRDeviceController?
    
    func start() throws {
        let factoryParams = MTRDeviceControllerFactoryParams(storage: TestStorage())
        
        let factory = MTRDeviceControllerFactory.sharedInstance()
        try factory.start(factoryParams)
        
        do {
            let ipk = try generateIPK()
            
            let rootKeypair = MatterKeypair()
            try rootKeypair.initialize()
            
            let params = MTRDeviceControllerStartupParams(
                ipk: ipk,
                fabricID: 1,
                nocSigner: rootKeypair
            )
            params.vendorID = 0xFFF1 as NSNumber  // test vendor ID
            
            controller = try factory.createController(onNewFabric: params)
            
            let setupPayload = try MTRSetupPayload(onboardingPayload: "3497-011-2332")
            let nodeId: NSNumber = 0x23
            
            try controller!.setupCommissioningSession(with: setupPayload, newNodeID: nodeId)
        }
        catch
        {
            print("Failed")
        }
    }
    
    func generateIPK() throws -> Data {
        var bytes = [UInt8](repeating: 0, count: 16)
        let status = SecRandomCopyBytes(kSecRandomDefault, 16, &bytes)
        
        guard status == errSecSuccess else {
            throw MatterControllerError.error
        }
        return Data(bytes)
    }
}
