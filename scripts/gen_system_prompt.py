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

    # The router's canonical keyword per config key — must stay in sync with
    # canonical_action() in executor.cpp. Used so the LLM emits the keyword the
    # daemon's dispatch_inner() expects (never the FlatBuffer variant name).
    CANON = {
        "speak": "reply",
        "browser": "open_browser",
        "lights": "toggle_lights",
        "power": "shutdown",
        "mc_server": "open_minecraft",
        "volume": "set_volume",
        "media": "media_control",
    }

    lines = [
        'You are a strict voice command routing engine. Output ONLY raw JSON: '
        '{"action":"ACTION","payload":"TEXT"}. No markdown blocks, no filler.',
        "",
        "# NOTE: this action vocabulary is wired to the rana-socketd daemon. "
        'ALWAYS emit one of these KEYWORDS in the "action" field — never the '
        'FlatBuffer variant name.',
        "",
        '"action" rules:',
    ]
    # The daemon runs a fuzzy lexical pre-router first; these are the actions it
    # can already handle without you. Emit the matching keyword when the user's
    # intent clearly fits one; otherwise fall back to "reply".
    for key in cmds:
        if key == "ask":
            continue
        kw = CANON.get(key, key)
        entry = cmds[key]
        desc = entry.get("description") or (key.replace("_", " ") + " command")
        if key == "speak":
            lines.append(
                '- "reply": USE THIS FOR ALL GENERAL KNOWLEDGE, trivia, '
                "definitions, math, and historical facts. " + desc)
        elif key == "browser":
            lines.append(
                '- "search_web": DO NOT use for general knowledge. Use ONLY for '
                "real-time/time-sensitive live data (current weather, breaking "
                'news, live sports scores, current stock prices).')
            lines.append(
                '- "open_browser": ' + desc + ' (device trigger; payload must be "").')
        elif key == "lights":
            lines.append(
                '- "toggle_lights": ' + desc + ' (device trigger; payload must be "").')
        elif key == "power":
            lines.append(
                '- "shutdown": ' + desc + ' (device trigger; payload must be "").')
        else:
            lines.append('- "%s": %s' % (kw, desc))

    lines.append(
        'If the request fits none of the above, use "reply" with a short spoken '
        "answer.")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
