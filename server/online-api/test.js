const assert = require("assert");
const { spawn } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const port = 18080 + Math.floor(Math.random() * 1000);
const data = fs.mkdtempSync(path.join(os.tmpdir(), "gx-online-"));
const child = spawn(process.execPath, ["server.js"], {
  cwd: __dirname,
  env: { ...process.env, PORT: String(port), DATA_DIR: data },
  stdio: ["ignore", "pipe", "pipe"]
});

const base = "http://127.0.0.1:" + port;
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function api(method, route, body) {
  const res = await fetch(base + route, {
    method,
    headers: body ? {"content-type":"application/json"} : {},
    body: body ? JSON.stringify(body) : undefined
  });
  const json = await res.json();
  return {status:res.status, json};
}

(async()=>{
  try {
    await sleep(250);
    let r=await api("GET","/v1/health");
    assert.equal(r.status,200); assert.equal(r.json.ok,true);

    const aliceId="test-alice";
    const bobId="test-bob";
    r=await api("POST","/v1/users/register",{userId:aliceId,name:"Alice"});
    assert.equal(r.status,200);
    r=await api("POST","/v1/users/register",{userId:bobId,name:"Bob"});
    assert.equal(r.status,200);

    r=await api("POST","/v1/lobbies",{userId:aliceId,playerName:"Alice",name:"CI Lobby",map:"",maxPlayers:2,bots:0,startingMoney:50000,roundPrice:25000});
    assert.equal(r.status,201);
    const lobby=r.json.lobby, hostToken=r.json.playerToken;
    assert.ok(/^\d+$/.test(lobby.id));

    r=await api("GET","/v1/lobbies");
    assert.equal(r.status,200); assert.equal(r.json.lobbies.length,1);

    r=await api("POST","/v1/lobbies/"+lobby.id+"/join",{userId:bobId,playerName:"Bob"});
    assert.equal(r.status,200);
    const guestToken=r.json.playerToken;
    assert.equal(r.json.lobby.players,2);

    r=await api("POST","/v1/lobbies/"+lobby.id+"/start",{playerToken:guestToken});
    assert.equal(r.status,403);

    r=await api("POST","/v1/lobbies/"+lobby.id+"/start",{playerToken:hostToken});
    assert.equal(r.status,200); assert.equal(r.json.lobby.state,"playing");

    r=await api("POST","/v1/users/"+aliceId+"/friends",{userId:bobId,fromName:"Alice"});
    assert.equal(r.status,201);
    r=await api("GET","/v1/users/"+bobId+"/friends");
    assert.equal(r.status,200); assert.equal(r.json.requests.length,1);

    r=await api("POST","/v1/users/"+bobId+"/friends/"+aliceId+"/accept",{});
    assert.equal(r.status,200);
    r=await api("GET","/v1/users/"+aliceId+"/friends");
    assert.equal(r.status,200); assert.equal(r.json.friends.length,1);

    console.log("ONLINE API INTEGRATION TEST: PASS");
  } finally {
    child.kill("SIGTERM");
    fs.rmSync(data,{recursive:true,force:true});
  }
})().catch(err=>{console.error(err);child.kill("SIGTERM");fs.rmSync(data,{recursive:true,force:true});process.exit(1);});
