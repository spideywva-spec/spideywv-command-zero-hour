"use strict";
const http=require("http");
const devPanel=require("./devpanel");

process.env.PORT="8081";
process.env.HOST="127.0.0.1";
require("./server.js");

const upstream=http.request;
function proxy(req,res){
  const opts={hostname:"127.0.0.1",port:8081,path:req.url,method:req.method,headers:{...req.headers,host:"127.0.0.1:8081"}};
  const r=upstream(opts,pr=>{
    res.writeHead(pr.statusCode||502,pr.headers);
    pr.pipe(res);
  });
  r.on("error",e=>{if(!res.headersSent){res.writeHead(502,{"Content-Type":"application/json"});res.end(JSON.stringify({error:"upstream_unavailable",message:e.message}))}});
  req.pipe(r);
}
const srv=http.createServer(async(req,res)=>{
  const u=new URL(req.url,"http://localhost");
  try{
    const handled=await devPanel.handle(req,res,u);
    if(handled!==false)return handled;
  }catch(e){
    if(!res.headersSent){res.writeHead(500,{"Content-Type":"application/json"});res.end(JSON.stringify({error:e.message||"server_error"}))}
    return;
  }
  proxy(req,res);
});
srv.listen(Number(process.env.PUBLIC_PORT||process.env.PORT||8080),"0.0.0.0",()=>console.log("SpideyDev gateway listening"));
