import Foundation
import Matter
import CryptoKit
import Security

// MARK: - Errors

enum MatterControllerError: LocalizedError {
    case notInitialized
    case commissioningInProgress
    case rootCertGenerationFailed
    case nocGenerationFailed
    case csrParseFailed
    case deviceNotFound
    case aclUpdateFailed(String)
    case commissioningFailed(String)

    var errorDescription: String? {
        switch self {
        case .notInitialized:          return "Matter controller not initialized — call initialize() first"
        case .commissioningInProgress: return "Another commissioning session is already in progress"
        case .rootCertGenerationFailed: return "Failed to generate root CA certificate"
        case .nocGenerationFailed:     return "Failed to generate Node Operational Certificate"
        case .csrParseFailed:          return "Failed to parse device CSR — unexpected format"
        case .deviceNotFound:          return "Device not found on fabric"
        case .aclUpdateFailed(let m):  return "ACL update failed: \(m)"
        case .commissioningFailed(let m): return "Commissioning failed: \(m)"
        }
    }
}

// MARK: - MatterController

/// Singleton that owns the MTRDeviceController for our custom Matter fabric.
/// Handles commissioning new devices and updating ACL entries.
@available(iOS 16.4, *)
@MainActor
final class MatterController: NSObject {
    static let shared = MatterController()

    private var deviceController: MTRDeviceController?
    private var rootKeyPair: RootCAKeyPair?
    private var rootCertData: Data?
    private var ipkData: Data?
    private(set) var controllerNodeId: UInt64 = 0

    // Single pending commissioning session
    private var pendingCommission: (nodeId: UInt64, completion: (Result<UInt64, Error>) -> Void)?

    private let fabricIDKey    = "hem_fabric_id"
    private let rootCertKey    = "hem_root_cert_data"
    private let ipkKey         = "hem_ipk_data"
    private let fabricReadyKey = "hem_fabric_ready"
    private let nodeIdsKey     = "hem_commissioned_node_ids"

    private static let controllerNodeID: UInt64 = 1
    private static let fabricID: UInt64 = 1

    // MARK: - Public API

    var isInitialized: Bool { deviceController?.isRunning == true }

    /// Initialize or resume the Matter fabric and controller.
    func initialize() throws {
        guard !isInitialized else { return }

        rootKeyPair = try RootCAKeyPair.loadOrCreate()
        ipkData = loadOrCreateIPK()

        let fabricReady = UserDefaults.standard.bool(forKey: fabricReadyKey)

        // Generate or load the root certificate
        if fabricReady, let cached = UserDefaults.standard.data(forKey: rootCertKey) {
            rootCertData = cached
        } else {
            guard let cert = MTRCertificates.generateRootCertificate(
                withIssuerID: nil,
                fabricID: NSNumber(value: Self.fabricID),
                publicKey: rootKeyPair!.publicKey,
                signingKey: rootKeyPair!,
                error: nil
            ) else {
                throw MatterControllerError.rootCertGenerationFailed
            }
            rootCertData = cert
            UserDefaults.standard.set(cert, forKey: rootCertKey)
        }

        // Start the controller factory
        let factoryParams = MTRDeviceControllerFactoryParams(storage: FabricStorage())
        // Skip PAA validation so prototype/dev devices without real certs can be commissioned
        factoryParams.productAttestationAuthorityCertificates = []
        try MTRDeviceControllerFactory.sharedInstance().start(factoryParams)

        // Build startup params
        let startupParams = MTRDeviceControllerStartupParams(
            ipk: ipkData!,
            fabricID: NSNumber(value: Self.fabricID),
            nocSigner: self
        )
        startupParams.vendorID = NSNumber(value: 0xFFF4)  // Test Vendor ID for dev builds
        startupParams.nodeID = NSNumber(value: Self.controllerNodeID)
        startupParams.rootCACertificate = rootCertData

        if fabricReady {
            deviceController = try MTRDeviceControllerFactory.sharedInstance()
                .createController(onExistingFabric: startupParams)
        } else {
            deviceController = try MTRDeviceControllerFactory.sharedInstance()
                .createController(onNewFabric: startupParams)
            UserDefaults.standard.set(true, forKey: fabricReadyKey)
        }

        controllerNodeId = Self.controllerNodeID
        deviceController?.setDeviceControllerDelegate(self, queue: .main)
    }

