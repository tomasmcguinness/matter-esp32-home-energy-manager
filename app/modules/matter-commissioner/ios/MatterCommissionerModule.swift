import ExpoModulesCore
import Matter

public class MatterCommissionerModule: Module {
    public func definition() -> ModuleDefinition {
        Name("MatterCommissioner")

        AsyncFunction("initialize") { () -> [String: String] in
            guard #available(iOS 16.4, *) else {
                throw NSError(
                    domain: "MatterCommissioner",
                    code: 0,
                    userInfo: [NSLocalizedDescriptionKey: "Matter commissioning requires iOS 16.4 or later"]
                )
            }
            let controller = await MatterController.shared
            try await MainActor.run { try controller.initialize() }
            let nodeId = await MainActor.run { controller.controllerNodeId }
            return [
                "fabricId": "1",
                "nodeId": String(nodeId),
            ]
        }

        AsyncFunction("commissionDevice") { (setupPayload: String) -> [String: String] in
            guard #available(iOS 16.4, *) else {
                throw NSError(
                    domain: "MatterCommissioner",
                    code: 0,
                    userInfo: [NSLocalizedDescriptionKey: "Matter commissioning requires iOS 16.4 or later"]
                )
            }
            let nodeId = try await MatterController.shared.commissionDevice(setupPayload: setupPayload)
            return ["nodeId": String(nodeId)]
        }

        AsyncFunction("grantControllerAccess") { (deviceNodeIdStr: String, controllerNodeIdStr: String) in
            guard #available(iOS 16.4, *) else {
                throw NSError(
                    domain: "MatterCommissioner",
                    code: 0,
                    userInfo: [NSLocalizedDescriptionKey: "Matter commissioning requires iOS 16.4 or later"]
                )
            }
            guard let deviceNodeId = UInt64(deviceNodeIdStr),
                  let controllerNodeId = UInt64(controllerNodeIdStr) else {
                throw NSError(
                    domain: "MatterCommissioner",
                    code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "Invalid node ID format — expected decimal string"]
                )
            }
            try await MatterController.shared.grantControllerAccess(
                deviceNodeId: deviceNodeId,
                controllerNodeId: controllerNodeId
            )
        }

        // Reads directly from UserDefaults — safe to call synchronously
        Function("getCommissionedNodeIds") { () -> [String] in
            UserDefaults.standard.array(forKey: "hem_commissioned_node_ids") as? [String] ?? []
        }

        Function("getHemNodeId") { () -> String? in
            UserDefaults.standard.string(forKey: "hem_rnapp_hem_node_id")
        }

        Function("setHemNodeId") { (nodeId: String) in
            UserDefaults.standard.set(nodeId, forKey: "hem_rnapp_hem_node_id")
        }

        Function("getDeviceLabels") { () -> [String: String] in
            UserDefaults.standard.dictionary(forKey: "hem_device_labels") as? [String: String] ?? [:]
        }

        Function("setDeviceLabel") { (nodeId: String, label: String) in
            var labels = UserDefaults.standard.dictionary(forKey: "hem_device_labels") as? [String: String] ?? [:]
            labels[nodeId] = label
            UserDefaults.standard.set(labels, forKey: "hem_device_labels")
        }
    }
}
