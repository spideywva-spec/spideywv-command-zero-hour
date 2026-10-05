import Foundation
import NetworkExtension

final class PacketTunnelProvider: NEPacketTunnelProvider {
    private var socket: URLSessionWebSocketTask?
    private var session: URLSession?
    private var stopped = false

    override func startTunnel(options: [String : NSObject]?, completionHandler: @escaping (Error?) -> Void) {
        stopped = false
        guard let config = (protocolConfiguration as? NETunnelProviderProtocol)?.providerConfiguration,
              let relay = config["relayURL"] as? String,
              let url = URL(string: relay),
              let lobbyID = config["lobbyID"] as? String,
              let token = config["playerToken"] as? String,
              let virtualIP = config["virtualIP"] as? String else {
            completionHandler(PacketTunnelError.invalidConfiguration)
            return
        }

        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: "10.42.0.1")
        let ipv4 = NEIPv4Settings(addresses: [virtualIP], subnetMasks: ["255.255.255.0"])
        ipv4.includedRoutes = [NEIPv4Route(destinationAddress: "10.42.0.0", subnetMask: "255.255.255.0")]
        settings.ipv4Settings = ipv4
        settings.mtu = 1400

        setTunnelNetworkSettings(settings) { [weak self] error in
            guard let self else { completionHandler(PacketTunnelError.providerGone); return }
            if let error { completionHandler(error); return }

            let session = URLSession(configuration: .ephemeral)
            self.session = session
            let socket = session.webSocketTask(with: url)
            self.socket = socket
            socket.resume()

            let join: [String: Any] = [
                "type": "join",
                "lobbyId": lobbyID,
                "playerToken": token,
                "virtualIP": virtualIP,
                "protocol": 1
            ]
            guard let data = try? JSONSerialization.data(withJSONObject: join),
                  let text = String(data: data, encoding: .utf8) else {
                completionHandler(PacketTunnelError.invalidConfiguration)
                return
            }

            socket.send(.string(text)) { sendError in
                if let sendError { completionHandler(sendError); return }
                self.receiveLoop()
                self.readLoop()
                completionHandler(nil)
            }
        }
    }

    private func readLoop() {
        guard !stopped else { return }
        packetFlow.readPackets { [weak self] packets, _ in
            guard let self, !self.stopped else { return }
            for packet in packets where !packet.isEmpty {
                self.socket?.send(.data(packet)) { _ in }
            }
            self.readLoop()
        }
    }

    private func receiveLoop() {
        guard !stopped, let socket else { return }
        socket.receive { [weak self] result in
            guard let self, !self.stopped else { return }
            switch result {
            case .success(.data(let packet)):
                self.packetFlow.writePackets([packet], withProtocols: [NSNumber(value: AF_INET)])
                self.receiveLoop()
            case .success(.string):
                self.receiveLoop()
            case .failure(let error):
                self.cancelTunnelWithError(error)
            @unknown default:
                self.cancelTunnelWithError(PacketTunnelError.connectionClosed)
            }
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        stopped = true
        socket?.cancel(with: .goingAway, reason: nil)
        socket = nil
        session?.invalidateAndCancel()
        session = nil
        completionHandler()
    }
}

private enum PacketTunnelError: LocalizedError {
    case invalidConfiguration
    case providerGone
    case connectionClosed

    var errorDescription: String? {
        switch self {
        case .invalidConfiguration: return "Invalid Generals virtual LAN configuration."
        case .providerGone: return "Generals virtual LAN provider stopped."
        case .connectionClosed: return "Generals virtual LAN relay connection closed."
        }
    }
}
