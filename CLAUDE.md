# CLAUDE.md

## What this is
A single self-contained `index.html` setup guide for the **Arduino Uno Q**, served via GitHub Pages.
No build step, no dependencies — all CSS/JS is inline in `index.html`. `README.md` mirrors the steps.

## Deploy
- **Live:** https://mikersays.github.io/uno-q-setup/
- GitHub Pages source: branch **`master`**, path **`/`** (root). The repo is **private**, but the Pages
  site is **publicly reachable** (verified 200 unauthenticated).
- **Editing `index.html` and pushing to `master` republishes** — Pages rebuilds automatically (~1–2 min).
  Check build state: `gh api repos/mikersays/uno-q-setup/pages --jq '.status'` (want `built`).
- `.nojekyll` is present to skip Jekyll processing.

## Local preview
```bash
python3 -m http.server 8080   # then open http://localhost:8080
```
Quick visual check headlessly: `google-chrome --headless=new --screenshot=out.png --window-size=1280,7200 "file://$PWD/index.html"`

## Guide structure (5 steps in `<main>`, each `<section class="step" data-accent="...">`)
1. App Lab setup (blue) — incl. first-install **update dialogue**
2. Reboot (purple) — `sudo reboot`, **required** before Tailscale or it won't install
3. Tailscale SSH (green)
4. Claude Code + `cc` alias (orange)
5. Cloudflare Tunnel (red)
Styling is one global stylesheet; sections use shared classes (`.step`, `.substep`, `.code-block`,
`.note`, `.warning`, `.kv-table`) with per-step accent via `data-accent` attribute selectors. Keep
sections style-free (no inline styles) so the theme stays consistent.

## Caveat — verify before trusting
Some board-specific commands and the exact App Lab flow are best-effort from research and **not fully
verified on real Uno Q hardware**. Sanity-check against an actual board before treating them as
confirmed. Confirmed corrections so far (from the board owner):
- default user is `arduino`
- **SSH is enabled by default** (no `openssh-server` install needed)
- App Lab first-install update requires a reboot before Tailscale will install
- Tailscale: **no `--ssh` flag** — use plain `sudo tailscale up`, then connect with normal
  `ssh arduino@<magicdns-or-100.x.y.z>` over the tailnet (don't use the `tailscale ssh` command)
- Claude Code install is the **native installer** `curl -fsSL https://claude.ai/install.sh | bash`
  (the `npm install -g @anthropic-ai/claude-code` method is deprecated); binary lands in `~/.local/bin`

## Domain facts
Board OS is Debian; default user is `arduino` (all SSH examples use `arduino@`). See recalled memory
for the update→reboot→Tailscale dependency rationale.
