// Spike AI relay: a Cloudflare Worker that does the AI work for the 3DS app.
//
// The 3DS sends a chat in a simple line format (no JSON on the 3DS side); the
// relay adds Spike's personality, optionally looks things up on the web, asks
// Cloudflare Workers AI (free daily allowance) and sends back plain text.
//
// Setup (Cloudflare dashboard): Workers AI binding named AI, and (smarter) a secret
// GEMINI_KEY from aistudio.google.com. Optional variables MODEL, GEMINI_MODEL.

// Smartest first; if one fails (or isn't on your account) the next is tried.
const MODELS = [
  "@cf/openai/gpt-oss-120b",
  "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
  "@cf/meta/llama-3.1-8b-instruct-fast",
];
// Small, quick model that only decides what to search for.
const PLANNER = "@cf/meta/llama-3.1-8b-instruct-fast";

const UA = "Mozilla/5.0 (compatible; SpikeAI-Relay/1.0)";

export const PERSONAS = {
  nofluff: "Answer as briefly as possible. No intro, no filler, no recap, no follow-up questions unless you truly need more info. One or two sentences when that is enough.",
  chill: "You are a chill, friendly buddy. Talk casually like a friend texting, keep it short, light humor is welcome, never preachy.",
  analyst: "You are a sharp analyst. Give thorough, well-organized answers: break things down step by step, compare options with pros and cons, use numbers when helpful, and end with a clear recommendation.",
  searcher: "You are a research assistant. Base your answer on the web results you are given, say what is current, and cite sources with their numbers like [1]. If the results don't answer the question, say so.",
  gamer: "You are a hype video game buddy who knows tons about games (Nintendo, PlayStation, Xbox, PC). Give practical tips, strategies and recommendations with energy, like a friend who games a lot.",
};

// ---------- the 3DS line format ----------
// Each line is KEY<TAB>value; values escape backslash, newline and tab.
//   P persona id   C custom instructions   I project instructions   W 1 = look things up
//   U user message  A Spike's earlier answer (in order)

export function unesc(s) {
  return String(s).replace(/\\(.)/g, (_, c) => (c === "n" ? "\n" : c === "t" ? "\t" : c));
}

export function esc(s) {
  return String(s).replace(/\\/g, "\\\\").replace(/\n/g, "\\n").replace(/\t/g, "\\t").replace(/\r/g, "");
}

export function parseRequest(body) {
  const req = { persona: "chill", custom: "", project: "", web: false, messages: [] };
  for (const line of String(body).split("\n")) {
    const tab = line.indexOf("\t");
    if (tab < 0) continue;
    const k = line.slice(0, tab), v = unesc(line.slice(tab + 1));
    if (k === "P") req.persona = v.trim();
    else if (k === "C") req.custom = v;
    else if (k === "I") req.project = v;
    else if (k === "W") req.web = v.trim() === "1";
    else if (k === "M") req.mcp = v.trim() === "1";
    else if (k === "U") req.messages.push({ role: "user", content: v, images: [] });
    else if (k === "A") req.messages.push({ role: "assistant", content: v });
    else if (req.messages.length) {
      const m = req.messages[req.messages.length - 1];
      if (m.role !== "user") continue;
      if (k === "G") { // picture: G<TAB>mime<TAB>base64
        const t = v.indexOf("\t");
        if (t > 0) m.images.push({ mime: v.slice(0, t), data: v.slice(t + 1) });
      } else if (k === "X") { // text file: X<TAB>name<TAB>content
        const t = v.indexOf("\t");
        const name = t > 0 ? v.slice(0, t) : "file.txt", content = t > 0 ? v.slice(t + 1) : v;
        m.content += `\n\n[Attached file "${name}"]\n${content.slice(0, 20000)}`;
      } else if (k === "V") m.voice = v; // recorded speech, base64 WAV
    }
  }
  return req;
}

// ---------- web lookups ----------

