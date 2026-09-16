"""Type-safe FlatBuffers command builders + framed socket transport.

One builder method per command `key` — the client cannot lie about the command
key it sends. Every command maps onto the generic envelope Command{key, action,
target, params, data} (see schema/command.fbs); the daemon validates `key`
against [commands.allowed] and routes to scripts/<key>.pluto.

The generated FlatBuffers module (`command_generated.py`) ships inside this
package and is produced by:

    flatc --gen-onefile --python -o schema/generated schema/command.fbs
"""

import base64
import logging
import socket
import struct
import time

import flatbuffers

from . import command_generated as fb

logger = logging.getLogger(__name__)


STATUS_NAMES = {
    fb.Status.OK: "OK",
    fb.Status.UNKNOWN_COMMAND: "UNKNOWN_COMMAND",
    fb.Status.EXECUTION_FAILED: "EXECUTION_FAILED",
    fb.Status.INVALID_PAYLOAD: "INVALID_PAYLOAD",
    fb.Status.FORBIDDEN: "FORBIDDEN",
    fb.Status.SAFETY_GATE_FAILED: "SAFETY_GATE_FAILED",
    fb.Status.FORWARD_TO_CLIENT: "FORWARD_TO_CLIENT",
}


def _str(builder, s):
    return builder.CreateString(s) if s else 0


def _params(builder, params):
    """Build a [StrPair] vector from a list of (k, v) string tuples.

    Uses the raw StartVector/PrependUOffsetTRelative/EndVector primitives
    instead of the generated CommandCreateParamsVector (which maps to
    Builder.CreateVectorOfTables — a method that exists only in unreleased
    flatbuffers master, not in any released Python runtime).
    """
    if not params:
        return 0
    offs = []
    for k, v in params:
        koff = builder.CreateString(k)
        voff = builder.CreateString(v)
        StrPairStart = fb.StrPairStart
        StrPairStart(builder)
        fb.StrPairAddK(builder, koff)
        fb.StrPairAddV(builder, voff)
        offs.append(fb.StrPairEnd(builder))
    fb.CommandStartParamsVector(builder, len(offs))
    for o in reversed(offs):
        builder.PrependUOffsetTRelative(o)
    return builder.EndVector(len(offs))


def _envelope(builder, key, action="", target="", params=None, data=None,
              request_id: int = 0) -> bytes:
    """Finish a generic Command envelope."""
    key_off = builder.CreateString(key)
    action_off = _str(builder, action)
    target_off = _str(builder, target)
    params_off = _params(builder, params or [])
    data_off = builder.CreateByteVector(bytes(data)) if data else 0

    fb.CommandStart(builder)
    fb.CommandAddRequestId(builder, request_id)
    fb.CommandAddKey(builder, key_off)
    fb.CommandAddAction(builder, action_off)
    fb.CommandAddTarget(builder, target_off)
    if params_off:
        fb.CommandAddParams(builder, params_off)
    if data_off:
        fb.CommandAddData(builder, data_off)
    cmd = fb.CommandEnd(builder)
    builder.Finish(cmd)
    return bytes(builder.Output())


class CommandBuilder:
    """Type-safe builders — one method per command key."""

    @staticmethod
    def speak(text: str, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        return _envelope(b, "speak", action="speak", target=text, request_id=request_id)

    @staticmethod
    def ask(text: str, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        return _envelope(b, "ask", target=text, request_id=request_id)

    @staticmethod
    def light(action: str, room: str = "all", request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        return _envelope(b, "lights", action=action, target=room, request_id=request_id)

    @staticmethod
    def volume(level: int, mute: bool = False, request_id: int = 0) -> bytes:
        assert 0 <= level <= 100
        b = flatbuffers.Builder(64)
        params = [("mute", "true")] if mute else []
        return _envelope(b, "volume", action="set", target=str(level),
                         params=params, request_id=request_id)

    @staticmethod
    def power(action: str, confirm: bool, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        params = [("confirm", "true")] if confirm else []
        return _envelope(b, "power", action=action, params=params, request_id=request_id)

    @staticmethod
    def launch_browser(url=None, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        return _envelope(b, "browser", action="open", target=url or "", request_id=request_id)

    @staticmethod
    def play_movie(path: str, start_seconds: int = 0, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        params = [("start", str(start_seconds))] if start_seconds else []
        return _envelope(b, "play_movie", action="play", target=path,
                         params=params, request_id=request_id)

    @staticmethod
    def media(action: str, target=None, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        return _envelope(b, "media", action=action, target=target or "", request_id=request_id)

    @staticmethod
    def lookup_machine(subnet=None, timeout_ms: int = 0, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        params = [("timeout_ms", str(timeout_ms))] if timeout_ms else []
        return _envelope(b, "lookup_machine", action="lookup", target=subnet or "",
                         params=params, request_id=request_id)

    @staticmethod
    def mc_server(action: str, world_name=None, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        return _envelope(b, "mc_server", action=action, target=world_name or "",
                         request_id=request_id)

    @staticmethod
    def talk(audio: bytes, encoding: str = "pcm16", sample_rate: int = 16000,
             channels: int = 1, request_id: int = 0) -> bytes:
        """Build a `talk` Command: raw audio bytes + format metadata. The daemon
        decodes/transcribes on-server (rana-serializer + STT hop), then routes the
        transcript through the ask hop — exactly as `ask()` does with text. The
        (untrusted) client never sees the LLM or STT."""
        b = flatbuffers.Builder(1024)
        params = [
            ("encoding", encoding),
            ("sample_rate", str(sample_rate)),
            ("channels", str(channels)),
        ]
        return _envelope(b, "talk", action=encoding, target="", params=params,
                         data=bytes(audio), request_id=request_id)


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed mid-frame")
        buf += chunk
    return buf


def send(sock: socket.socket, payload: bytes) -> dict:
    """Sends one framed command, returns the daemon's decoded response.

    A response may instead be a forward directive: status FORWARD_TO_CLIENT
    (7) means the daemon could not run the command locally and has base64-
    encoded the inner Command in the message for the caller to relay elsewhere.
    """
    logger.debug("send: transmitting %d-byte framed command", len(payload))
    t0 = time.monotonic()
    try:
        sock.sendall(struct.pack(">I", len(payload)) + payload)
        length = struct.unpack(">I", _recv_exact(sock, 4))[0]
        resp_bytes = _recv_exact(sock, length)
    except (OSError, ConnectionError) as e:
        logger.error("send: transport error after %.2fs: %s", time.monotonic() - t0, e)
        raise
    r = fb.Response.GetRootAsResponse(resp_bytes, 0)
    status = r.Status()
    dt = time.monotonic() - t0
    if status == fb.Status.FORWARD_TO_CLIENT:
        logger.info(
            "recv: %d-byte response in %.2fs — status=FORWARD_TO_CLIENT (server "
            "defers to local daemon), inner command %d bytes",
            length, dt, len(r.Message() or b""),
        )
        return {
            "request_id": r.RequestId(),
            "status": status,
            "status_name": "FORWARD_TO_CLIENT",
            "forward": True,
            "command": base64.b64decode(r.Message()),
        }
    msg = (r.Message() or b"").decode(errors="replace")
    logger.info(
        "recv: %d-byte response in %.2fs — status=%s(%d)%s",
        length, dt, STATUS_NAMES.get(status, str(status)), status,
        f" message={msg!r}" if msg else "",
    )
    return {
        "request_id": r.RequestId(),
        "status": status,
        "status_name": STATUS_NAMES.get(status, str(status)),
        "message": msg,
        "forward": False,
    }
