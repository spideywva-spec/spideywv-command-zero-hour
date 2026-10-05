#!/usr/bin/env node
"use strict";

const http = require("http");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const PORT = Number(process.env.PORT || 8080);
const HOST = process.env.HOST || "0.0.0.0";
const ORIGIN = process.env.PUBLIC_ORIGIN || "*";
const TTL = Number(process.env.LOBBY_TTL_MS || 7200000);
const TIMEOUT = Number(process.env.PLAYER_TIMEOUT_MS || 15000);
const MAX = 65536;
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, "data");
const DATA_FILE = path.join(DATA_DIR, "state.json");

const lobbies = new Map();
const users = new Map();
const friends = new Map();
const requests = new Map();

const str = (v, n) => String(v ?? "").trim().slice(0, n);
const num = (v, a, b, d) => {
  const n = Number(v);
  return Number.isFinite(n) ? Math.min(b, Math.max(a, n)) : d;
};
const makeId = () => crypto.randomBytes(12).toString("hex");
const makeLobbyId = () => String(crypto.randomBytes(6).readUIntBE(0, 6));

function userPublic(u) {
  return { id: u.id, name: u.name, avatar: u.avatar || "", online: u.online !== false, lastSeen: u.lastSeen || 0 };
}

function ensureUser(b) {
  const id = str(b.userId, 80) || makeId();
  let u = users.get(id);
  if (!u) {
    u = { id, name: str(b.name, 32) || "Player", avatar: str(b.avatar, 512), online: true, lastSeen: Date.now() };
    users.set(id, u);
  } else {
    if (b.name != null) u.name = str(b.name, 32) || u.name;
    if (b.avatar != null) u.avatar = str(b.avatar, 512);
    u.online = true;
    u.lastSeen = Date.now();
  }
  if (!friends.has(id)) friends.set(id, new Set());
  if (!requests.has(id)) requests.set(id, new Set());
  return u;
}

function serialize() {
  return {
    users: [...users.values()],
    friends: [...friends.entries()].map(([id, set]) => [id, [...set]]),
    requests: [...requests.entries()].map(([id, set]) => [id, [...set]]),
    lobbies: [...lobbies.values()]
  };
}

function save() {
  try {
    fs.mkdirSync(DATA_DIR, { recursive: true });
    const tmp = DATA_FILE + ".tmp";
    fs.writeFileSync(tmp, JSON.stringify(serialize()), "utf8");
    fs.renameSync(tmp, DATA_FILE);
  } catch (e) {
    console.error("STATE SAVE FAILED:", e.message);
  }
}

function load() {
  try {
    if (!fs.existsSync(DATA_FILE)) return;
    const s = JSON.parse(fs.readFileSync(DATA_FILE, "utf8"));
    for (const u of s.users || []) users.set(u.id, u);
    for (const [id, list] of s.friends || []) friends.set(id, new Set(list));
    for (const [id, list] of s.requests || []) requests.set(id, new Set(list));
    for (const l of s.lobbies || []) lobbies.set(l.id, l);
    console.log("Loaded persistent state:", users.size, "users,", lobbies.size, "lobbies");
  } catch (e) {
    console.error("STATE LOAD FAILED:", e.message);
  }
}

function playerPublic(p) {
  const u = users.get(p.userId);
  return { id: p.id, userId: p.userId || "", name: p.name, avatar: u?.avatar || p.avatar || "", online: u ? u.online !== false : true, host: !!p.host, ready: p.ready !== false };
}

function pub(l) {
  return {
    id: l.id, name: l.name, map: l.map, maxPlayers: l.maxPlayers, players: l.players.length,
    bots: l.bots, gameSpeed: l.gameSpeed, startingMoney: l.startingMoney, roundPrice: l.roundPrice,
    passworded: !!l.passwordHash, createdAt: l.createdAt, state: l.state, settings: l.settings,
    playerList: l.players.map(playerPublic)
  };
}

function send(res, status, body) {
  const d = JSON.stringify(body);
  res.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": ORIGIN, "Access-Control-Allow-Headers": "Content-Type, X-Client-Version",
    "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS"
  });
  res.end(d);
}

function sendRoot(res) {
  const body = "GENERALS API — RUNNING";
  res.writeHead(200, {
    "Content-Type": "text/plain; charset=utf-8",
    "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": ORIGIN
  });
  res.end(body);
}