function decodeEntities(s) {
  return String(s)
    .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(+n))
    .replace(/&#x([0-9a-f]+);/gi, (_, n) => String.fromCodePoint(parseInt(n, 16)))
    .replace(/&quot;/g, '"').replace(/&apos;|&#39;/g, "'").replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&nbsp;/g, " ").replace(/&amp;/g, "&");
}

function tag(block, name) {
  const m = block.match(new RegExp(`<${name}\\b[^>]*>([\\s\\S]*?)</${name}>`, "i"));
  return m ? decodeEntities(m[1]).replace(/<[^>]+>/g, "").trim() : "";
}

export function parseRss(xml) {
  return (String(xml).match(/<item\b[\s\S]*?<\/item>/gi) || []).map((b) => {
    let link = tag(b, "link");
    try { const u = new URL(link); if (/bing\.com$/.test(u.hostname) && u.searchParams.get("url")) link = u.searchParams.get("url"); } catch {}
    return { title: tag(b, "title"), link, text: tag(b, "description"), date: tag(b, "pubDate") };
  }).filter((r) => r.title && /^https?:/.test(r.link));
}

async function getText(url) {
  const res = await fetch(url, { headers: { "User-Agent": UA, "Accept-Language": "en-US,en;q=0.9,es;q=0.8" }, cf: { cacheTtl: 600 } });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.text();
}

const SKIP = /(amazon|walmart|target|ebay|bestbuy|kroger|costco|youtube|tiktok|instagram|facebook|pinterest)\./i;

async function search(query) {
  const enc = encodeURIComponent(query);
  const [news, web] = await Promise.all([
    getText(`https://www.bing.com/news/search?q=${enc}&format=rss&setlang=en-US`).then(parseRss).catch(() => []),
    getText(`https://www.bing.com/search?q=${enc}&format=rss&setlang=en-US&count=10`).then(parseRss).catch(() => []),
  ]);
  const out = [], seen = new Set();
  // Real web pages first (they answer questions), then a couple of news items.
  for (const r of [...web.slice(0, 6), ...news.slice(0, 3), ...web.slice(6)]) {
    if (out.length >= 7) break;
    if (seen.has(r.link) || SKIP.test(r.link)) continue;
    seen.add(r.link);
    out.push(r);
  }
  return out;
}

function words(q) {
  return [...new Set(String(q).toLowerCase().split(/[^\p{L}\p{N}]+/u).filter((w) => w.length > 2))];
}

// The readable text of a page, keeping the parts that match the query.
export function pageText(html, query, max = 2500) {
  const body = String(html)
    .replace(/<(script|style|noscript|svg|nav|footer|header|form|iframe)\b[\s\S]*?<\/\1>/gi, " ")
    .replace(/<!--[\s\S]*?-->/g, " ")
    .replace(/<(br|\/p|\/div|\/li|\/h\d|\/tr|\/td|\/th|\/section|\/article)\b[^>]*>/gi, "\n")
    .replace(/<[^>]+>/g, " ");
  const lines = decodeEntities(body).split("\n").map((l) => l.replace(/\s+/g, " ").trim()).filter((l) => l.length > 2 && l.length < 600);
  const keys = words(query);
  const scored = lines.map((l, i) => {
    const low = l.toLowerCase();
    let sc = 0;
    for (const k of keys) if (low.includes(k)) sc += 2;
    if (/\d/.test(l)) sc += 1;
    if (l.length > 60) sc += 1;
    return { l, i, sc };
  });
  const pick = [...scored].sort((a, b) => b.sc - a.sc || a.i - b.i);
  const keep = new Set();
  let len = 0;
  for (const p of pick) {
    if (p.sc <= 0 || len > max) break;
    // keep the neighbors too: numbers often sit on the next line (e.g. "Protein" / "21g")
    for (const j of [p.i - 1, p.i, p.i + 1]) {
      if (j < 0 || j >= lines.length || keep.has(j)) continue;
      keep.add(j);
      len += lines[j].length + 1;
    }
  }
  return [...keep].sort((a, b) => a - b).map((j) => lines[j]).join("\n").slice(0, max);
}

async function fetchPage(url, query) {
  const res = await fetch(url, {
    headers: { "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124 Safari/537.36", "Accept-Language": "en-US,en;q=0.9,es;q=0.8" },
    redirect: "follow",
    signal: AbortSignal.timeout(5000),
  });
  if (!res.ok || !/html/.test(res.headers.get("content-type") || "")) return "";
  const html = (await res.text()).slice(0, 600000);
  return pageText(html, query);
}

// Searches, then reads the top pages so Spike answers from real sites, not snippets.
export async function webLookup(query) {
  const results = await search(query);
  await Promise.all(results.slice(0, 4).map(async (r) => {
    r.page = await fetchPage(r.link, query).catch(() => "");
  }));
  return results;
}

// Decides whether the latest message needs a web search and writes the query,
// using the conversation so follow-ups ("and the macros?") keep their topic.
export async function planSearch(env, history, force) {
  const convo = history.slice(-8).map((m) => `${m.role === "user" ? "User" : "Spike"}: ${m.content.slice(0, 600)}`).join("\n");
  const prompt = [
    { role: "system", content: "You write web search queries for an assistant. Read the conversation and look at the LAST user message. " +
      "If answering it needs facts from the web (any specific product, brand, food, nutrition facts, game, person, place, price, score, release, news, statistic, or anything that could be outdated or that you are not 100% sure about), reply with exactly: SEARCH: <query>. " +
      "The query must stand on its own: include the topic from earlier messages (for example 'Quest chips macros nutrition facts', not 'macros'). Never 'correct' names you don't recognize; search them exactly as written. " +
      "Only if it's pure chit-chat, opinion, math, or writing help, reply exactly: NONE." },
    { role: "user", content: convo },
  ];
  let q = "";
  try {
    const r = await env.AI.run(PLANNER, { messages: prompt, max_tokens: 40, temperature: 0 });
    const t = answerText(r).trim();
    const m = t.match(/SEARCH:\s*(.+)/i);
    if (m) q = m[1].replace(/^["']|["']$/g, "").trim();
    else if (/^none/i.test(t) && !force) return "";
  } catch {}
  if (!q && (force || !env.AI)) q = history[history.length - 1].content;
  return q.replace(/\s+/g, " ").slice(0, 150);
}

// ---------- the AI ----------

export function systemPrompt(req, results, today) {
  const persona = req.persona === "custom" ? (req.custom || PERSONAS.chill) : (PERSONAS[req.persona] || PERSONAS.chill);
  const parts = [
    "You are Spike, a friendly AI assistant that lives on a Nintendo 3DS. Your mascot is a little blue sprout.",
    `Today is ${today}.`,
    "Always reply in the same language the user writes in (English or Spanish).",
    "Follow the whole conversation: follow-up questions are about what you were just talking about. If the user corrects you, accept it and use the correction from then on.",
    "Never assume the user made a typo or meant a different product, game or person. If you don't recognize a name, use the web results; if those don't cover it, say you're not sure instead of guessing.",
    "Never make up numbers (nutrition facts, prices, stats, dates). Only give numbers that appear in the web results or that you are certain of.",
    "The screen is small: use plain text only. No markdown, no tables, no headings, no asterisks. Short paragraphs; simple dashes for lists.",
    `Personality: ${persona}`,
  ];
  if (req.project.trim()) parts.push(`About this project (keep this in mind in every answer): ${req.project.trim()}`);
  if (results) {
    if (results.length) {
      parts.push(`Web results found just now (searched: "${results.query || ""}"):\n\n` +
        results.map((r, i) => `[${i + 1}] ${r.title}${r.date ? ` (${r.date})` : ""}\n${r.link}\n${r.page ? `Page text:\n${r.page}` : r.text}`).join("\n\n") +
        "\n\nAnswer from these results first and cite them like [1]. Prefer the official site or the most reliable source when they disagree.");
    } else parts.push("A web search was attempted but found nothing; answer from what you know and say clearly that you couldn't check it online.");
  }
  return parts.join("\n\n");
}

// Removes markdown and characters the 3DS font can't show (they appear as "?" boxes).
export function plain(s) {
  return String(s)
    .replace(/【(\d+)[^】]*】/g, "[$1]").replace(/[【】〖〗]/g, "")
    .replace(/\*\*(.+?)\*\*/g, "$1").replace(/__(.+?)__/g, "$1")
    .replace(/^#{1,6}\s*/gm, "").replace(/^\s*[*•·]\s+/gm, "- ").replace(/`{1,3}/g, "")
    .replace(/[‐-―−]/g, "-")
    .replace(/[‘’‚′]/g, "'").replace(/[“”„″]/g, '"')
    .replace(/…/g, "...").replace(/[•●▪]/g, "-")
    .replace(/→/g, "->").replace(/←/g, "<-").replace(/≈/g, "~").replace(/≤/g, "<=").replace(/≥/g, ">=")
    .replace(/[  -   　]/g, " ")
    .replace(/[​-‍⁠﻿️]/g, "")
    .replace(/[☀-➿]/g, "")
    .replace(/[\u{10000}-\u{10FFFF}]/gu, "")
    .replace(/[ \t]+\n/g, "\n").replace(/\n{3,}/g, "\n\n").trim();
}

// Pulls the text out of the different answer shapes Workers AI models use.
export function answerText(r) {
  if (!r) return "";
  if (typeof r === "string") return r;
  if (typeof r.response === "string") return r.response;
  if (r.response && typeof r.response === "object") return answerText(r.response);
  if (r.result) return answerText(r.result);
  const c = r.choices && r.choices[0];
  if (c) return (c.message && c.message.content) || c.text || "";
  if (Array.isArray(r.output)) {
    return r.output.filter((o) => o.type === "message")
      .flatMap((o) => o.content || []).map((p) => p.text || "").join("");
  }
  if (typeof r.output_text === "string") return r.output_text;
  return "";
}

async function runAI(env, messages) {
  const models = env.MODEL ? [env.MODEL, ...MODELS] : MODELS;
  let last;
  for (const m of models) {
    try {
      const r = await env.AI.run(m, { messages, max_tokens: m.includes("gpt-oss") ? 3000 : 1000, temperature: 0.4 });
      const text = answerText(r);
      if (text && String(text).trim()) return String(text);
      last = new Error("empty answer");
    } catch (e) { last = e; }
  }
  throw last || new Error("no model answered");
}

// ---------- MCP (tools from other services, e.g. Zapier, Firecrawl) ----------
//
// Servers come from Cloudflare variables:
//   FIRECRAWL_KEY   your Firecrawl API key (fc-...)
//   ZAPIER_TOKEN    a connection token from mcp.zapier.com
//   MCP_SERVERS     more servers, one per line: name url [token]

export function mcpServers(env) {
  const list = [];
  if (env.FIRECRAWL_KEY) list.push({ name: "firecrawl", url: `https://mcp.firecrawl.dev/${String(env.FIRECRAWL_KEY).trim()}/v2/mcp` });
  if (env.ZAPIER_TOKEN) list.push({ name: "zapier", url: "https://mcp.zapier.com/api/v1/connect", token: String(env.ZAPIER_TOKEN).trim() });
  for (const line of String(env.MCP_SERVERS || "").split("\n")) {
    const [name, url, token] = line.trim().split(/\s+/);
    if (name && /^https:\/\//.test(url || "")) list.push({ name, url, token });
  }
  return list;
}

// Reads a JSON-RPC answer that may come as plain JSON or as server-sent events.
export function rpcResult(bodyText, id) {
  const tryOne = (t) => { try { const j = JSON.parse(t); return j && (j.id === id || j.result || j.error) ? j : null; } catch { return null; } };
  const direct = tryOne(bodyText.trim());
  if (direct) return direct;
  let found = null;
  for (const block of bodyText.split(/\r?\n\r?\n/)) {
    const data = block.split(/\r?\n/).filter((l) => l.startsWith("data:")).map((l) => l.slice(5).trim()).join("\n");
    const j = data && tryOne(data);
    if (j && j.id === id) found = j;
  }
  return found;
}

class McpClient {
  // (fetch must not be called as a method: Cloudflare throws "Illegal invocation")
  constructor(server, doFetch) { this.s = server; this.f = (...args) => doFetch(...args); this.session = null; this.n = 0; }
  headers() {
    const h = { "Content-Type": "application/json", Accept: "application/json, text/event-stream", "MCP-Protocol-Version": "2025-06-18" };
    if (this.s.token) h.Authorization = `Bearer ${this.s.token}`;
    if (this.session) h["Mcp-Session-Id"] = this.session;
    return h;
  }
  async rpc(method, params, notify = false) {
    const id = notify ? undefined : ++this.n;
    const res = await this.f(this.s.url, {
      method: "POST", headers: this.headers(),
      body: JSON.stringify(notify ? { jsonrpc: "2.0", method, params } : { jsonrpc: "2.0", id, method, params }),
      signal: AbortSignal.timeout(25000),
    });
    const sid = res.headers.get("mcp-session-id");
    if (sid) this.session = sid;
    const t = await res.text();
    if (notify) return null;
    if (!res.ok) throw new Error(`${this.s.name} ${res.status} ${t.slice(0, 80)}`);
    const j = rpcResult(t, id);
    if (!j) throw new Error(`${this.s.name}: unreadable answer`);
    if (j.error) throw new Error(`${this.s.name}: ${j.error.message || "error"}`);
    return j.result;
  }
  async start() {
    await this.rpc("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "spike-ai", version: "1.0" } });
    await this.rpc("notifications/initialized", {}, true).catch(() => {});
  }
  async tools() {
    const out = [];
    let cursor;
    for (let i = 0; i < 5; i++) {
      const r = await this.rpc("tools/list", cursor ? { cursor } : {});
      out.push(...(r.tools || []));
      cursor = r.nextCursor;
      if (!cursor) break;
    }
    return out;
  }
  async call(name, args) {
    const r = await this.rpc("tools/call", { name, arguments: args || {} });
    const text = (r.content || []).map((c) => c.type === "text" ? c.text : c.type === "resource" ? (c.resource?.text || "") : `[${c.type}]`).join("\n");
    return { error: !!r.isError, text: (text || JSON.stringify(r.structuredContent || {})).slice(0, 9000) };
  }
}

const safeName = (s) => String(s).replace(/[^a-zA-Z0-9_]/g, "_").slice(0, 60);

// Connects to every server and returns their tools for Gemini.
export async function mcpConnect(env, doFetch = fetch) {
  const servers = mcpServers(env);
  const tools = [], errors = [];
  await Promise.all(servers.map(async (srv) => {
    const c = new McpClient(srv, doFetch);
    try {
      await c.start();
      for (const t of (await c.tools()).slice(0, 40)) {
        const fn = safeName(`${srv.name}__${t.name}`);
        tools.push({ fn, server: srv.name, name: t.name, client: c, description: String(t.description || "").slice(0, 500), schema: t.inputSchema || { type: "object", properties: {} } });
      }
    } catch (e) { errors.push(`${srv.name}: ${e.message}`.slice(0, 120)); }
  }));
  return { tools, errors, servers };
}

// ---------- Gemini (smarter, with Google Search built in) ----------

// Free-tier models first (checked with /v1/gemini-check): the newest "flash"
// models answer 429 (paid only) on a free key.
const GEMINI_MODELS = ["gemini-3-flash-preview", "gemini-3.6-flash", "gemini-3.5-flash-lite", "gemini-flash-lite-latest", "gemini-3.1-flash-lite"];

// Google Search inside Gemini is paid on some free keys; once it fails we do
// our own web search instead (remembered while this worker stays warm).
let groundingOff = false;

export async function gemini(req, history, env, today, doFetch = fetch, lookup = webLookup) {
  const search = req.web || req.persona === "searcher";
  const contents = history.map((m) => ({
    role: m.role === "user" ? "user" : "model",
    parts: [...(m.images || []).map((i) => ({ inline_data: { mime_type: i.mime, data: i.data } })), { text: m.content || "." }],
  }));
  const models = env.GEMINI_MODEL ? [env.GEMINI_MODEL, ...GEMINI_MODELS] : GEMINI_MODELS;
  const errs = [];

  async function ask(m, sys, tools) {
    const body = { systemInstruction: { parts: [{ text: sys }] }, contents, generationConfig: { maxOutputTokens: 2048, temperature: 0.5 } };
    if (tools) body.tools = [{ google_search: {} }];
    const res = await doFetch(`https://generativelanguage.googleapis.com/v1beta/models/${m}:generateContent`, {
      method: "POST",
      headers: { "Content-Type": "application/json", "x-goog-api-key": env.GEMINI_KEY },
      body: JSON.stringify(body),
    });
    const j = await res.json().catch(() => ({}));
    if (!res.ok) return { status: res.status, err: `${m}: ${res.status} ${(j.error?.message || "").slice(0, 90)}`, msg: j.error?.message || "" };
    const c = j.candidates && j.candidates[0];
    const text = (c?.content?.parts || []).map((p) => p.text || "").join("");
    if (!text.trim()) return { status: 0, err: `${m}: empty answer (${c?.finishReason || "?"})` };
    const seen = new Set(), sources = [];
    for (const g of c.groundingMetadata?.groundingChunks || []) {
      const w = g.web;
      if (!w || !w.uri || seen.has(w.title || w.uri)) continue;
      seen.add(w.title || w.uri);
      sources.push({ title: w.title || w.uri, link: w.uri });
      if (sources.length >= 6) break;
    }
    return { text, sources };
  }

  // 0) Gemini with MCP tools (Zapier, Firecrawl, ...)
  if (req.mcp && mcpServers(env).length) {
    const r = await geminiWithTools(req, contents, env, today, models, doFetch, errs);
    if (r) return r;
  }

  // 1) Gemini with Google Search built in
  if (search && !groundingOff) {
    const sys = systemPrompt(req, null, today) +
      "\n\nYou can use Google Search. Search before answering anything about a specific product, brand, food or nutrition facts, game, person, place, price, score, release or news, or anything you aren't sure of. Base numbers on what you find. Don't write citation numbers; sources are shown under your answer." +
      (req.persona === "searcher" ? " Always search, for every question." : "");
    try {
      const r = await ask(models[0], sys, true);
      if (r.text) return r;
      errs.push(r.err);
      if (r.status === 429 || r.status === 400) groundingOff = true;
      if (r.status === 403 || /api key/i.test(r.msg || "")) throw new Error(r.err);
    } catch (e) { if (!errs.length) errs.push(e.message); }
  }

  // 2) plain Gemini, with our own web search results in the instructions
  let results = null;
  if (search) {
    const force = req.persona === "searcher";
    let q = env.AI ? await planSearch(env, history, force).catch(() => "") : history[history.length - 1].content.slice(0, 150);
    if (!q && force) q = history[history.length - 1].content.slice(0, 150);
    if (q.trim()) {
      results = await lookup(q).catch(() => []);
      results.query = q;
    }
  }
  const sys = systemPrompt(req, results, today);
  for (const m of models) {
    try {
      const r = await ask(m, sys, false);
      if (r.text) return { text: r.text, sources: (results || []).map((x) => ({ title: x.title, link: x.link })) };
      errs.push(r.err);
      if (r.status === 403 || (r.status === 400 && /api key/i.test(r.msg || ""))) break;
    } catch (e) { errs.push(`${m}: ${e.message}`); }
  }
  // the first error is usually the telling one (later models are just fallbacks)
  throw new Error(`Gemini didn't answer. ${errs.slice(-2).join(" | ")}`.slice(0, 230));
}

// Lets Gemini call MCP tools: ask, run the tools it picks, give it the results, repeat.
async function geminiWithTools(req, contents, env, today, models, doFetch, errs) {
  const { tools, errors } = await mcpConnect(env, doFetch);
  if (errors.length) errs.push(...errors);
  if (!tools.length) return null;
  const sys = systemPrompt(req, null, today) +
    "\n\nYou have tools from connected services (" + [...new Set(tools.map((t) => t.server))].join(", ") + "). Use them when they help: web search/scraping for facts, Zapier for the user's apps. " +
    "Before any tool that sends, posts, buys, deletes or changes something (email, messages, calendar events, files), first tell the user exactly what you'll do and only do it after they say yes in their next message. Reading and searching don't need confirmation. Don't write citation numbers.";
  const decl = tools.map((t) => ({ name: t.fn, description: `[${t.server}] ${t.description}`, parametersJsonSchema: t.schema }));
  const convo = [...contents];
  const used = [];
  for (const m of models.slice(0, 3)) {
    try {
      for (let round = 0; round < 5; round++) {
        const res = await doFetch(`https://generativelanguage.googleapis.com/v1beta/models/${m}:generateContent`, {
          method: "POST",
          headers: { "Content-Type": "application/json", "x-goog-api-key": env.GEMINI_KEY },
          body: JSON.stringify({ systemInstruction: { parts: [{ text: sys }] }, contents: convo, tools: [{ functionDeclarations: decl }], generationConfig: { maxOutputTokens: 2048, temperature: 0.4 } }),
        });
        const j = await res.json().catch(() => ({}));
        if (!res.ok) { errs.push(`${m}+tools: ${res.status} ${(j.error?.message || "").slice(0, 90)}`); break; }
        const parts = j.candidates?.[0]?.content?.parts || [];
        const calls = parts.filter((p) => p.functionCall);
        if (!calls.length) {
          const text = parts.map((p) => p.text || "").join("");
          if (text.trim()) return { text, sources: used.map((u) => ({ title: `Used ${u}`, link: "" })) };
          errs.push(`${m}+tools: empty answer`);
          break;
        }
        convo.push({ role: "model", parts });
        const answers = await Promise.all(calls.map(async ({ functionCall: fc }) => {
          const t = tools.find((x) => x.fn === fc.name);
          let response;
          if (!t) response = { error: "unknown tool" };
          else {
            used.push(`${t.server}: ${t.name}`);
            try { const r = await t.client.call(t.name, fc.args); response = r.error ? { error: r.text } : { result: r.text }; }
            catch (e) { response = { error: e.message }; }
          }
          return { functionResponse: { name: fc.name, response } };
        }));
        convo.push({ role: "user", parts: answers });
      }
    } catch (e) { errs.push(`${m}+tools: ${e.message}`); }
    convo.length = contents.length; // start over with the next model
    used.length = 0;
  }
  return null;
}

// For the app's MCP screen: which servers are set up and what tools they have.
export async function mcpStatus(env, doFetch = fetch) {
  const servers = mcpServers(env);
  if (!servers.length) return "OK\n";
  const { tools, errors } = await mcpConnect(env, doFetch);
  const lines = ["OK"];
  for (const srv of servers) {
    const mine = tools.filter((t) => t.server === srv.name);
    const err = errors.find((e) => e.startsWith(srv.name + ":"));
    lines.push(`S\t${esc(srv.name)}\t${err ? "ERR" : mine.length}\t${esc(err ? err.slice(srv.name.length + 2) : mine.map((t) => t.name).join(", "))}`);
  }
  return lines.join("\n") + "\n";
}

export function resetGrounding() { groundingOff = false; }

// ---------- voice ----------

export async function transcribe(env, wavB64, doFetch = fetch) {
  if (env.AI) {
    try {
      const r = await env.AI.run("@cf/openai/whisper-large-v3-turbo", { audio: wavB64 });
      const t = (r && (r.text || r.result?.text) || "").trim();
      if (t) return t;
    } catch {}
  }
  if (env.GEMINI_KEY) {
    for (const m of GEMINI_MODELS) {
      const res = await doFetch(`https://generativelanguage.googleapis.com/v1beta/models/${m}:generateContent`, {
        method: "POST",
        headers: { "Content-Type": "application/json", "x-goog-api-key": env.GEMINI_KEY },
        body: JSON.stringify({ contents: [{ role: "user", parts: [
          { inline_data: { mime_type: "audio/wav", data: wavB64 } },
          { text: "Transcribe this recording exactly, in the language it is spoken in (English or Spanish). Output only the words, nothing else." },
        ] }] }),
      }).catch(() => null);
      if (!res || !res.ok) continue;
      const j = await res.json().catch(() => ({}));
      const t = (j.candidates?.[0]?.content?.parts || []).map((p) => p.text || "").join("").trim();
      if (t) return t;
    }
  }
  return "";
}

const PHOTO_ONLY = "(I sent you this picture without a question. Tell me what it is and anything useful about it.)";

export async function chat(body, env, today) {
  const req = parseRequest(body);
  const history = req.messages.slice(-16);
  if (!history.length || history[history.length - 1].role !== "user") return "ERR\tNothing to answer.\n";
  const lastMsg = history[history.length - 1];
  const extra = [];
  if (lastMsg.voice) {
    const t = await transcribe(env, lastMsg.voice);
    if (!t) return "ERR\tI couldn't make out what you said. Try again a bit closer to the mic.\n";
    lastMsg.content = t;
    extra.push(`T\t${esc(t)}`);
  }
  for (const m of history) if (m.role === "user" && !m.content.trim() && m.images && m.images.length) m.content = PHOTO_ONLY;
  const hasPhoto = !!(lastMsg.images && lastMsg.images.length);
  const out = (answer, sources) => ["OK", ...extra, `A\t${esc(answer)}`, ...sources.map((r) => `S\t${esc(r.title)}\t${esc(r.link)}`)].join("\n") + "\n";
  if (env.GEMINI_KEY) {
    try {
      const g = await gemini(req, history, env, today);
      return out(plain(g.text), g.sources);
    } catch (e) {
      if (!env.AI || hasPhoto) throw e;
      // daily limit reached or Gemini down: fall back to Cloudflare's own AI below
    }
  }
  if (hasPhoto) return "ERR\tPhotos need Gemini. Add a GEMINI_KEY to your relay (see the setup steps).\n";
  const force = req.persona === "searcher";
  let results = null;
  if (req.web || force) {
    const q = await planSearch(env, history, force);
    if (q) {
      results = await webLookup(q).catch(() => []);
      results.query = q;
    }
  }
  const flat = history.map((m) => ({ role: m.role, content: m.images && m.images.length ? `${m.content}\n(There was a photo here that you can't see.)` : m.content }));
  const messages = [{ role: "system", content: systemPrompt(req, results, today) }, ...flat];
  const answer = plain(await runAI(env, messages));
  return out(answer, results || []);
}

// Open /v1/gemini-check in a browser: tries a tiny question on each Gemini
// model and shows which ones your key can use for free.
async function geminiCheck(env) {
  if (!env.GEMINI_KEY) return "No GEMINI_KEY on this relay.\n";
  const lines = ["Gemini models for your key:", ""];
  let names = [];
  try {
    const r = await fetch("https://generativelanguage.googleapis.com/v1beta/models?pageSize=100", { headers: { "x-goog-api-key": env.GEMINI_KEY } });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) lines.push(`(listing models failed: ${r.status} ${j.error?.message || ""})`);
    names = (j.models || []).map((m) => String(m.name || "").replace("models/", ""))
      .filter((n) => /^gemini-[\d.]+-(flash|pro)(-lite)?$|latest$/.test(n));
  } catch (e) { lines.push(`(listing models failed: ${e.message})`); }
  const want = [...new Set([...(env.GEMINI_MODEL ? [env.GEMINI_MODEL] : []), ...GEMINI_MODELS, ...names])].slice(0, 12);
  for (const m of want) {
    try {
      const r = await fetch(`https://generativelanguage.googleapis.com/v1beta/models/${m}:generateContent`, {
        method: "POST", headers: { "Content-Type": "application/json", "x-goog-api-key": env.GEMINI_KEY },
        body: JSON.stringify({ contents: [{ role: "user", parts: [{ text: "Say ok" }] }], generationConfig: { maxOutputTokens: 5 } }),
      });
      const j = await r.json().catch(() => ({}));
      lines.push(r.ok ? `WORKS  ${m}` : `no     ${m}  (${r.status} ${String(j.error?.message || "").slice(0, 70)})`);
    } catch (e) { lines.push(`no     ${m}  (${e.message})`); }
  }
  lines.push("", "Models on this key: " + (names.join(", ") || "?"));
  return lines.join("\n") + "\n";
}

// ---------- entry ----------

function text(body, status = 200) {
  return new Response(body, { status, headers: { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" } });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/" || url.pathname === "/v1/ping") {
      return text(`SPIKE OK\t${env.AI || env.GEMINI_KEY ? "ai" : "no-ai"}\t${env.GEMINI_KEY ? "gemini" : "cloudflare"}\n`);
    }
    if (url.pathname === "/v1/mcp") {
      try { return text(await mcpStatus(env)); } catch (e) { return text(`ERR\t${esc(e.message)}\n`); }
    }
    if (url.pathname === "/v1/gemini-check") {
      try { return text(await geminiCheck(env)); } catch (e) { return text(`Check failed: ${e.message}\n`); }
    }
    const body = request.method === "POST" ? await request.text() : "";
    if (!env.AI && !env.GEMINI_KEY) return text("ERR\tNo AI set up yet: add the Workers AI binding named AI, or a GEMINI_KEY secret.\n");
    try {
      if (url.pathname === "/v1/chat" && request.method === "POST") {
        const today = new Date().toISOString().slice(0, 10);
        return text(await chat(body, env, today));
      }
      return text("ERR\tUnknown request\n", 404);
    } catch (e) {
      return text(`ERR\t${esc(String(e && e.message || e)).slice(0, 180)}\n`);
    }
  },
};