    /// Commission a device using its Matter QR code or 11-digit manual pairing code.
    /// Returns the new device's node ID on our fabric.
    func commissionDevice(setupPayload payloadString: String) async throws -> UInt64 {
        guard let controller = deviceController, controller.isRunning else {
            throw MatterControllerError.notInitialized
        }
        guard pendingCommission == nil else {
            throw MatterControllerError.commissioningInProgress
        }

        let payload = try MTRSetupPayload(onboardingPayload: payloadString)
        let newNodeID = UInt64.random(in: 2...0x0000_FFFF_FFFF_FFFE)

        return try await withCheckedThrowingContinuation { continuation in
            pendingCommission = (nodeId: newNodeID, completion: { result in
                continuation.resume(with: result)
            })
            do {
                try controller.setupCommissioningSession(
                    with: payload,
                    newNodeID: NSNumber(value: newNodeID)
                )
            } catch {
                pendingCommission = nil
                continuation.resume(throwing: error)
            }
        }
    }

    /// Add an ACL entry on `deviceNodeId` granting `controllerNodeId` Operate privilege.
    /// Call this after commissioning a device to give the HEM controller access.
    func grantControllerAccess(deviceNodeId: UInt64, controllerNodeId: UInt64) async throws {
        guard let controller = deviceController, controller.isRunning else {
            throw MatterControllerError.notInitialized
        }

        let baseDevice = MTRBaseDevice(
            nodeID: NSNumber(value: deviceNodeId),
            controller: controller
        )
        let aclCluster = MTRBaseClusterAccessControl(
            device: baseDevice,
            endpointID: NSNumber(value: 0),
            queue: .main
        )

        return try await withCheckedThrowingContinuation { continuation in
            aclCluster.readAttributeACL { entries, error in
                if let error = error {
                    continuation.resume(throwing: error)
                    return
                }

                var acl = entries as? [MTRAccessControlClusterAccessControlEntryStruct] ?? []

                // Skip if the controller node already has an entry
                let alreadyPresent = acl.contains { entry in
                    (entry.subjects as? [NSNumber])?.contains(NSNumber(value: controllerNodeId)) == true
                }
                guard !alreadyPresent else {
                    continuation.resume()
                    return
                }

                let newEntry = MTRAccessControlClusterAccessControlEntryStruct()
                // Operate (3) lets the HEM send commands and read attributes; CASE auth mode (2)
                newEntry.privilege = NSNumber(value: 3)  // MTRAccessControlEntryPrivilege.operate
                newEntry.authMode = NSNumber(value: 2)   // MTRAccessControlEntryAuthMode.CASE
                newEntry.subjects = [NSNumber(value: controllerNodeId)]
                newEntry.targets = nil
                newEntry.fabricIndex = NSNumber(value: 0)
                acl.append(newEntry)

                aclCluster.writeAttributeACL(withValue: acl) { error in
                    if let error = error {
                        continuation.resume(throwing: MatterControllerError.aclUpdateFailed(error.localizedDescription))
                    } else {
                        continuation.resume()
                    }
                }
            }
        }
    }

    /// Node IDs of all devices commissioned to our fabric (persisted across launches).
    func commissionedNodeIds() -> [UInt64] {
        (UserDefaults.standard.array(forKey: nodeIdsKey) as? [String] ?? [])
            .compactMap { UInt64($0) }
    }

    // MARK: - Private helpers