const readBody = req => new Promise((resolve, reject) => {
  let d = "";
  req.on("data", c => {
    d += c;
    if (d.length > MAX) { req.destroy(); reject(Error("body_too_large")); }
  });
  req.on("end", () => {
    try { resolve(d ? JSON.parse(d) : {}); } catch (_) { reject(Error("invalid_json")); }
  });
  req.on("error", reject);
});

function clean() {
  const now = Date.now();
  for (const [id, l] of lobbies) {
    l.players = l.players.filter(p => now - p.lastSeen <= TIMEOUT);
    for (const p of l.players) {
      const u = users.get(p.userId);
      if (u) u.online = now - u.lastSeen < TIMEOUT * 2;
    }
    if (l.players.length === 0 && now - l.lastSeen > 30000) { lobbies.delete(id); continue; }
    if (now - l.lastSeen > TTL) lobbies.delete(id);
  }
}

function lobbyByToken(id, token) {
  const l = lobbies.get(id);
  if (!l) return { error: "lobby_not_found" };
  const p = l.players.find(x => x.id === str(token, 128));
  if (!p) return { error: "invalid_player_token" };
  return { l, p };
}

function updatePresence(p) {
  p.lastSeen = Date.now();
  if (p.userId) {
    const u = users.get(p.userId);
    if (u) { u.online = true; u.lastSeen = Date.now(); }
  }
}

load();
setInterval(() => { clean(); save(); }, 30000).unref();

