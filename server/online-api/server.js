#!/usr/bin/env node
"use strict";

const http = require("http");
const crypto = require("crypto");
const { WebSocketServer, WebSocket } = require("ws");

const PORT = Number(process.env.PORT || 8080);
const HOST = process.env.HOST || "0.0.0.0";
const ORIGIN = process.env.PUBLIC_ORIGIN || "*";
const LOBBY_TTL = Number(process.env.LOBBY_TTL_MS || 3600000);
const PEER_TIMEOUT = Number(process.env.PLAYER_TIMEOUT_MS || 30000);

const lobbies = new Map();
const peers = new Map();

const id = () => crypto.randomBytes(8).toString("hex");
const lobbyId = () => String(crypto.randomBytes(4).readUIntBE(0, 4));
const ipFor = n => "10.42.0." + (n + 2);
const clean = v => String(v ?? "").trim().slice(0, 128);

function send(res, code, body) {
  res.writeHead(code, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": ORIGIN,
    "Access-Control-Allow-Headers": "Content-Type",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS"
  });
  res.end(JSON.stringify(body));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let data = "";
    req.on("data", chunk => {
      data += chunk;
      if (data.length > 65536) reject(new Error("body_too_large"));
    });
    req.on("end", () => {
      try { resolve(data ? JSON.parse(data) : {}); }
      catch (_) { reject(new Error("invalid_json")); }
    });
    req.on("error", reject);
  });
}

function publicLobby(l) {
  return {
    id: l.id,
    name: l.name,
    maxPlayers: l.maxPlayers,
    players: l.players.map(p => ({
      name: p.name,
      virtualIP: p.virtualIP,
      connected: peers.get(l.id)?.has(p.token) || false
    }))
  };
}

function vpn(l, player) {
  const host = process.env.RENDER_EXTERNAL_HOSTNAME;
  const relayURL = process.env.VPN_PUBLIC_WS_URL ||
    (host ? "wss://" + host + "/v1/vpn" : "");
  return {
    relayURL,
    lobbyId: l.id,
    playerToken: player.token,
    virtualIP: player.virtualIP
  };
}

function sweep() {
  const now = Date.now();
  for (const [id, l] of lobbies) {
    if (now - l.createdAt > LOBBY_TTL) {
      for (const p of l.players) {
        const ws = peers.get(id)?.get(p.token);
        try { ws?.close(); } catch (_) {}
      }
      peers.delete(id);
      lobbies.delete(id);
    }
  }
}
setInterval(sweep, 15000).unref();

const server = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "Access-Control-Allow-Origin": ORIGIN,
      "Access-Control-Allow-Headers": "Content-Type",
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS"
    });
    return res.end();
  }

  sweep();
  const url = new URL(req.url, "http://localhost");

  try {
    if (req.method === "GET" && url.pathname === "/") {
      return send(res, 200, {
        ok: true,
        service: "VPN Render Test",
        websocket: "/v1/vpn",
        lobbies: lobbies.size
      });
    }

    if (req.method === "GET" && url.pathname === "/v1/health") {
      return send(res, 200, {
        ok: true,
        service: "VPN Render Test",
        lobbies: lobbies.size,
        websocketPeers: [...peers.values()].reduce((n, m) => n + m.size, 0)
      });
    }

    if (req.method === "GET" && url.pathname === "/v1/lobbies") {
      return send(res, 200, { lobbies: [...lobbies.values()].map(publicLobby) });
    }

    if (req.method === "POST" && url.pathname === "/v1/lobbies") {
      const b = await readBody(req);
      const l = {
        id: lobbyId(),
        name: clean(b.name) || "VPN Test Lobby",
        maxPlayers: 2,
        createdAt: Date.now(),
        players: []
      };
      const p = {
        token: id(),
        name: clean(b.playerName) || "iPhone",
        virtualIP: ipFor(0)
      };
      l.players.push(p);
      lobbies.set(l.id, l);
      return send(res, 201, { lobby: publicLobby(l), playerToken: p.token, vpn: vpn(l, p) });
    }

    const match = url.pathname.match(/^\/v1\/lobbies\/([^/]+)\/join$/);
    if (req.method === "POST" && match) {
      const l = lobbies.get(match[1]);
      if (!l) return send(res, 404, { error: "lobby_not_found" });
      if (l.players.length >= l.maxPlayers) return send(res, 409, { error: "lobby_full" });

      const b = await readBody(req);
      const p = {
        token: id(),
        name: clean(b.playerName) || "iPhone",
        virtualIP: ipFor(l.players.length)
      };
      l.players.push(p);
      return send(res, 200, { lobby: publicLobby(l), playerToken: p.token, vpn: vpn(l, p) });
    }

    return send(res, 404, { error: "not_found" });
  } catch (e) {
    return send(res, 500, { error: e.message || "server_error" });
  }
});

const wss = new WebSocketServer({ noServer: true, maxPayload: 128 * 1024 });

wss.on("connection", ws => {
  let lobby = null;
  let token = null;

  ws.on("message", (data, binary) => {
    if (!lobby) {
      if (binary) return ws.close(1008, "join required");
      let msg;
      try { msg = JSON.parse(data.toString()); } catch (_) {
        return ws.close(1008, "invalid join");
      }

      const l = lobbies.get(clean(msg.lobbyId));
      const p = l?.players.find(x => x.token === clean(msg.playerToken));
      if (!l || !p || msg.virtualIP !== p.virtualIP) {
        return ws.close(1008, "invalid lobby");
      }

      lobby = l;
      token = p.token;
      if (!peers.has(l.id)) peers.set(l.id, new Map());
      peers.get(l.id).set(token, ws);

      ws.send(JSON.stringify({
        type: "ready",
        lobbyId: l.id,
        virtualIP: p.virtualIP,
        peerCount: peers.get(l.id).size
      }));
      return;
    }

    if (!binary) return;
    const group = peers.get(lobby.id);
    if (!group) return;

    for (const [otherToken, peer] of group) {
      if (otherToken !== token && peer.readyState === WebSocket.OPEN) {
        try { peer.send(data, { binary: true }); } catch (_) {}
      }
    }
  });

  ws.on("close", () => {
    if (!lobby) return;
    const group = peers.get(lobby.id);
    if (group?.get(token) === ws) group.delete(token);
    if (group && group.size === 0) peers.delete(lobby.id);
  });
});

server.on("upgrade", (req, socket, head) => {
  const url = new URL(req.url, "http://localhost");
  if (url.pathname !== "/v1/vpn") return socket.destroy();
  wss.handleUpgrade(req, socket, head, ws => wss.emit("connection", ws, req));
});

server.listen(PORT, HOST, () =>
  console.log("VPN Render Test listening on " + HOST + ":" + PORT)
);
