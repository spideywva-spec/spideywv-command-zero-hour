import UIKit
import NetworkExtension
import Network

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = VPNTestViewController()
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}

final class VPNTestViewController: UIViewController {
    private let apiField = UITextField()
    private let lobbyField = UITextField()
    private let nameField = UITextField()
    private let logView = UITextView()
    private let statusLabel = UILabel()
    private var playerToken = ""
    private var virtualIP = ""
    private var vpnManager: NETunnelProviderManager?

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground

        let title = UILabel()
        title.text = "VPN Render Test"
        title.font = .boldSystemFont(ofSize: 28)

        statusLabel.text = "VPN: disconnected"
        statusLabel.font = .boldSystemFont(ofSize: 17)

        apiField.placeholder = "https://your-render-service.onrender.com"
        apiField.text = UserDefaults.standard.string(forKey: "api") ?? ""
        lobbyField.placeholder = "Lobby ID (for Join)"
        nameField.placeholder = "Player name"
        nameField.text = "iPhone-" + String(Int.random(in: 1000...9999))

        for field in [apiField, lobbyField, nameField] {
            field.borderStyle = .roundedRect
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
        }

        let create = UIButton(type: .system)
        create.setTitle("CREATE LOBBY + CONNECT", for: .normal)
        create.addTarget(self, action: #selector(createLobby), for: .touchUpInside)

        let join = UIButton(type: .system)
        join.setTitle("JOIN LOBBY + CONNECT", for: .normal)
        join.addTarget(self, action: #selector(joinLobby), for: .touchUpInside)

        let disconnect = UIButton(type: .system)
        disconnect.setTitle("DISCONNECT VPN", for: .normal)
        disconnect.addTarget(self, action: #selector(disconnectVPN), for: .touchUpInside)

        let send = UIButton(type: .system)
        send.setTitle("SEND UDP TEST", for: .normal)
        send.addTarget(self, action: #selector(sendUDP), for: .touchUpInside)

        logView.isEditable = false
        logView.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
        logView.layer.borderWidth = 1
        logView.layer.cornerRadius = 10

        let stack = UIStackView(arrangedSubviews: [
            title, statusLabel, apiField, nameField, lobbyField,
            create, join, disconnect, send, logView
        ])
        stack.axis = .vertical
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 20),
            logView.heightAnchor.constraint(greaterThanOrEqualToConstant: 240)
        ])
        loadManager()
        log("Ready. Enter Render API URL.")
    }

    private func baseURL() -> String {
        var s = apiField.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        while s.hasSuffix("/") { s.removeLast() }
        return s
    }

    private func loadManager() {
        NETunnelProviderManager.loadAllFromPreferences { [weak self] managers, error in
            DispatchQueue.main.async {
                if let error { self?.log("VPN manager load: (error.localizedDescription)") }
                self?.vpnManager = managers?.first
                self?.refreshStatus()
            }
        }
    }

