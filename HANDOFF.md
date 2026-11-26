# Spike AI: handoff (2026-11-26)

Paste this into a new chat to continue.

## What Spike is
Spike AI is an AI assistant app for the Nintendo 3DS. It has two parts:
- **The 3DS app**, written in C with libctru, citro2d, curl and mbedTLS.
- **The relay**, a Cloudflare Worker at https://spike-ai.cesarg3204.workers.dev that the app talks to.

**Repo:** `cesarg3204-boop/3ds-ai` (public). All work is on branch `claude/project-thread-gb0su1`.
- `app/`: the 3DS app (`source/main.c`, `media.c`, `net.c`, `gfx.c`; `romfs/cacert.pem`)
- `app/cia/app.rsf` and `build_cia.sh`: CIA packaging
- `relay/src/worker.js`: the relay. Tests run with `node relay/test/worker.test.mjs` (28 passing). Always check the exit code, because piping through `tail` hides failures.
- `dist/SpikeAI.cia`: the CIA that FBI QR installs download
- Deliverables are also in `/mnt/project-files/spike/`: SpikeAI.3dsx, SpikeAI.cia, worker.js, SETUP.md, QR PNGs.

## About the user
- Prefers short, informal replies. Timezone is America/Panama.
- Likes being asked one question at a time before a big build.
- Gets frustrated by misreads: read their message carefully and answer the actual question.
- Is nervous about harming their real 3DS. Reassure them: Spike only writes to `sdmc:/3ds/SpikeAI/`, and holding power to force it off is safe.
- Deploys the relay by copying `worker.js` from the GitHub blob page (copy icon) into the Cloudflare dashboard. The raw link caches for about 5 minutes; a commit-specific raw URL avoids that.

## How to build
- **App:** in `app/`, run `docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make`. If dockerd isn't running, start it with `nohup dockerd > /tmp/dockerd.log 2>&1 &`.
- **CIA:** build makerom from https://github.com/3DSGuy/Project_CTR (`makerom/` folder, `make`), then run:
  `makerom -f cia -o SpikeAI.cia -elf SpikeAI.elf -rsf cia/app.rsf -icon SpikeAI.smdh -target t -exefslogo`
- **Check every CIA before sending it.** Build ctrtool from the same repo and run `ctrtool -i SpikeAI.cia`. Confirm it shows `IoMapping 0x1FF00000-0x1FF7FFFF` and `StaticMapping 0x1F000000-0x1F5FFFFF:r`. Without those, the app froze on the Homebrew logo on a real 3DS. That's fixed in commit after 06f29c9.
- **FBI QR code:** commit `dist/SpikeAI.cia`, push, then make a QR (python `segno`) of `https://raw.githubusercontent.com/cesarg3204-boop/3ds-ai/<commit>/dist/SpikeAI.cia`.

## Relay (worker.js)
- **Models:** Gemini first (`GEMINI_KEY`), using free models gemini-3-flash-preview, 3.6-flash, 3.5-flash-lite, flash-lite-latest and 3.1-flash-lite. If those fail, it falls back to Cloudflare Workers AI (gpt-oss-120b, then llama).
  - Google Search grounding is paid on this key, so the relay uses its own Bing search plus page reading instead.
- **Cloudflare variables:** `AI` binding, `GEMINI_KEY`, `FIRECRAWL_KEY`, `ZAPIER_TOKEN`, `MCP_SERVERS` (one per line: `name url [token]`), and optional `MODEL` / `GEMINI_MODEL`. There is no password.
- **MCP tools:** only used when the message tags a server, like `@zapier`, `@firecrawl` or `@all`.
  - It tries every Gemini model with the tools. If none work (429 limit or 503 busy), Cloudflare llama-3.3-70b runs the tools instead.
  - **Zapier WhatsApp works:** the user confirmed it.
- **Skills:** a `Z\tname\tinstructions` line gets added to the system prompt.
- **Answers:** citation numbers like [1] and source links are removed. Only notes about failures are shown (blue lines with an empty link).
- **Routes:** `/v1/ping`, `/v1/chat`, `/v1/mcp`, `/v1/gemini-check`.

## App features
- Chats, projects, settings, personalities (L button), and web on/off (Y).
- Talk (R, mic), Photo (camera), and Attach (JPG/PNG/TXT from the SD card).
- **Controls:** the D-Pad moves a white highlight between the buttons, A presses it, and the Circle Pad scrolls. X starts a new chat, B opens Chats, SELECT opens Settings.
- **Skills:** `.txt` files in `sdmc:/3ds/SpikeAI/skills/`. The first line is the description and the rest is the instructions. Typing `/name` at the start of a message uses one.
  - Starter skills: `/truth`, `/research`, `/manual` (adapted from the user's Claude skills).
- **Settings:** has MCP tools (@) and Skills (/) screens. Tapping a server or skill pre-fills the keyboard with it.
- **Start-up log:** each start-up step prints on the top screen and is saved to `sdmc:/3ds/SpikeAI/boot.txt`.

## Open items
1. **The real 3DS still hasn't fully confirmed it works.** After the RSF fix, the user saw the white-on-black start-up text, which means Spike's code now runs. Still unknown: does it reach the app, or freeze on a step? Ask for the last line on the top screen or the contents of `boot.txt`.
2. **No banner in the CIA yet.** The original bannertool repo (Steveice10) is gone. Cloning the fork `diasurgical/bannertool` was blocked by the auto-mode safety check. The user hasn't answered yet; if they say "ok use diasurgical bannertool", try again. The fallback is writing a small banner generator ourselves.
3. **Memory:** the user is turning on Claude memory in Settings.
4. **Gemini free limits** reset at 3 AM Panama time; Cloudflare's reset at 7 PM Panama time.
