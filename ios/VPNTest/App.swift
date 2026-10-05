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
    private let apiURL = "https://spideywv-command-zero-hour.onrender.com"
    private let lobbyField = UITextField()
    private let nameField = UITextField()
    private let logView = UITextView()
    private let statusLabel = UILabel()
    private var vpnManager: NETunnelProviderManager?
    private var virtualIP = ""

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemBackground

        let title = UILabel()
        title.text = "VPN Render"
        title.font = .boldSystemFont(ofSize: 30)

        statusLabel.text = "VPN: DISCONNECTED"
        statusLabel.font = .boldSystemFont(ofSize: 17)

        nameField.placeholder = "Player name"
        nameField.text = "iPhone-" + String(Int.random(in: 1000...9999))
        lobbyField.placeholder = "Lobby ID — leave empty to CREATE"

        for field in [nameField, lobbyField] {
            field.borderStyle = .roundedRect
            field.autocapitalizationType = .none
            field.autocorrectionType = .no
        }

        let vpn = UIButton(type: .system)
        vpn.setTitle("VPN — CONNECT TO RENDER", for: .normal)
        vpn.titleLabel?.font = .boldSystemFont(ofSize: 18)
        vpn.addTarget(self, action: #selector(connectVPN), for: .touchUpInside)

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
            title, statusLabel, nameField, lobbyField, vpn, disconnect, send, logView
        ])
        stack.axis = .vertical
        stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)

        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor, constant: 20),
            logView.heightAnchor.constraint(greaterThanOrEqualToConstant: 260)
        ])

        loadManager()
        log("Render: \(apiURL)")
        log("Нажми VPN. Пустой Lobby ID = создать сеть.")
    }

    private func loadManager() {
        NETunnelProviderManager.loadAllFromPreferences { [weak self] managers, error in
            DispatchQueue.main.async {
                if let error { self?.log("VPN manager: \(error.localizedDescription)") }
                self?.vpnManager = managers?.first
                self?.refreshStatus()
            }
        }
    }

    @objc private func connectVPN() {
        let lobby = lobbyField.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if lobby.isEmpty {
            createLobby()
        } else {
            joinLobby(id: lobby)
        }
    }

    private func createLobby() {
        log("Создаю сеть на Render…")
        request(path: "/v1/lobbies", body: [
            "playerName": nameField.text ?? "iPhone",
            "maxPlayers": 2
        ]) { [weak self] result in
            DispatchQueue.main.async {
                switch result {
                case .failure(let error):
                    self?.log("CREATE ERROR: \(error.localizedDescription)")
                case .success(let json):
                    if let lobby = json["lobby"] as? [String: Any],
                       let id = lobby["id"] as? String {
                        self?.lobbyField.text = id
                        self?.log("Lobby ID: \(id)")
                    }
                    self?.startFromResponse(json)
                }
            }
        }
    }

    private func joinLobby(id: String) {
        log("Подключаюсь к Lobby \(id)…")
        request(path: "/v1/lobbies/\(id)/join", body: [
            "playerName": nameField.text ?? "iPhone"
        ]) { [weak self] result in
            DispatchQueue.main.async {
                switch result {
                case .failure(let error):
                    self?.log("JOIN ERROR: \(error.localizedDescription)")
                case .success(let json):
                    self?.startFromResponse(json)
                }
            }
        }
    }

    private func request(path: String, body: [String: Any],
                         completion: @escaping (Result<[String: Any], Error>) -> Void) {
        guard let url = URL(string: apiURL + path) else {
            completion(.failure(TestError.message("Bad Render URL")))
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try? JSONSerialization.data(withJSONObject: body)

        URLSession.shared.dataTask(with: request) { data, response, error in
            if let error {
                completion(.failure(error))
                return
            }
            guard let data,
                  let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                completion(.failure(TestError.message("Invalid Render response")))
                return
            }
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            guard (200..<300).contains(code) else {
                completion(.failure(TestError.message((json["error"] as? String) ?? "HTTP \(code)")))
                return
            }
            completion(.success(json))
        }.resume()
    }

    private func startFromResponse(_ json: [String: Any]) {
        guard let vpn = json["vpn"] as? [String: Any],
              let relay = vpn["relayURL"] as? String,
              let lobby = vpn["lobbyId"] as? String,
              let token = vpn["playerToken"] as? String,
              let ip = vpn["virtualIP"] as? String else {
            log("Render не вернул VPN config.")
            return
        }

        virtualIP = ip
        log("Virtual IP: \(ip)")
        log("Relay: \(relay)")
        configureAndStart(relay: relay, lobby: lobby, token: token, ip: ip)
    }

    private func configureAndStart(relay: String, lobby: String, token: String, ip: String) {
        let provider = NETunnelProviderProtocol()
        provider.providerBundleIdentifier = "me.spideywv.vpnrendertest.VPNRenderExtension"
        provider.serverAddress = "Render VPN"
        provider.providerConfiguration = [
            "relayURL": relay,
            "lobbyID": lobby,
            "playerToken": token,
            "virtualIP": ip
        ]

        let manager = vpnManager ?? NETunnelProviderManager()
        manager.protocolConfiguration = provider
        manager.localizedDescription = "SpideyWV Render VPN"
        manager.isEnabled = true

        manager.saveToPreferences { [weak self] error in
            if let error {
                DispatchQueue.main.async { self?.log("VPN SAVE ERROR: \(error.localizedDescription)") }
                return
            }
            manager.loadFromPreferences { error in
                if let error {
                    DispatchQueue.main.async { self?.log("VPN RELOAD ERROR: \(error.localizedDescription)") }
                    return
                }
                do {
                    try manager.connection.startVPNTunnel()
                    DispatchQueue.main.async {
                        self?.vpnManager = manager
                        self?.log("VPN STARTED. iOS покажет разрешение VPN.")
                        self?.refreshStatus()
                    }
                } catch {
                    DispatchQueue.main.async { self?.log("VPN START ERROR: \(error.localizedDescription)") }
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
        guard !virtualIP.isEmpty else {
            log("Сначала подключи VPN.")
            return
        }

        let connection = NWConnection(
            host: NWEndpoint.Host(virtualIP),
            port: 9999,
            using: .udp
        )
        let payload = Data("VPN-RENDER-TEST \(Date())".utf8)

        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                connection.send(content: payload, completion: .contentProcessed { error in
                    DispatchQueue.main.async {
                        self?.log(error == nil ? "UDP → \(self?.virtualIP ?? ""):9999" :
                                  "UDP ERROR: \(error!.localizedDescription)")
                    }
                    connection.cancel()
                })
            case .failed(let error):
                DispatchQueue.main.async { self?.log("UDP ERROR: \(error.localizedDescription)") }
            default:
                break
            }
        }
        connection.start(queue: .global(qos: .utility))
    }

    private func refreshStatus() {
        let status = vpnManager?.connection.status
        switch status {
        case .connected: statusLabel.text = "VPN: CONNECTED"
        case .connecting: statusLabel.text = "VPN: CONNECTING"
        case .disconnecting: statusLabel.text = "VPN: DISCONNECTING"
        case .reasserting: statusLabel.text = "VPN: REASSERTING"
        case .invalid: statusLabel.text = "VPN: INVALID"
        default: statusLabel.text = "VPN: DISCONNECTED"
        }
    }

    private func log(_ message: String) {
        logView.text += (logView.text.isEmpty ? "" : "\n") + message
        logView.scrollRangeToVisible(NSRange(location: logView.text.count, length: 0))
    }
}

private enum TestError: LocalizedError {
    case message(String)
    var errorDescription: String? {
        if case .message(let value) = self { return value }
        return nil
    }
}