const srv = http.createServer(async (req, res) => {
  if (req.method === "OPTIONS") {
    res.writeHead(204, {
      "Access-Control-Allow-Origin": ORIGIN, "Access-Control-Allow-Headers": "Content-Type, X-Client-Version",
      "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS"
    });
    return res.end();
  }

  clean();
  const u = new URL(req.url, "http://localhost");
  const p = u.pathname.split("/").filter(Boolean);

  try {
    if (req.method === "GET" && u.pathname === "/") return sendRoot(res);

    if (req.method === "GET" && u.pathname === "/v1/health") {
      return send(res, 200, {
        ok: true, service: "GeneralsXZH Online API", version: 3, users: users.size,
        waitingLobbies: [...lobbies.values()].filter(x => x.state === "waiting").length
      });
    }

    if (req.method === "POST" && u.pathname === "/v1/users/register") {
      const b = await readBody(req); const user = ensureUser(b); save();
      return send(res, 200, { user: userPublic(user) });
    }

    if (req.method === "GET" && u.pathname === "/v1/users/search") {
      const q = str(u.searchParams.get("q"), 32).toLowerCase();
      return send(res, 200, { users: [...users.values()].filter(x => !q || x.name.toLowerCase().includes(q) || x.id.toLowerCase().includes(q)).slice(0, 30).map(userPublic) });
    }

    if (req.method === "GET" && p[0] === "v1" && p[1] === "users" && p[2] && p[3] === "friends" && p.length === 4) {
      const id = p[2];
      return send(res, 200, {
        me: users.has(id) ? userPublic(users.get(id)) : null,
        friends: [...(friends.get(id) || new Set())].map(x => users.get(x)).filter(Boolean).map(userPublic),
        requests: [...(requests.get(id) || new Set())].map(x => users.get(x)).filter(Boolean).map(userPublic)
      });
    }

    if (req.method === "POST" && p[0] === "v1" && p[1] === "users" && p[2] && p[3] === "friends" && p.length === 4) {
      const b = await readBody(req);
      const from = ensureUser({ userId: p[2], name: b.fromName, avatar: b.fromAvatar });
      const to = users.get(str(b.userId, 80));
      if (!to || to.id === from.id) return send(res, 400, { error: "invalid_friend" });
      if (friends.get(from.id)?.has(to.id)) return send(res, 409, { error: "already_friends" });
      if (!requests.has(to.id)) requests.set(to.id, new Set());
      if (!requests.has(from.id)) requests.set(from.id, new Set());
      if (requests.get(to.id).has(from.id)) {
        requests.get(to.id).delete(from.id); friends.get(from.id).add(to.id); friends.get(to.id).add(from.id); save();
        return send(res, 200, { status: "accepted", user: userPublic(to) });
      }
      requests.get(to.id).add(from.id); save();
      return send(res, 201, { status: "requested", user: userPublic(to) });
    }

    if (req.method === "POST" && p[0] === "v1" && p[1] === "users" && p[2] && p[3] === "friends" && p[4] && p[5] === "accept") {
      const me = ensureUser({ userId: p[2] }); const from = users.get(p[4]);
      if (!from) return send(res, 404, { error: "user_not_found" });
      if (!(requests.get(me.id) || new Set()).has(from.id)) return send(res, 409, { error: "request_not_found" });
      requests.get(me.id).delete(from.id); friends.get(me.id).add(from.id); friends.get(from.id).add(me.id); save();
      return send(res, 200, { status: "accepted", user: userPublic(from) });
    }

    if (req.method === "POST" && p[0] === "v1" && p[1] === "users" && p[2] && p[3] === "friends" && p[4] && p[5] === "reject") {
      const me = ensureUser({ userId: p[2] }); const from = users.get(p[4]);
      if (!from) return send(res, 404, { error: "user_not_found" });
      requests.get(me.id)?.delete(from.id); save(); return send(res, 200, { status: "rejected" });
    }

    if (req.method === "DELETE" && p[0] === "v1" && p[1] === "users" && p[2] && p[3] === "friends" && p[4]) {
      const a = p[2], b = p[4]; friends.get(a)?.delete(b); friends.get(b)?.delete(a); save();
      return send(res, 200, { ok: true });
    }

    if (req.method === "POST" && p[0] === "v1" && p[1] === "users" && p[2] && p[3] === "presence") {
      const b = await readBody(req); const user = ensureUser({ userId: p[2], name: b.name, avatar: b.avatar });
      user.online = b.online !== false; user.lastSeen = Date.now(); save();
      return send(res, 200, { user: userPublic(user) });
    }

    if (req.method === "GET" && u.pathname === "/v1/lobbies") {
      return send(res, 200, { serverTime: Date.now(), lobbies: [...lobbies.values()].filter(l => l.state === "waiting" && l.players.length < l.maxPlayers).sort((a, b) => b.createdAt - a.createdAt).map(pub) });
    }

    if (req.method === "POST" && u.pathname === "/v1/lobbies") {
      const b = await readBody(req);
      const user = ensureUser({ userId: b.userId, name: b.playerName, avatar: b.avatar });
      const pw = str(b.password, 128), lobbyName = str(b.name, 48) || "GeneralsXZH Lobby";
      if ([...lobbies.values()].some(x => x.state === "waiting" && x.name.toLowerCase() === lobbyName.toLowerCase())) return send(res, 409, { error: "lobby_name_taken" });
      const now = Date.now(), token = makeId();
      const lobby = {
        id: makeLobbyId(), name: lobbyName, map: str(b.map, 160) || "Official Map",
        maxPlayers: Math.round(num(b.maxPlayers, 2, 8, 4)), bots: Math.round(num(b.bots, 0, 7, 0)),
        gameSpeed: num(b.gameSpeed, 0.25, 2, 1), startingMoney: Math.round(num(b.startingMoney, 10000, 200000, 10000)),
        roundPrice: Math.round(num(b.roundPrice, 10000, 200000, 10000)),
        passwordSalt: pw ? crypto.randomBytes(16).toString("hex") : "", passwordHash: "",
        createdAt: now, lastSeen: now, state: "waiting", settings: { fog: b.fog !== false, weather: b.weather !== false },
        players: [{ id: token, userId: user.id, name: user.name, host: true, ready: true, lastSeen: now }]
      };
      if (pw) lobby.passwordHash = crypto.scryptSync(pw, lobby.passwordSalt, 32).toString("hex");
      lobbies.set(lobby.id, lobby); save();
      return send(res, 201, { lobby: pub(lobby), playerToken: token });
    }

    if (p[0] === "v1" && p[1] === "lobbies" && p[2] && req.method === "POST" && p[3] === "join") {
      const lobby = lobbies.get(p[2]);
      if (!lobby) return send(res, 404, { error: "lobby_not_found" });
      if (lobby.state !== "waiting" || lobby.players.length >= lobby.maxPlayers) return send(res, 409, { error: "lobby_full" });
      const b = await readBody(req), pw = str(b.password, 128);
      if (lobby.passwordHash) {
        const candidate = crypto.scryptSync(pw, lobby.passwordSalt, 32).toString("hex");
        if (candidate !== lobby.passwordHash) return send(res, 401, { error: "bad_password" });
      }
      const user = ensureUser({ userId: b.userId, name: b.playerName, avatar: b.avatar });
      if (lobby.players.some(x => x.userId === user.id)) return send(res, 409, { error: "already_in_lobby" });
      const token = makeId();
      lobby.players.push({ id: token, userId: user.id, name: user.name, host: false, ready: true, lastSeen: Date.now() });
      lobby.lastSeen = Date.now(); save();
      return send(res, 200, { lobby: pub(lobby), playerToken: token });
    }

    if (p[0] === "v1" && p[1] === "lobbies" && p[2] && req.method === "POST" && p[3] === "heartbeat") {
      const hit = lobbyByToken(p[2], (await readBody(req)).playerToken);
      if (hit.error) return send(res, hit.error === "lobby_not_found" ? 404 : 401, { error: hit.error });
      updatePresence(hit.p); hit.l.lastSeen = Date.now(); save();
      return send(res, 200, { lobby: pub(hit.l) });
    }

    if (p[0] === "v1" && p[1] === "lobbies" && p[2] && req.method === "POST" && p[3] === "ready") {
      const b = await readBody(req), hit = lobbyByToken(p[2], b.playerToken);
      if (hit.error) return send(res, 401, { error: hit.error });
      hit.p.ready = b.ready !== false; updatePresence(hit.p); hit.l.lastSeen = Date.now(); save();
      return send(res, 200, { lobby: pub(hit.l) });
    }

    if (p[0] === "v1" && p[1] === "lobbies" && p[2] && req.method === "POST" && p[3] === "start") {
      const b = await readBody(req), hit = lobbyByToken(p[2], b.playerToken);
      if (hit.error) return send(res, 401, { error: hit.error });
      if (!hit.p.host) return send(res, 403, { error: "host_required" });
      if (hit.l.players.length < 2) return send(res, 409, { error: "not_enough_players" });
      if (hit.l.players.some(x => !x.ready)) return send(res, 409, { error: "players_not_ready" });
      hit.l.state = "playing"; hit.l.lastSeen = Date.now(); save();
      return send(res, 200, { lobby: pub(hit.l) });
    }

    if (p[0] === "v1" && p[1] === "lobbies" && p[2] && req.method === "POST" && p[3] === "settings") {
      const b = await readBody(req), hit = lobbyByToken(p[2], b.playerToken);
      if (hit.error) return send(res, 401, { error: hit.error });
      if (!hit.p.host) return send(res, 403, { error: "host_required" });
      if (hit.l.state !== "waiting") return send(res, 409, { error: "already_started" });
      if (b.name != null) hit.l.name = str(b.name, 48) || hit.l.name;
      if (b.map != null) hit.l.map = str(b.map, 160) || hit.l.map;
      if (b.maxPlayers != null) hit.l.maxPlayers = Math.round(num(b.maxPlayers, 2, 8, hit.l.maxPlayers));
      if (b.bots != null) hit.l.bots = Math.round(num(b.bots, 0, 7, hit.l.bots));
      if (b.startingMoney != null) hit.l.startingMoney = Math.round(num(b.startingMoney, 10000, 200000, hit.l.startingMoney));
      if (b.roundPrice != null) hit.l.roundPrice = Math.round(num(b.roundPrice, 10000, 200000, hit.l.roundPrice));
      if (b.fog != null) hit.l.settings.fog = !!b.fog;
      if (b.weather != null) hit.l.settings.weather = !!b.weather;
      if (b.password != null) {
        const newPassword = str(b.password, 128);
        if (newPassword) {
          hit.l.passwordSalt = crypto.randomBytes(16).toString("hex");
          hit.l.passwordHash = crypto.scryptSync(newPassword, hit.l.passwordSalt, 32).toString("hex");
        } else { hit.l.passwordSalt = ""; hit.l.passwordHash = ""; }
      }
      if (hit.l.maxPlayers < hit.l.players.length) hit.l.maxPlayers = hit.l.players.length;
      hit.l.lastSeen = Date.now(); save();
      return send(res, 200, { lobby: pub(hit.l) });
    }

    if (p[0] === "v1" && p[1] === "lobbies" && p[2] && req.method === "POST" && p[3] === "leave") {
      const b = await readBody(req), hit = lobbyByToken(p[2], b.playerToken);
      if (hit.error) return send(res, 401, { error: hit.error });
      const wasHost = hit.p.host;
      hit.l.players = hit.l.players.filter(x => x.id !== hit.p.id);
      if (wasHost) { if (hit.l.players.length) hit.l.players[0].host = true; else lobbies.delete(hit.l.id); }
      hit.l.lastSeen = Date.now(); save();
      return send(res, 200, { ok: true, lobby: lobbies.has(hit.l.id) ? pub(hit.l) : null });
    }

    return send(res, 404, { error: "not_found" });
  } catch (e) {
    console.error("REQUEST ERROR", e);
    return send(res, e.message === "invalid_json" ? 400 : 500, { error: e.message || "server_error" });
  }
});

srv.listen(PORT, HOST, () => console.log("GeneralsXZH Online API v3 listening on " + HOST + ":" + PORT));
