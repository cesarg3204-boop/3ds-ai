import assert from "node:assert/strict";
import worker, { parseRequest, esc, unesc, plain, parseRss, systemPrompt, chat, planSearch, pageText, answerText, gemini, resetGrounding, rpcResult, mcpServers } from "../src/worker.js";

let passed = 0;
async function test(name, fn) { try { await fn(); passed++; console.log("ok -", name); } catch (e) { console.log("FAIL -", name); console.log(e); process.exitCode = 1; } }

await test("escape round trip", () => {
  const s = "línea uno\nline\ttwo \\ back";
  assert.equal(unesc(esc(s)), s);
  assert.ok(!esc(s).includes("\n"));
});

await test("parse request", () => {
  const r = parseRequest(`P\tgamer\nI\tI'm cutting\\n180g protein\nW\t1\nU\thola\nA\they\nU\tqué tal`);
  assert.equal(r.persona, "gamer");
  assert.equal(r.project, "I'm cutting\n180g protein");
  assert.equal(r.web, true);
  assert.deepEqual(r.messages.map((m) => m.role), ["user", "assistant", "user"]);
  assert.equal(r.messages[2].content, "qué tal");
});

await test("plain strips markdown", () => {
  assert.equal(plain("## Title\n**bold** and `code`\n* item"), "Title\nbold and code\n- item");
});

await test("rss parse unwraps bing", () => {
  const xml = `<rss><item><title>A &amp; B</title><link>https://www.bing.com/news/apiclick.aspx?url=https%3A%2F%2Fx.com%2Fa&amp;c=1</link><description>&lt;b&gt;hi&lt;/b&gt;</description></item></rss>`;
  const r = parseRss(xml);
  assert.equal(r[0].title, "A & B");
  assert.equal(r[0].link, "https://x.com/a");
  assert.equal(r[0].text, "hi");
});

await test("system prompt has persona, language, project, results", () => {
  const p = systemPrompt({ persona: "nofluff", custom: "", project: "Gym stuff" }, [{ title: "T", text: "D", date: "" }], "2026-11-25");
  assert.match(p, /briefly/);
  assert.match(p, /same language/);
  assert.match(p, /Gym stuff/);
  assert.match(p, /\[1\] T/);
  assert.match(systemPrompt({ persona: "custom", custom: "Talk like a pirate", project: "" }, null, "x"), /pirate/);
});

await test("chat calls AI and formats answer", async () => {
  let seen;
  const env = { AI: { run: async (m, o) => { seen = o; return { response: "**Hi** there\nline 2" }; } } };
  const out = await chat("P\tchill\nU\thello", env, "2026-11-25");
  assert.equal(out, "OK\nA\tHi there\\nline 2\n");
  assert.equal(seen.messages[0].role, "system");
  assert.equal(seen.messages[1].content, "hello");
});

await test("falls back to another model", async () => {
  const tried = [];
  const env = { AI: { run: async (m) => { tried.push(m); if (tried.length === 1) throw new Error("no such model"); return { response: "ok" }; } } };
  assert.equal(await chat("U\thi", env, "d"), "OK\nA\tok\n");
  assert.equal(tried.length, 2);
});

await test("ping", async () => {
  const r = await worker.fetch(new Request("https://x/v1/ping"), { AI: {} });
  assert.equal(await r.text(), "SPIKE OK\tai\tcloudflare\n");
});

await test("planner keeps the topic of the chat", async () => {
  let seen;
  const env = { AI: { run: async (m, o) => { seen = o; return { response: "SEARCH: Quest chips macros nutrition facts" }; } } };
  const q = await planSearch(env, [{ role: "user", content: "quest chips?" }, { role: "assistant", content: "..." }, { role: "user", content: "and the macros?" }], false);
  assert.equal(q, "Quest chips macros nutrition facts");
  assert.match(seen.messages[1].content, /quest chips\?/);
});

await test("planner can skip searching", async () => {
  const env = { AI: { run: async () => ({ response: "NONE" }) } };
  assert.equal(await planSearch(env, [{ role: "user", content: "hi" }], false), "");
  assert.equal(await planSearch(env, [{ role: "user", content: "hi" }], true), "hi");
});

