import Foundation
import MatterSupport
import React

@objc(MatterModule)
class MatterModule: NSObject {
  
  @objc
  func commissionDevice() -> Void {
    //RCTLogInfo("My info message")
    await commissionDeviceAsync()
  }
  
  func commissionDeviceAsync() async -> Void {
    let homes = [MatterAddDeviceRequest.Home(displayName: "My Home")]
    let topology = MatterAddDeviceRequest.Topology(ecosystemName: "MyEcosystemName", homes: homes)
    
    let request = MatterAddDeviceRequest(topology: topology)
    
    Task {
        do {
            try await request.perform()
            print("Successfully set up device!")
            resolve("Successfully set up device!")
            
            // Handle the success full setup request and update your app's UI, register the device in your database, or set up any default automations.
//        } catch let error as MyMatterAddDeviceExtensionRequestHandler {
//            // Handle specific Matter errors.
//            switch error {
//            case .cancelled:
//                print("Someone cancelled the setup process.")
//            case .accessDenied:
//                print("Access denied - check entitlements and permissions.")
//            case .unsupported:
//                print("Matter setup is not supported on this device.")
//            default:
//                print("Failed with Matter error: \(error.localizedDescription).")
//            }
        } catch {
            // Handle other errors.
            print("Failed to set up device with error: \(error.localizedDescription).")
            reject("MATTER_ERROR", "Failed to set up device", error)
        }
    }
  }
  
  @objc
  func constantsToExport() -> [String: Any]! {
    return ["someKey": "someValue"]
  }
  
}
