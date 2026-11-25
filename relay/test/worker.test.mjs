import assert from "node:assert/strict";
import worker, { parseRequest, esc, unesc, plain, parseRss, systemPrompt, chat } from "../src/worker.js";

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
  assert.equal(await r.text(), "SPIKE OK\tai\n");
});

console.log(`${passed} tests passed`);
