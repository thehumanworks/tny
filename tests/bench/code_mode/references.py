"""Hand-written executor controls. NEVER included in model-generation prompts."""

PREFIX = {
    "lua": """local function call(n,a) return tools.call(n,json.encode(a)) end
local function read(p) return call("fs.read",{path=p}) end
local function write(p,c) return call("fs.write",{path=p,content=c}) end
local function array() return json.decode("[]") end
local function save(v) write("output.json",json.encode(v)); print("done") end
""",
    "javascript": """const call=(n,a)=>tools.call(n,JSON.stringify(a));
const read=p=>call("fs.read",{path:p});
const write=(p,c)=>call("fs.write",{path:p,content:c});
const save=v=>{write("output.json",JSON.stringify(v));print("done");};
""",
    "python": """def call(n,a): return tools.call(n,json.dumps(a))
def read(p): return call("fs.read",{"path":p})
def write(p,c): return call("fs.write",{"path":p,"content":c})
def save(v):
    write("output.json",json.dumps(v))
    print("done")
""",
}

LUA = {
    "filter": """local d=json.decode(read("input.json")); local out=array()
for _,x in ipairs(d.items) do if x.active==true and x.score>=d.minimum then out[#out+1]=x.id end end
table.sort(out); save(out)""",
    "aggregate": """local out={}; for _,x in ipairs(json.decode(read("input.json"))) do
if x.approved==true then out[x.department]=(out[x.department] or 0)+x.cents end end; save(out)""",
    "join": """local names={}; for _,u in ipairs(json.decode(read("users.json"))) do names[u.id]=u.name end
local out=array(); for _,t in ipairs(json.decode(read("tickets.json"))) do out[#out+1]={id=t.id,owner=names[t.owner] or json.null} end; save(out)""",
    "pagination": """local cursor=json.null; local seen={}; local out=array()
repeat local p=json.decode(call("api.page",{cursor=cursor})); for _,x in ipairs(p.items) do
if not seen[x.id] then seen[x.id]=true; out[#out+1]=x.id end end; cursor=p.next until cursor==json.null
table.sort(out); save(out)""",
    "batch_edit": """local changed=array(); local paths=json.decode(call("fs.list",{prefix="src/"}))
for _,p in ipairs(paths) do local text=read(p); local next,n=string.gsub(text,"FEATURE=false","FEATURE=true")
if n>0 then write(p,next); changed[#changed+1]=p end end; table.sort(changed); save(changed)""",
    "json_shapes": """local d=json.decode(read("input.json")); d.status="ready"; save(d)""",
    "ndjson": """local out={}; for line in string.gmatch(read("events.ndjson"),"[^\\n]+") do
if string.find(line,"%S") then local v=json.decode(line); out[v.level]=(out[v.level] or 0)+1 end end; save(out)""",
    "top_k": """local items=json.decode(read("input.json")); table.sort(items,function(a,b)
if a.score==b.score then return a.id<b.id end; return a.score>b.score end)
local out=array(); for i=1,math.min(3,#items) do out[#out+1]=items[i] end; save(out)""",
    "bounded_retry": """local out={}; for _,id in ipairs(json.decode(read("input.json"))) do out[id]=json.null
for i=1,3 do local r=json.decode(call("api.check",{id=id})); if r.ok==true then out[id]=r.value; break end
if r.retry~=true then break end end end; save(out)""",
    "literal_replace": """local d=json.decode(read("input.json")); local parts={}; local pos=1; local count=0
while true do local a,b=string.find(d.text,d.needle,pos,true); if not a then parts[#parts+1]=string.sub(d.text,pos); break end
parts[#parts+1]=string.sub(d.text,pos,a-1); parts[#parts+1]=d.replacement; pos=b+1; count=count+1 end
save({text=table.concat(parts),count=count})""",
    "unicode": """local out=array(); for _,x in ipairs(json.decode(read("input.json"))) do
local n=utf8.len(x.text); local take=math.min(x.take,n); local stop=utf8.offset(x.text,take+1)
out[#out+1]={prefix=string.sub(x.text,1,(stop or (#x.text+1))-1),length=n} end; save(out)""",
    "dependency_order": """local graph=json.decode(read("input.json")); local keys={}; for k in pairs(graph) do keys[#keys+1]=k end
table.sort(keys); local done={}; local out=array(); while #out<#keys do for _,k in ipairs(keys) do if not done[k] then
local ready=true; for _,d in ipairs(graph[k]) do if not done[d] then ready=false end end
if ready then done[k]=true; out[#out+1]=k; break end end end end; save(out)""",
}
JAVASCRIPT = {
    "filter": 'const d=JSON.parse(read("input.json")); save(d.items.filter(x=>x.active===true&&x.score>=d.minimum).map(x=>x.id).sort());',
    "aggregate": 'const out={}; for(const x of JSON.parse(read("input.json"))) if(x.approved===true) out[x.department]=(out[x.department]||0)+x.cents; save(out);',
    "join": 'const names=Object.fromEntries(JSON.parse(read("users.json")).map(u=>[u.id,u.name])); save(JSON.parse(read("tickets.json")).map(t=>({id:t.id,owner:names[t.owner]??null})));',
    "pagination": 'let cursor=null; const ids=new Set(); do {const p=JSON.parse(call("api.page",{cursor})); for(const x of p.items) ids.add(x.id); cursor=p.next;} while(cursor!==null); save([...ids].sort());',
    "batch_edit": 'const changed=[]; for(const p of JSON.parse(call("fs.list",{prefix:"src/"}))) {const text=read(p); const next=text.replaceAll("FEATURE=false","FEATURE=true"); if(text!==next){write(p,next);changed.push(p);}} save(changed.sort());',
    "json_shapes": 'const d=JSON.parse(read("input.json")); d.status="ready"; save(d);',
    "ndjson": 'const out={}; for(const line of read("events.ndjson").split("\\n")){if(line.trim()){const v=JSON.parse(line);out[v.level]=(out[v.level]||0)+1;}} save(out);',
    "top_k": 'const items=JSON.parse(read("input.json")); items.sort((a,b)=>b.score-a.score||(a.id<b.id?-1:a.id>b.id?1:0)); save(items.slice(0,3));',
    "bounded_retry": 'const out={}; for(const id of JSON.parse(read("input.json"))){out[id]=null;for(let i=0;i<3;i++){const r=JSON.parse(call("api.check",{id}));if(r.ok===true){out[id]=r.value;break;} if(r.retry!==true)break;}} save(out);',
    "literal_replace": 'const d=JSON.parse(read("input.json")); const parts=d.text.split(d.needle); save({text:parts.join(d.replacement),count:parts.length-1});',
    "unicode": 'save(JSON.parse(read("input.json")).map(x=>({prefix:[...x.text].slice(0,x.take).join(""),length:[...x.text].length})));',
    "dependency_order": 'const graph=JSON.parse(read("input.json")),keys=Object.keys(graph).sort(),done=new Set(),out=[]; while(out.length<keys.length){const k=keys.find(k=>!done.has(k)&&graph[k].every(d=>done.has(d)));done.add(k);out.push(k);}save(out);',
}
PYTHON = {
    "filter": 'd=json.loads(read("input.json"))\nsave(sorted(x["id"] for x in d["items"] if x.get("active") is True and x["score"]>=d["minimum"]))',
    "aggregate": 'out={}\nfor x in json.loads(read("input.json")):\n    if x["approved"] is True: out[x["department"]]=out.get(x["department"],0)+x["cents"]\nsave(out)',
    "join": 'names={u["id"]:u["name"] for u in json.loads(read("users.json"))}\nsave([{"id":t["id"],"owner":names.get(t["owner"])} for t in json.loads(read("tickets.json"))])',
    "pagination": 'cursor=None\nids=set()\nwhile True:\n    p=json.loads(call("api.page",{"cursor":cursor}))\n    ids.update(x["id"] for x in p["items"])\n    cursor=p["next"]\n    if cursor is None: break\nsave(sorted(ids))',
    "batch_edit": 'changed=[]\nfor p in json.loads(call("fs.list",{"prefix":"src/"})):\n    text=read(p)\n    new=text.replace("FEATURE=false","FEATURE=true")\n    if new!=text:\n        write(p,new)\n        changed.append(p)\nsave(sorted(changed))',
    "json_shapes": 'd=json.loads(read("input.json"))\nd["status"]="ready"\nsave(d)',
    "ndjson": 'out={}\nfor line in read("events.ndjson").splitlines():\n    if line.strip():\n        v=json.loads(line)\n        out[v["level"]]=out.get(v["level"],0)+1\nsave(out)',
    "top_k": 'save(sorted(json.loads(read("input.json")),key=lambda x:(-x["score"],x["id"]))[:3])',
    "bounded_retry": 'out={}\nfor id in json.loads(read("input.json")):\n    out[id]=None\n    for i in range(3):\n        r=json.loads(call("api.check",{"id":id}))\n        if r["ok"] is True:\n            out[id]=r["value"]\n            break\n        if r.get("retry") is not True: break\nsave(out)',
    "literal_replace": 'd=json.loads(read("input.json"))\nsave({"text":d["text"].replace(d["needle"],d["replacement"]),"count":d["text"].count(d["needle"])})',
    "unicode": 'save([{"prefix":x["text"][:x["take"]],"length":len(x["text"])} for x in json.loads(read("input.json"))])',
    "dependency_order": 'graph=json.loads(read("input.json"))\ndone=set()\nout=[]\nwhile len(out)<len(graph):\n    k=min(k for k in graph if k not in done and all(d in done for d in graph[k]))\n    done.add(k)\n    out.append(k)\nsave(out)',
}


def reference(language: str, task: str) -> str:
    return (
        PREFIX[language]
        + {"lua": LUA, "javascript": JAVASCRIPT, "python": PYTHON}[language][task]
        + "\n"
    )
