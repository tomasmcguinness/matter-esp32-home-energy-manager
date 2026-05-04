import Foundation

enum ESP32ClientError: LocalizedError {
    case invalidResponse
    case signingFailed(String)
    case commissionFailed(Int)

    var errorDescription: String? {
        switch self {
        case .invalidResponse:          return "Invalid response from hub"
        case .signingFailed(let m):     return "NOC signing failed: \(m)"
        case .commissionFailed(let c):  return "Commissioning failed (HTTP \(c))"
        }
    }
}

class ESP32Client {
    let baseURL: URL

    init(baseURL: URL) {
        self.baseURL = baseURL
    }

    func commissionDevice(onboardingPayload: String) async throws {
        var request = URLRequest(url: baseURL.appendingPathComponent("controller/commission"))
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.timeoutInterval = 90  // commissioning can take a while
        request.httpBody = try JSONSerialization.data(withJSONObject: [
            "onboardingPayload": onboardingPayload
        ])

        let (_, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            throw ESP32ClientError.commissionFailed(code)
        }
    }
}