    private func apiRequest(path: String, body: [String: Any], completion: @escaping (Result<[String: Any], Error>) -> Void) {
        let base = baseURL()
        guard let url = URL(string: base + path) else {
            completion(.failure(TestError.message("Bad Render API URL")))
            return
        }
        UserDefaults.standard.set(base, forKey: "api")
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: body)
        URLSession.shared.dataTask(with: req) { data, response, error in
            if let error { completion(.failure(error)); return }
            guard let data,
                  let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                completion(.failure(TestError.message("Invalid API response")))
                return
            }
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            guard (200..<300).contains(code) else {
                completion(.failure(TestError.message((obj["error"] as? String) ?? "HTTP (code)")))
                return
            }
            completion(.success(obj))
        }.resume()
    }

    @objc private func createLobby() {
        log("Creating test lobby…")
        apiRequest(path: "/v1/lobbies", body: [
            "playerName": nameField.text ?? "iPhone",
            "maxPlayers": 2
        ]) { [weak self] result in
            DispatchQueue.main.async {
                switch result {
                case .failure(let e): self?.log("CREATE ERROR: (e.localizedDescription)")
                case .success(let json):
                    if let lobby = json["lobby"] as? [String: Any] {
                        self?.lobbyField.text = lobby["id"] as? String
                        self?.log("Lobby: (lobby["id"] as? String ?? "?")")
                    }
                    self?.connectFromResponse(json)
                }
            }
        }
    }

    @objc private func joinLobby() {
        guard let id = lobbyField.text, !id.isEmpty else {
            log("Enter Lobby ID first.")
            return
        }
        log("Joining lobby (id)…")
        apiRequest(path: "/v1/lobbies/(id)/join", body: [
            "playerName": nameField.text ?? "iPhone"
        ]) { [weak self] result in
            DispatchQueue.main.async {
                switch result {
                case .failure(let e): self?.log("JOIN ERROR: (e.localizedDescription)")
                case .success(let json): self?.connectFromResponse(json)
                }
            }
        }
    }

    private func connectFromResponse(_ json: [String: Any]) {
        guard let vpn = json["vpn"] as? [String: Any],
              let relay = vpn["relayURL"] as? String,
              let lobby = vpn["lobbyId"] as? String,
              let token = vpn["playerToken"] as? String,
              let ip = vpn["virtualIP"] as? String else {
            log("API did not return VPN configuration.")
            return
        }
        playerToken = token
        virtualIP = ip
        log("VPN config: (ip), relay=(relay)")
        configureAndStart(relay: relay, lobby: lobby, token: token, ip: ip)
    }

    private func configureAndStart(relay: String, lobby: String, token: String, ip: String) {
        let provider = NETunnelProviderProtocol()
        provider.providerBundleIdentifier = Bundle.main.bundleIdentifier! + ".VPNRenderExtension"
        provider.serverAddress = "Render VPN Test"
        provider.providerConfiguration = [
            "relayURL": relay,
            "lobbyID": lobby,
            "playerToken": token,
            "virtualIP": ip
        ]

        let manager = vpnManager ?? NETunnelProviderManager()
        manager.protocolConfiguration = provider
        manager.localizedDescription = "VPN Render Test"
        manager.isEnabled = true
        manager.saveToPreferences { [weak self] error in
            if let error {
                DispatchQueue.main.async { self?.log("VPN SAVE ERROR: (error.localizedDescription)") }
                return
            }
            manager.loadFromPreferences { error in
                if let error {
                    DispatchQueue.main.async { self?.log("VPN RELOAD ERROR: (error.localizedDescription)") }
                    return
                }
                do {
                    try manager.connection.startVPNTunnel()
                    DispatchQueue.main.async {
                        self?.vpnManager = manager
                        self?.log("VPN start requested. iOS may ask for VPN permission.")
                        self?.refreshStatus()
                    }
                } catch {
                    DispatchQueue.main.async { self?.log("VPN START ERROR: (error.localizedDescription)") }
                }
            }
        }
    }

    @objc private func disconnectVPN() {
        vpnManager?.connection.stopVPNTunnel()
        log("VPN disconnect requested.")
        refreshStatus()
    }

    @objc private func sendUDP() {
        guard !virtualIP.isEmpty else { log("No virtual IP. Connect first."); return }
        let host = NWEndpoint.Host(virtualIP)
        let port = NWEndpoint.Port(integerLiteral: 9999)
        let connection = NWConnection(host: host, port: port, using: .udp)
        let payload = Data("VPN-RENDER-TEST (Date())".utf8)
        connection.stateUpdateHandler = { [weak self] state in
            if case .ready = state {
                connection.send(content: payload, completion: .contentProcessed { error in
                    DispatchQueue.main.async { self?.log(error == nil ? "UDP test packet sent to (self?.virtualIP ?? "")" : "UDP SEND ERROR: (error!.localizedDescription)") }
                    connection.cancel()
                })
            } else if case .failed(let error) = state {
                DispatchQueue.main.async { self?.log("UDP ERROR: (error.localizedDescription)") }
            }
        }
        connection.start(queue: .global(qos: .utility))
    }

    private func refreshStatus() {
        let status = vpnManager?.connection.status
        let text: String
        switch status {
        case .connected: text = "VPN: CONNECTED"
        case .connecting: text = "VPN: CONNECTING"
        case .disconnecting: text = "VPN: DISCONNECTING"
        case .reasserting: text = "VPN: REASSERTING"
        case .invalid: text = "VPN: INVALID"
        default: text = "VPN: DISCONNECTED"
        }
        statusLabel.text = text
    }

    private func log(_ message: String) {
        logView.text += (logView.text.isEmpty ? "" : "\n") + message
        logView.scrollRangeToVisible(NSRange(location: logView.text.count, length: 0))
    }
}

private enum TestError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        if case .message(let s) = self { return s }
    }
}
