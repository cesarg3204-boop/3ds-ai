# Spike AI

A chat assistant for the Nintendo 3DS, with a little blue sprout named Spike.

- `app/` is the 3DS homebrew app (devkitARM, citro2d, curl).
- `relay/` is a Cloudflare Worker that does the AI part with Cloudflare's free Workers AI.

## Set up the relay (once, about 5 minutes)

1. Go to dash.cloudflare.com, then **Workers & Pages**, **Create**, **Create Worker**. Name it `spike-ai` and press **Deploy**.
2. Press **Edit code**, delete what's there, paste all of `relay/src/worker.js`, and press **Deploy**.
3. In the worker, open **Settings**, **Bindings**, **Add binding**, choose **Workers AI**, and name it `AI`. Save.
4. (Smarter Spike, free) Get an API key at aistudio.google.com (Get API key, Create API key). In the worker's **Settings**, **Variables and Secrets**, add a **Secret** named `GEMINI_KEY` with that key. Spike then uses Gemini with Google Search, and falls back to Cloudflare's AI if Gemini's daily limit runs out.
5. Open `https://spike-ai.<your-subdomain>.workers.dev/v1/ping` in a browser. It should say `SPIKE OK  ai`.

## MCP tools (optional)

Spike can use tools from MCP servers through the relay. Add these as variables (or secrets) in the worker's Settings:

- `FIRECRAWL_KEY`: your Firecrawl API key (fc-...) from firecrawl.dev, for searching and reading websites.
- `ZAPIER_TOKEN`: a connection token from mcp.zapier.com (New MCP Server, Other client, add tools, Connect, generate token), for your apps.
- `MCP_SERVERS`: any other servers, one per line: `name https://url [token]`.

To use a server, tag it in your message: `@zapier send me a WhatsApp saying hi`, `@firecrawl what's on nintendo.com?`, or `@all` for every server. Without an `@`, Spike doesn't use your tools. Settings, MCP tools shows what's connected (tap a server to start a message with its tag). Spike asks before sending or changing anything.

## Skills

Start a message with `/name` to use a skill, like `/truth is my plan good?`. Skills are .txt files in `sdmc:/3ds/SpikeAI/skills`: the first line says what the skill does, the rest is instructions Spike follows. The app adds three starter skills, adapted from Claude skills: `/truth` (honest answers, labels guesses), `/research` (deeper research across sources) and `/manual` (explains a topic like a short course manual, in Spanish style). Settings, Skills lists them; tap one to start a message with it.

## Set up the app

1. Copy `SpikeAI.3dsx` to `sdmc:/3ds/` and open it from the Homebrew Launcher.
2. It opens on Settings the first time: enter the relay address, then pick **Test connection**.

Chats, projects and settings are saved in `sdmc:/3ds/SpikeAI/`.

## Controls

| Where | Button | Does |
|---|---|---|
| Chat | D-Pad | Move the highlight between buttons (Photo, Attach...) |
| Chat | A | Press the highlighted button (Type a message by default) |
| Chat | Circle Pad | Scroll the chat |
| Chat | R | Talk (record your voice) |
| Chat | A (after an error) | Try again |
| Chat | X | New chat |
| Chat | Y | Web lookups on/off |
| Chat | L | Change personality |
| Chat | B | Chats and projects |
| Chat | SELECT | Settings |
| Lists | A / X / Y (twice) | Open / rename / delete |
| Lists | L / R | Switch Chats and Projects |
| Anywhere | START | Exit |

## Install as a .cia (Home Menu icon)

Install `SpikeAI.cia` with FBI (on a real 3DS) or File > Install CIA (in an emulator). It shows up on the Home Menu like a normal app; chats and settings are the same files as the .3dsx version. It has no animated banner yet (the top screen stays plain when you pick it).

## Build

```
cd app
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make
```

Relay tests: `node relay/test/worker.test.mjs`