    private func loadOrCreateIPK() -> Data {
        if let existing = UserDefaults.standard.data(forKey: ipkKey), existing.count == 16 {
            return existing
        }
        var ipk = Data(count: 16)
        _ = ipk.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, 16, $0.baseAddress!) }
        UserDefaults.standard.set(ipk, forKey: ipkKey)
        return ipk
    }

    private func persistNodeId(_ nodeId: UInt64) {
        var ids = commissionedNodeIds().map { String($0) }
        let str = String(nodeId)
        if !ids.contains(str) {
            ids.append(str)
            UserDefaults.standard.set(ids, forKey: nodeIdsKey)
        }
    }

    /// Extract the P-256 public key from a PKCS#10 CSR (DER encoded).
    /// The CSR structure is: SEQUENCE { SEQUENCE { INTEGER, SEQUENCE(subject), SEQUENCE(SPKI), ... }, ... }
    private func publicKeyFromCSR(_ csrData: Data) throws -> SecKey {
        let bytes = [UInt8](csrData)
        var i = 0

        func skipTag(_ tag: UInt8) throws {
            guard i < bytes.count, bytes[i] == tag else {
                throw MatterControllerError.csrParseFailed
            }
            i += 1
        }

        func readLength() -> Int {
            let first = Int(bytes[i]); i += 1
            if first < 0x80 { return first }
            let numBytes = first & 0x7F
            var length = 0
            for _ in 0..<numBytes { length = (length << 8) | Int(bytes[i]); i += 1 }
            return length
        }

        // SEQUENCE (outer — full CSR)
        try skipTag(0x30); _ = readLength()
        // SEQUENCE (CertificationRequestInfo)
        try skipTag(0x30); _ = readLength()
        // INTEGER (version = 0)
        try skipTag(0x02); let vLen = readLength(); i += vLen
        // SEQUENCE (subject — empty for Matter devices: 30 00)
        try skipTag(0x30); let sLen = readLength(); i += sLen
        // SEQUENCE (SubjectPublicKeyInfo)
        try skipTag(0x30); _ = readLength()
        // SEQUENCE (AlgorithmIdentifier)
        try skipTag(0x30); let algLen = readLength(); i += algLen
        // BIT STRING — contains EC point (04 || x || y)
        try skipTag(0x03); let bsLen = readLength()
        i += 1  // skip "unused bits" byte (always 0 for EC keys)

        let keyBytes = Data(bytes[i ..< (i + bsLen - 1)])

        var cfError: Unmanaged<CFError>?
        guard let secKey = SecKeyCreateWithData(
            keyBytes as CFData,
            [kSecAttrKeyType: kSecAttrKeyTypeECSECPrimeRandom,
             kSecAttrKeyClass: kSecAttrKeyClassPublic] as CFDictionary,
            &cfError
        ) else {
            throw cfError!.takeRetainedValue() as Error
        }
        return secKey
    }
}

// MARK: - MTRDeviceControllerDelegate

@available(iOS 16.4, *)
extension MatterController: MTRDeviceControllerDelegate {
    func controller(
        _ controller: MTRDeviceController,
        commissioningSessionEstablishmentDone error: Error?
    ) {
        guard let pending = pendingCommission else { return }

        if let error = error {
            pendingCommission = nil
            pending.completion(.failure(error))
            return
        }

        // PASE established → proceed with operational commissioning
        // Device attestation is handled by shouldSkipAttestationCertificateValidation = true
        // in the MTROperationalCertificateIssuer implementation
        let params = MTRCommissioningParameters()

        do {
            try controller.commissionNode(
                withID: NSNumber(value: pending.nodeId),
                commissioningParams: params
            )
        } catch {
            pendingCommission = nil
            pending.completion(.failure(error))
        }
    }

    func controller(
        _ controller: MTRDeviceController,
        commissioningComplete error: Error?,
        nodeID: NSNumber?
    ) {
        guard let pending = pendingCommission else { return }
        pendingCommission = nil

        if let error = error {
            pending.completion(.failure(error))
        } else {
            let nodeId = nodeID?.uint64Value ?? pending.nodeId
            persistNodeId(nodeId)
            pending.completion(.success(nodeId))
        }
    }
}

// MARK: - MTROperationalCertificateIssuer

@available(iOS 16.4, *)
extension MatterController: MTROperationalCertificateIssuer {
    // Skip DAC/PAI attestation so prototype devices without real certs can commission
    var shouldSkipAttestationCertificateValidation: Bool { true }

    func issueOperationalCertificate(
        for request: MTROperationalCSRInfo,
        attestationInfo: MTRDeviceAttestationInfo,
        completion: @escaping (MTROperationalCertificateChain?, Error?) -> Void
    ) {
        guard let rootKeyPair = rootKeyPair,
              let rootCertData = rootCertData,
              let ipkData = ipkData,
              let pending = pendingCommission else {
            completion(nil, MatterControllerError.notInitialized)
            return
        }

        do {
            // Parse the CSR to extract the device's P-256 public key
            let devicePublicKey = try publicKeyFromCSR(request.csr)

            // Sign a Node Operational Certificate for this device using our root CA
            guard let noc = MTRCertificates.generateOperationalCertificate(
                withSigningKey: rootKeyPair,
                signingCertificate: rootCertData,
                operationalPublicKey: devicePublicKey,
                fabricID: NSNumber(value: Self.fabricID),
                nodeID: NSNumber(value: pending.nodeId),
                caseAuthenticatedTags: nil,
                error: nil
            ) else {
                completion(nil, MatterControllerError.nocGenerationFailed)
                return
            }

            let chain = MTROperationalCertificateChain(
                operationalCertificate: noc,
                intermediateCertificate: nil,
                rootCertificate: rootCertData,
                ipk: ipkData
            )
            completion(chain, nil)
        } catch {
            completion(nil, error)
        }
    }
}
