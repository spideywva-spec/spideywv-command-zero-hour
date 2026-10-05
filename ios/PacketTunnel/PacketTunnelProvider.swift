import Foundation
import NetworkExtension
import Network

final class PacketTunnelProvider: NEPacketTunnelProvider {
    private var peerConnection: NWConnection?
    private var readLoopActive = false
    private var receiveLoopActive = false
    private var localPort: UInt16 = 0
    private var apiBase = ""
    private var lobbyID = ""
    private var playerToken = ""
    private var role = "client"
    private var virtualIP = "10.42.0.3"
    private var peerIP = "10.42.0.2"

    private let magic: [UInt8] = [0x47, 0x58, 0x50, 0x32]
    private let stunServers = ["stun.l.google.com", "stun1.l.google.com"]

    override func startTunnel(options: [String : NSObject]?, completionHandler: @escaping (Error?) -> Void) {
        guard let proto = protocolConfiguration as? NETunnelProviderProtocol,
              let cfg = proto.providerConfiguration else {
            completionHandler(NSError(domain: "GeneralsXZH.P2P", code: 1,
                                      userInfo: [NSLocalizedDescriptionKey: "Missing tunnel configuration"]))
            return
        }

        apiBase = (cfg["apiBase"] as? String ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        lobbyID = cfg["lobbyID"] as? String ?? ""
        playerToken = cfg["playerToken"] as? String ?? ""
        role = cfg["role"] as? String ?? "client"
        virtualIP = role == "host" ? "10.42.0.2" : "10.42.0.3"
        peerIP = role == "host" ? "10.42.0.3" : "10.42.0.2"
        localPort = UInt16(cfg["localPort"] as? Int ?? 0)

        guard !apiBase.isEmpty, !lobbyID.isEmpty, !playerToken.isEmpty else {
            completionHandler(NSError(domain: "GeneralsXZH.P2P", code: 2,
                                      userInfo: [NSLocalizedDescriptionKey: "Missing lobby credentials"]))
            return
        }

        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: "10.42.0.1")
        let ipv4 = NEIPv4Settings(addresses: [virtualIP], subnetMasks: ["255.255.255.0"])
        ipv4.includedRoutes = [NEIPv4Route(destinationAddress: "10.42.0.0", subnetMask: "255.255.255.0")]
        settings.ipv4Settings = ipv4
        settings.mtu = 1400

        setTunnelNetworkSettings(settings) { [weak self] error in
            guard let self else { completionHandler(error); return }
            if let error { completionHandler(error); return }
            self.establishTransport { error in
                completionHandler(error)
            }
        }
    }