await test("page text keeps matching lines and neighbors", () => {
  const html = "<html><script>junk()</script><nav>Menu</nav><body><p>Welcome to our shop of things</p><div>Quest Tortilla Style Protein Chips</div><table><tr><td>Protein</td></tr><tr><td>18g</td></tr></table><p>Unrelated footer text here</p></body></html>";
  const t = pageText(html, "quest chips protein");
  assert.match(t, /Quest Tortilla/);
  assert.match(t, /Protein\n18g/);
  assert.doesNotMatch(t, /junk|Menu/);
});

await test("answer text from different model shapes", () => {
  assert.equal(answerText({ choices: [{ message: { content: "a" } }] }), "a");
  assert.equal(answerText({ output: [{ type: "reasoning" }, { type: "message", content: [{ type: "output_text", text: "b" }] }] }), "b");
  assert.equal(answerText({ response: "c" }), "c");
});

await test("plain removes characters the 3DS can't show", () => {
  assert.equal(plain("high\u2011protein 16\u202Fg \u201Cchewy\u201D bar 4\u3011. Cite \u30104\u2020L1-L3\u3011 \u{1F600}"), 'high-protein 16 g "chewy" bar 4. Cite [4]');
});

await test("gemini request and sources", async () => {
  let url, sent;
  const fakeFetch = async (u, o) => { url = u; sent = JSON.parse(o.body); return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: "Quest chips have 18g protein." }] }, groundingMetadata: { groundingChunks: [{ web: { uri: "https://r/1", title: "questnutrition.com" } }, { web: { uri: "https://r/2", title: "questnutrition.com" } }] } }] }), { status: 200 }); };
  const g = await gemini({ persona: "chill", custom: "", project: "", web: true }, [{ role: "user", content: "hi" }, { role: "assistant", content: "yo" }, { role: "user", content: "quest chips protein" }], { GEMINI_KEY: "k" }, "d", fakeFetch);
  assert.match(url, /gemini-3-flash-preview:generateContent/);
  assert.deepEqual(sent.contents.map((c) => c.role), ["user", "model", "user"]);
  assert.ok(sent.tools[0].google_search);
  assert.equal(g.text, "Quest chips have 18g protein.");
  assert.equal(g.sources.length, 1);
});

await test("gemini tries the next model when one is busy", async () => {
  const tried = [];
  const fakeFetch = async (u) => { tried.push(u); return tried.length === 1 ? new Response("{}", { status: 429 }) : new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: "ok" }] } }] })); };
  const g = await gemini({ persona: "chill", custom: "", project: "", web: false }, [{ role: "user", content: "hi" }], { GEMINI_KEY: "k" }, "d", fakeFetch);
  assert.equal(g.text, "ok");
  assert.equal(tried.length, 2);
});

await test("photo, file and voice lines attach to the user message", () => {
  const r = parseRequest("U\twhat is this\nG\timage/png\tAAAA\nX\tnotes.txt\tline1\\nline2\nU\t\nV\tUklGRg==");
  assert.equal(r.messages[0].images[0].mime, "image/png");
  assert.match(r.messages[0].content, /Attached file "notes.txt"\]\nline1\nline2/);
  assert.equal(r.messages[1].voice, "UklGRg==");
});

await test("voice is transcribed and sent back", async () => {
  const env = { AI: { run: async (m, o) => m.includes("whisper") ? { text: " hola spike " } : { response: "¡Hola!" } } };
  const out = await chat("U\t\nV\tUklGRg==", env, "d");
  assert.equal(out, "OK\nT\thola spike\nA\t¡Hola!\n");
});

await test("photos without gemini say so", async () => {
  const env = { AI: { run: async () => ({ response: "x" }) } };
  assert.match(await chat("U\t\nG\timage/png\tAAAA", env, "d"), /^ERR\tPhotos need Gemini/);
});

await test("gemini gets the photo", async () => {
  let sent;
  const fakeFetch = async (u, o) => { sent = JSON.parse(o.body); return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: "A cat" }] } }] })); };
  const req = parseRequest("U\t\nG\timage/jpeg\tQUJD");
  req.messages[0].content = "(photo)";
  await gemini(req, req.messages, { GEMINI_KEY: "k" }, "d", fakeFetch);
  assert.deepEqual(sent.contents[0].parts[0], { inline_data: { mime_type: "image/jpeg", data: "QUJD" } });
});

