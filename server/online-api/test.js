#!/usr/bin/env node
"use strict";
const assert = require("node:assert");

async function main() {
  const base = process.env.TEST_BASE || "http://127.0.0.1:8080";
  const json = async (url, options) => {
    const r = await fetch(base + url, options);
    const body = await r.json();
    assert.ok(r.ok, JSON.stringify(body));
    return body;
  };

  const health = await json("/v1/health");
  assert.equal(health.ok, true);

  const host = await json("/v1/lobbies", {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify({userId:"test-host", playerName:"Host", name:"Apple P2P Test"})
  });
  assert.ok(host.lobby.id);
  assert.ok(host.playerToken);

  const client = await json("/v1/lobbies/" + host.lobby.id + "/join", {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify({userId:"test-client", playerName:"Client"})
  });
  assert.ok(client.playerToken);

  await json("/v1/lobbies/" + host.lobby.id + "/candidates", {
    method: "POST",
    headers: {"content-type": "application/json"},
    body: JSON.stringify({
      playerToken: host.playerToken,
      candidates: [{type:"srflx", address:"203.0.113.10", port:45000, protocol:"udp"}]
    })
  });

  const peers = await json("/v1/lobbies/" + host.lobby.id + "/candidates?playerToken=" + encodeURIComponent(client.playerToken));
  assert.equal(peers.peers.length, 1);
  assert.equal(peers.peers[0].candidates[0].port, 45000);

  console.log("online-api tests passed");
}

main().catch(error => { console.error(error); process.exit(1); });