    override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
        readLoopActive = false
        receiveLoopActive = false
        peerConnection?.cancel()
        peerConnection = nil
        completionHandler()
    }

    private func establishTransport(completion: @escaping (Error?) -> Void) {
        let port = localPort == 0 ? UInt16.random(in: 40000...52000) : localPort
        localPort = port
        discoverMappedCandidate(localPort: port) { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let error):
                completion(error)
            case .success(let candidate):
                self.postCandidate(candidate) {
                    self.waitForPeerCandidate { result in
                        switch result {
                        case .failure(let error): completion(error)
                        case .success(let peer): self.connectToPeer(peer, localPort: port, completion: completion)
                        }
                    }
                }
            }
        }
    }

    private func discoverMappedCandidate(localPort: UInt16, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        let params = NWParameters.udp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.any), port: NWEndpoint.Port(rawValue: localPort)!)
        let conn = NWConnection(host: NWEndpoint.Host(stunServers[0]), port: 19302, using: params)
        let transaction = (0..<12).map { _ in UInt8.random(in: 0...255) }
        var packet = Data([0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42])
        packet.append(contentsOf: transaction)

        conn.stateUpdateHandler = { state in
            if case .ready = state {
                conn.send(content: packet, completion: .contentProcessed { error in
                    if let error { conn.cancel(); completion(.failure(error)); return }
                    conn.receiveMessage { data, _, _, error in
                        defer { conn.cancel() }
                        if let error { completion(.failure(error)); return }
                        guard let data else {
                            completion(.failure(NSError(domain: "GeneralsXZH.P2P", code: 10,
                                                        userInfo: [NSLocalizedDescriptionKey: "Empty STUN response"])))
                            return
                        }
                        if let address = Self.parseStunAddress(data, transaction: transaction) {
                            completion(.success([
                                "type": "srflx", "address": address.host,
                                "port": address.port, "protocol": "udp", "priority": 200
                            ]))
                        } else {
                            completion(.failure(NSError(domain: "GeneralsXZH.P2P", code: 11,
                                                        userInfo: [NSLocalizedDescriptionKey: "No XOR-MAPPED-ADDRESS in STUN response"])))
                        }
                    }
                })
            } else if case .failed(let error) = state {
                conn.cancel(); completion(.failure(error))
            }
        }
        conn.start(queue: .global(qos: .userInitiated))
    }

    private static func parseStunAddress(_ data: Data, transaction: [UInt8]) -> (host: String, port: Int)? {
        guard data.count >= 20 else { return nil }
        let bytes = [UInt8](data)
        var offset = 20
        while offset + 4 <= bytes.count {
            let type = UInt16(bytes[offset]) << 8 | UInt16(bytes[offset + 1])
            let len = Int(UInt16(bytes[offset + 2]) << 8 | UInt16(bytes[offset + 3]))
            offset += 4
            guard offset + len <= bytes.count else { break }
            if type == 0x0020 && len >= 8 {
                let family = bytes[offset + 1]
                guard family == 0x01 else { return nil }
                let xPort = UInt16(bytes[offset + 2]) << 8 | UInt16(bytes[offset + 3])
                let cookie: UInt16 = 0x2112
                let port = Int(xPort ^ cookie)
                let ip = (0..<4).map { i in
                    let x = bytes[offset + 4 + i] ^ [0x21, 0x12, 0xA4, 0x42][i]
                    return String(x)
                }.joined(separator: ".")
                return (ip, port)
            }
            offset += (len + 3) & ~3
        }
        return nil
    }

    private func postCandidate(_ candidate: [String: Any], completion: @escaping () -> Void) {
        guard let url = URL(string: "\(apiBase)/v1/lobbies/\(lobbyID)/candidates") else { completion(); return }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: [
            "playerToken": playerToken, "candidates": [candidate]
        ])
        URLSession.shared.dataTask(with: req) { _, _, _ in completion() }.resume()
    }

    private func waitForPeerCandidate(completion: @escaping (Result<(String, UInt16), Error>) -> Void) {
        func poll(_ attempt: Int) {
            guard attempt < 80 else {
                completion(.failure(NSError(domain: "GeneralsXZH.P2P", code: 20,
                                             userInfo: [NSLocalizedDescriptionKey: "Peer candidate timeout"])))
                return
            }
            let urlString = "\(apiBase)/v1/lobbies/\(lobbyID)/candidates?playerToken=\(playerToken)"
            guard let url = URL(string: urlString) else {
                completion(.failure(NSError(domain: "GeneralsXZH.P2P", code: 21,
                                             userInfo: [NSLocalizedDescriptionKey: "Bad signaling URL"])))
                return
            }
            URLSession.shared.dataTask(with: url) { [weak self] data, _, _ in
                guard self != nil else { return }
                if let data,
                   let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                   let peers = obj["peers"] as? [[String: Any]] {
                    for peer in peers {
                        if let candidates = peer["candidates"] as? [[String: Any]] {
                            for c in candidates {
                                if let host = c["address"] as? String,
                                   let port = c["port"] as? Int,
                                   !host.isEmpty, port > 0 {
                                    completion(.success((host, UInt16(port))))
                                    return
                                }
                            }
                        }
                    }
                }
                DispatchQueue.global().asyncAfter(deadline: .now() + 0.25) { poll(attempt + 1) }
            }.resume()
        }
        poll(0)
    }

    private func connectToPeer(_ peer: (String, UInt16), localPort: UInt16, completion: @escaping (Error?) -> Void) {
        let params = NWParameters.udp
        params.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback), port: NWEndpoint.Port(rawValue: localPort)!)
        let connection = NWConnection(host: NWEndpoint.Host(peer.0), port: NWEndpoint.Port(rawValue: peer.1)!, using: params)
        peerConnection = connection
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.readLoopActive = true
                self.receiveLoopActive = true
                self.receiveLoop()
                self.readPackets()
                self.sendKeepAlive()
                completion(nil)
            case .failed(let error):
                completion(error)
            default:
                break
            }
        }
        connection.start(queue: .global(qos: .userInitiated))
    }

    private func sendKeepAlive() {
        guard readLoopActive, let connection = peerConnection else { return }
        connection.send(content: Data(magic + [0x01]), completion: .contentProcessed { [weak self] _ in
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.25) { self?.sendKeepAlive() }
        })
    }

    private func readPackets() {
        guard readLoopActive else { return }
        packetFlow.readPackets { [weak self] packets, _ in
            guard let self else { return }
            if let connection = self.peerConnection {
                for packet in packets {
                    var frame = Data(self.magic + [0x02])
                    frame.append(packet)
                    connection.send(content: frame, completion: .contentProcessed { _ in })
                }
            }
            self.readPackets()
        }
    }

    private func receiveLoop() {
        guard receiveLoopActive, let connection = peerConnection else { return }
        connection.receiveMessage { [weak self] data, _, _, _ in
            guard let self else { return }
            if let data, data.count > 5 {
                let bytes = [UInt8](data)
                guard Array(bytes.prefix(4)) == self.magic else {
                    self.receiveLoop(); return
                }
                if bytes[4] == 0x02 {
                    let packet = Data(bytes.dropFirst(5))
                    self.packetFlow.writePackets([packet], withProtocols: [NSNumber(value: AF_INET)])
                }
            }
            self.receiveLoop()
        }
    }
}
