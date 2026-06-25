# Arduino Uno Q — Full Setup Guide

A single-page, copy-paste setup guide for the **Arduino Uno Q**, published via GitHub Pages.

It walks through, in order:

1. **Set up the Uno Q via Arduino App Lab** — connect over USB-C, accept the first-install update dialogue, set credentials (user `arduino`), join Wi-Fi, enable SSH.
2. **Reboot to finish the update** — `sudo reboot`. Required: the App Lab update only finishes after a restart, and Tailscale won't install without it.
3. **SSH from anywhere with Tailscale** — free account, `tailscale up --ssh`, then `tailscale ssh arduino@<host>` from any device.
4. **Install Claude Code + the `cc` alias** — `npm i -g @anthropic-ai/claude-code`, PATH fix if `claude` isn't found, and `alias cc='claude --dangerously-skip-permissions --remote-control'`.
5. **Go public with a Cloudflare Tunnel** — quick `trycloudflare.com` tunnel or a persistent named tunnel.

The whole guide is `index.html` — no build step, no dependencies.

## Local preview

```bash
python3 -m http.server 8080
# open http://localhost:8080
```

> Not affiliated with Arduino, Tailscale, Anthropic, or Cloudflare.
