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

## Set up the app

1. Copy `SpikeAI.3dsx` to `sdmc:/3ds/` and open it from the Homebrew Launcher.
2. It opens on Settings the first time: enter the relay address, then pick **Test connection**.

Chats, projects and settings are saved in `sdmc:/3ds/SpikeAI/`.

## Controls

| Where | Button | Does |
|---|---|---|
| Chat | A | Type a message |
| Chat | R | Try again after an error |
| Chat | X | New chat |
| Chat | Y | Web lookups on/off |
| Chat | L | Change personality |
| Chat | B | Chats and projects |
| Chat | SELECT | Settings |
| Lists | A / X / Y (twice) | Open / rename / delete |
| Lists | L / R | Switch Chats and Projects |
| Anywhere | START | Exit |

## Build

```
cd app
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make
```

Relay tests: `node relay/test/worker.test.mjs`
