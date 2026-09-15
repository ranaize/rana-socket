#!/usr/bin/env bash
#
# rana-ask.sh — LLM hop for AskCmd.
# Invoked by rana-socketd with the user's question as $1 (never via a shell).
# Talks ONLY to the tower's LocalAI (ASK_LLM_URL, default http://localai:8080),
# extracts the assistant's JSON content, and pipes it through rana-serializer,
# which emits a binary rana::AskReply FlatBuffer on stdout. rana-socketd verifies
# and reads that buffer — no JSON is ever parsed inside the daemon.

set -euo pipefail

ASK_LLM_URL="${ASK_LLM_URL:-http://localai:8080}"
MODEL="command"
QUESTION="${1:-}"

if [[ -z "$QUESTION" ]]; then
    # No question: answer safely without touching LocalAI.
    echo '{"action":"reply","payload":"I did not catch that."}' | rana-serializer
    exit 0
fi

# The action vocabulary is owned by rana-socketd; derive the system prompt from
# its config (RANA_CONFIG is set by the daemon). Fall back to a minimal prompt
# only if the config is somehow unavailable.
SYSTEM_PROMPT="$(gen_system_prompt.py 2>/dev/null)" || \
SYSTEM_PROMPT='You are a strict voice command routing engine. Output ONLY raw JSON: {"action":"ACTION","payload":"TEXT"}. No markdown blocks, no filler.'

REQUEST="$(jq -n \
    --arg sys "$SYSTEM_PROMPT" \
    --arg q "$QUESTION" \
    --arg model "$MODEL" \
    '{model:$model,
      messages:[{role:"system",content:$sys},{role:"user",content:$q}],
      response_format:{type:"json_object"},
      temperature:0,
      max_tokens:256}')"

RESP="$(curl -sS -f -X POST "$ASK_LLM_URL/v1/chat/completions" \
    -H 'Content-Type: application/json' \
    -d "$REQUEST")"

# Extract the assistant content (the routed JSON) and let rana-serializer turn
# it into a binary AskReply flatbuffer on stdout.
CONTENT="$(jq -r '.choices[0].message.content // empty' <<<"$RESP")"
if [[ -z "$CONTENT" ]]; then
    echo '{"action":"reply","payload":"Sorry, I had no answer."}' | rana-serializer
    exit 0
fi
printf '%s' "$CONTENT" | rana-serializer
