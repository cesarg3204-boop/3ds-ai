// Spike AI relay: a Cloudflare Worker that does the AI work for the 3DS app.
//
// The 3DS sends a chat in a simple line format (no JSON on the 3DS side); the
// relay adds Spike's personality, optionally looks things up on the web, asks
// Cloudflare Workers AI (free daily allowance) and sends back plain text.
//
// Setup (Cloudflare dashboard): Workers AI binding named AI. Optional variable MODEL.

const MODELS = [
  "@cf/meta/llama-3.3-70b-instruct-fp8-fast",
  "@cf/meta/llama-3.1-8b-instruct-fast",
  "@cf/meta/llama-3.1-8b-instruct",
];

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
    else if (k === "U") req.messages.push({ role: "user", content: v });
    else if (k === "A") req.messages.push({ role: "assistant", content: v });
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

const SHOPS = /(amazon|walmart|target|ebay|bestbuy|kroger|costco)\./i;

export async function webLookup(q) {
  const query = q.replace(/\s+/g, " ").trim().slice(0, 120);
  const enc = encodeURIComponent(query);
  const [news, web] = await Promise.all([
    getText(`https://www.bing.com/news/search?q=${enc}&format=rss&setlang=en-US`).then(parseRss).catch(() => []),
    getText(`https://www.bing.com/search?q=${enc}&format=rss&setlang=en-US`).then(parseRss).catch(() => []),
  ]);
  const out = [], seen = new Set();
  // Mix: news first (fresh), then web pages; skip shop pages.
  for (const r of [...news.slice(0, 4), ...web]) {
    if (out.length >= 6) break;
    if (seen.has(r.link) || SHOPS.test(r.link)) continue;
    seen.add(r.link);
    out.push(r);
  }
  return out;
}

// ---------- the AI ----------

export function systemPrompt(req, results, today) {
  const persona = req.persona === "custom" ? (req.custom || PERSONAS.chill) : (PERSONAS[req.persona] || PERSONAS.chill);
  const parts = [
    "You are Spike, a friendly AI assistant that lives on a Nintendo 3DS. Your mascot is a little blue sprout.",
    `Today is ${today}.`,
    "Always reply in the same language the user writes in (English or Spanish).",
    "The screen is small: use plain text only. No markdown, no tables, no headings, no asterisks. Short paragraphs; simple dashes for lists.",
    `Personality: ${persona}`,
  ];
  if (req.project.trim()) parts.push(`About this project (keep this in mind in every answer): ${req.project.trim()}`);
  if (results) {
    if (results.length) {
      parts.push("Web results found just now for the user's latest message:\n" +
        results.map((r, i) => `[${i + 1}] ${r.title}${r.date ? ` (${r.date})` : ""}\n${r.text}`).join("\n\n") +
        "\nUse them for current facts and cite them like [1].");
    } else parts.push("A web search was attempted but found nothing; answer from what you know and say it may be out of date.");
  }
  return parts.join("\n\n");
}

// Removes markdown the 3DS can't show.
export function plain(s) {
  return String(s)
    .replace(/\*\*(.+?)\*\*/g, "$1").replace(/__(.+?)__/g, "$1")
    .replace(/^#{1,6}\s*/gm, "").replace(/^\s*[*•]\s+/gm, "- ").replace(/`{1,3}/g, "")
    .replace(/\n{3,}/g, "\n\n").trim();
}

async function runAI(env, messages) {
  const models = env.MODEL ? [env.MODEL, ...MODELS] : MODELS;
  let last;
  for (const m of models) {
    try {
      const r = await env.AI.run(m, { messages, max_tokens: 900, temperature: 0.6 });
      const text = typeof r === "string" ? r : r && (r.response ?? r.result?.response);
      if (text && String(text).trim()) return String(text);
      last = new Error("empty answer");
    } catch (e) { last = e; }
  }
  throw last || new Error("no model answered");
}

export async function chat(body, env, today) {
  const req = parseRequest(body);
  const history = req.messages.slice(-16);
  if (!history.length || history[history.length - 1].role !== "user") return "ERR\tNothing to answer.\n";
  const wantWeb = req.web || req.persona === "searcher";
  const results = wantWeb ? await webLookup(history[history.length - 1].content).catch(() => []) : null;
  const messages = [{ role: "system", content: systemPrompt(req, results, today) }, ...history];
  const answer = plain(await runAI(env, messages));
  const lines = ["OK", `A\t${esc(answer)}`];
  for (const r of results || []) lines.push(`S\t${esc(r.title)}\t${esc(r.link)}`);
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
      return text(`SPIKE OK\t${env.AI ? "ai" : "no-ai"}\n`);
    }
    const body = request.method === "POST" ? await request.text() : "";
    if (!env.AI) return text("ERR\tWorkers AI isn't turned on for this relay yet (binding named AI).\n");
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
