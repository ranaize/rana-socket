#!/usr/bin/env bash
#
# rana-stt.sh — STT hop for TalkCmd.
# Invoked by rana-socketd with a WAV file path as $1 (the decoded TalkCmd
# audio). Transcribes it against the LocalAI whisper `talk` model and prints
# the transcript text on stdout. rana-socketd then feeds the transcript into
# the same ask hop as AskCmd.
#
# Env (set by the daemon):
#   STT_URL    LocalAI base URL (default http://localai:8080)
#   STT_MODEL  whisper model name (default talk; see talk.yaml)

set -euo pipefail

STT_URL="${STT_URL:-http://localai:8080}"
STT_MODEL="${STT_MODEL:-talk}"
WAV="${1:-}"

if [[ -z "$WAV" || ! -f "$WAV" ]]; then
    echo "rana-stt.sh: missing or unreadable wav file: '$WAV'" >&2
    exit 1
fi

RESP="$(curl -sS -f -X POST "$STT_URL/v1/audio/transcriptions" \
    -F "file=@$WAV;type=audio/wav" \
    -F "model=$STT_MODEL")"

# Print only the transcript text (no trailing newline) — jq handles the parse.
printf '%s' "$(jq -r '.text // empty' <<<"$RESP")"
