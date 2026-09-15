#!/usr/bin/env python3
"""gen_system_prompt.py — build the ask-hop system prompt from rana-socket.toml.

rana-socketd owns the action vocabulary and the re-dispatch router, so the
system prompt must be derived from the daemon's own [commands] table — not
hardcoded in rana-ask.sh. This script reads the TOML config (path from env
RANA_CONFIG or argv[1]) and emits the routing system prompt on stdout.

It uses only the standard library (tomllib), which is why python3 is now part
of the rana-socketd runtime image.
"""
import os
import sys
import tomllib


def main() -> int:
    path = os.environ.get("RANA_CONFIG") or (sys.argv[1] if len(sys.argv) > 1 else "")
    if not path:
        print("gen_system_prompt.py: RANA_CONFIG not set", file=sys.stderr)
        return 1

    with open(path, "rb") as fh:
        cfg = tomllib.load(fh)

    cmds = cfg.get("commands", {})
    has = lambda k: k in cmds  # noqa: E731

    lines = [
        'You are a strict voice command routing engine. Output ONLY raw JSON: '
        '{"action":"ACTION","payload":"TEXT"}. No markdown blocks, no filler.',
        "",
        "# NOTE: this action vocabulary is wired to the rana-socketd daemon "
        "(SpeakCmd=reply, LaunchBrowserCmd=open_browser/search_web, "
        "PowerCmd=shutdown, LightCmd=toggle_lights).",
        "",
        '"action" rules:',
    ]

    if has("speak"):
        lines.append(
            '- "reply": USE THIS FOR ALL GENERAL KNOWLEDGE, trivia, definitions, '
            "math, and historical facts.")
    if has("browser"):
        lines.append(
            '- "search_web": DO NOT use for general knowledge. Use ONLY for '
            "real-time/time-sensitive live data (current weather, breaking news, "
            "live sports scores, current stock prices).")
        lines.append(
            '- "open_browser": open the default web browser (device trigger; '
            'payload must be "").')
    if has("lights"):
        lines.append(
            '- "toggle_lights": toggle the lights (device trigger; payload must '
            'be "").')
    if has("power"):
        lines.append(
            '- "shutdown": shut down the machine (device trigger; payload must '
            'be "").')

    lines.append(
        'If the request fits none of the above, use "reply" with a short spoken '
        "answer.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