await test("google search quota: falls back to our own search", async () => {
  resetGrounding();
  const calls = [];
  const fakeFetch = async (u, o) => {
    const b = JSON.parse(o.body);
    calls.push(!!b.tools);
    if (b.tools) return new Response(JSON.stringify({ error: { message: "You exceeded your current quota" } }), { status: 429 });
    assert.match(b.systemInstruction.parts[0].text, /Quest page text/);
    return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: "18g" }] } }] }));
  };
  const lookup = async () => [{ title: "Quest", link: "https://quest", text: "", page: "Quest page text" }];
  const env = { GEMINI_KEY: "k", AI: { run: async () => ({ response: "SEARCH: quest chips protein" }) } };
  const g = await gemini({ persona: "chill", custom: "", project: "", web: true }, [{ role: "user", content: "quest chips protein?" }], env, "d", fakeFetch, lookup);
  assert.equal(g.text, "18g");
  assert.deepEqual(g.sources, [{ title: "Quest", link: "https://quest" }]);
  assert.deepEqual(calls, [true, false]);
  // next time it skips Google Search right away
  calls.length = 0;
  await gemini({ persona: "chill", custom: "", project: "", web: true }, [{ role: "user", content: "x" }], env, "d", fakeFetch, lookup);
  assert.deepEqual(calls, [false]);
  resetGrounding();
});

await test("rpc answers can be SSE", () => {
  assert.equal(rpcResult('event: message\ndata: {"jsonrpc":"2.0","id":2,"result":{"tools":[]}}\n\n', 2).result.tools.length, 0);
  assert.equal(rpcResult('{"jsonrpc":"2.0","id":1,"result":{}}', 1).id, 1);
});

await test("mcp servers from variables", () => {
  const s = mcpServers({ FIRECRAWL_KEY: " fc-1 ", ZAPIER_TOKEN: "z", MCP_SERVERS: "wiki https://w.example/mcp tok\nbad notaurl" });
  assert.deepEqual(s.map((x) => x.name), ["firecrawl", "zapier", "wiki"]);
  assert.equal(s[0].url, "https://mcp.firecrawl.dev/fc-1/v2/mcp");
  assert.equal(s[1].token, "z");
});

await test("gemini calls an MCP tool and answers with the result", async () => {
  const seen = [];
  const fakeFetch = async (u, o) => {
    const b = JSON.parse(o.body);
    if (u.startsWith("https://mcp.firecrawl.dev")) {
      if (b.method === "initialize") return new Response('{"jsonrpc":"2.0","id":1,"result":{}}', { headers: { "Mcp-Session-Id": "s1" } });
      if (!b.id) return new Response("", { status: 202 });
      assert.equal(o.headers["Mcp-Session-Id"], "s1");
      if (b.method === "tools/list") return new Response(`event: message\ndata: ${JSON.stringify({ jsonrpc: "2.0", id: b.id, result: { tools: [{ name: "firecrawl_search", description: "Search", inputSchema: { type: "object", properties: { query: { type: "string" } } } }] } })}\n\n`);
      if (b.method === "tools/call") { seen.push(b.params); return new Response(JSON.stringify({ jsonrpc: "2.0", id: b.id, result: { content: [{ type: "text", text: "Quest chips: 19g protein" }] } })); }
    }
    // Gemini
    const last = b.contents[b.contents.length - 1];
    if (last.parts[0].functionResponse) {
      assert.match(last.parts[0].functionResponse.response.result, /19g/);
      return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ text: "19g of protein." }] } }] }));
    }
    assert.equal(b.tools[0].functionDeclarations[0].name, "firecrawl__firecrawl_search");
    return new Response(JSON.stringify({ candidates: [{ content: { parts: [{ functionCall: { name: "firecrawl__firecrawl_search", args: { query: "quest chips protein" } }, thoughtSignature: "x" }] } }] }));
  };
  const g = await gemini({ persona: "chill", custom: "", project: "", web: false, mcp: true }, [{ role: "user", content: "quest chips protein?" }], { GEMINI_KEY: "k", FIRECRAWL_KEY: "fc" }, "d", fakeFetch);
  assert.equal(g.text, "19g of protein.");
  assert.deepEqual(seen[0], { name: "firecrawl_search", arguments: { query: "quest chips protein" } });
  assert.equal(g.sources[0].title, "Used firecrawl: firecrawl_search");
});

console.log(`${passed} tests passed`);
