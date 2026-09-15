"""Type-safe FlatBuffers command builders + framed socket transport.

One builder method per union variant — the client cannot lie about the
payload shape it sends; anything else is rejected by the schema itself.

The generated FlatBuffers module (`command_generated.py`) ships inside this
package and is produced by:

    flatc --gen-onefile --python -o schema/generated schema/command.fbs
"""

import base64
import socket
import struct

import flatbuffers

from . import command_generated as fb


_MC_ACTIONS = {
    "start": fb.McAction.Start,
    "stop": fb.McAction.Stop,
    "restart": fb.McAction.Restart,
}
_POWER_ACTIONS = {
    "shutdown": fb.PowerAction.Shutdown,
    "standby": fb.PowerAction.Standby,
    "reboot": fb.PowerAction.Reboot,
}
_MEDIA_ACTIONS = {
    "play": fb.MediaAction.Play,
    "pause": fb.MediaAction.Pause,
    "playpause": fb.MediaAction.PlayPause,
    "stop": fb.MediaAction.Stop,
}
_LIGHT_ACTIONS = {
    "toggle": fb.LightAction.Toggle,
    "on": fb.LightAction.On,
    "off": fb.LightAction.Off,
}

STATUS_NAMES = {
    fb.Status.OK: "OK",
    fb.Status.UNKNOWN_COMMAND: "UNKNOWN_COMMAND",
    fb.Status.TYPE_MISMATCH: "TYPE_MISMATCH",
    fb.Status.EXECUTION_FAILED: "EXECUTION_FAILED",
    fb.Status.INVALID_PAYLOAD: "INVALID_PAYLOAD",
    fb.Status.FORBIDDEN: "FORBIDDEN",
    fb.Status.SAFETY_GATE_FAILED: "SAFETY_GATE_FAILED",
    fb.Status.FORWARD_TO_CLIENT: "FORWARD_TO_CLIENT",
}


class CommandBuilder:
    """Type-safe builders — one method per union variant."""

    @staticmethod
    def mc_server(action: str, world_name=None, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        world_off = b.CreateString(world_name) if world_name else 0
        fb.McServerCmdStart(b)
        fb.McServerCmdAddAction(b, _MC_ACTIONS[action])
        if world_off:
            fb.McServerCmdAddWorldName(b, world_off)
        payload = fb.McServerCmdEnd(b)
        return _envelope(b, fb.CommandPayload.McServerCmd, payload, request_id)

    @staticmethod
    def volume(level: int, mute: bool = False, request_id: int = 0) -> bytes:
        assert 0 <= level <= 100
        b = flatbuffers.Builder(64)
        fb.VolumeCmdStart(b)
        fb.VolumeCmdAddLevel(b, level)
        fb.VolumeCmdAddMute(b, mute)
        payload = fb.VolumeCmdEnd(b)
        return _envelope(b, fb.CommandPayload.VolumeCmd, payload, request_id)

    @staticmethod
    def power(action: str, confirm: bool, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        fb.PowerCmdStart(b)
        fb.PowerCmdAddAction(b, _POWER_ACTIONS[action])
        fb.PowerCmdAddConfirm(b, confirm)
        payload = fb.PowerCmdEnd(b)
        return _envelope(b, fb.CommandPayload.PowerCmd, payload, request_id)

    @staticmethod
    def play_movie(path: str, start_seconds: int = 0, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        path_off = b.CreateString(path)
        fb.PlayMovieCmdStart(b)
        fb.PlayMovieCmdAddPath(b, path_off)
        fb.PlayMovieCmdAddStartSeconds(b, start_seconds)
        payload = fb.PlayMovieCmdEnd(b)
        return _envelope(b, fb.CommandPayload.PlayMovieCmd, payload, request_id)

    @staticmethod
    def launch_browser(url=None, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        url_off = b.CreateString(url) if url else 0
        fb.LaunchBrowserCmdStart(b)
        if url_off:
            fb.LaunchBrowserCmdAddUrl(b, url_off)
        payload = fb.LaunchBrowserCmdEnd(b)
        return _envelope(b, fb.CommandPayload.LaunchBrowserCmd, payload, request_id)

    @staticmethod
    def media(action: str, target=None, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        target_off = b.CreateString(target) if target else 0
        fb.MediaCmdStart(b)
        fb.MediaCmdAddAction(b, _MEDIA_ACTIONS[action])
        if target_off:
            fb.MediaCmdAddTarget(b, target_off)
        payload = fb.MediaCmdEnd(b)
        return _envelope(b, fb.CommandPayload.MediaCmd, payload, request_id)

    @staticmethod
    def lookup_machine(subnet=None, timeout_ms: int = 0, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        subnet_off = b.CreateString(subnet) if subnet else 0
        fb.LookupMachineCmdStart(b)
        if subnet_off:
            fb.LookupMachineCmdAddSubnet(b, subnet_off)
        fb.LookupMachineCmdAddTimeoutMs(b, timeout_ms)
        payload = fb.LookupMachineCmdEnd(b)
        return _envelope(b, fb.CommandPayload.LookupMachineCmd, payload, request_id)

    @staticmethod
    def speak(text: str, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        text_off = b.CreateString(text)
        fb.SpeakCmdStart(b)
        fb.SpeakCmdAddText(b, text_off)
        payload = fb.SpeakCmdEnd(b)
        return _envelope(b, fb.CommandPayload.SpeakCmd, payload, request_id)

    @staticmethod
    def light(action: str, room: str = "all", request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(64)
        room_off = b.CreateString(room) if room else 0
        fb.LightCmdStart(b)
        if room_off:
            fb.LightCmdAddRoom(b, room_off)
        fb.LightCmdAddAction(b, _LIGHT_ACTIONS[action])
        payload = fb.LightCmdEnd(b)
        return _envelope(b, fb.CommandPayload.LightCmd, payload, request_id)

    @staticmethod
    def ask(text: str, request_id: int = 0) -> bytes:
        b = flatbuffers.Builder(256)
        text_off = b.CreateString(text)
        fb.AskCmdStart(b)
        fb.AskCmdAddText(b, text_off)
        payload = fb.AskCmdEnd(b)
        return _envelope(b, fb.CommandPayload.AskCmd, payload, request_id)

    @staticmethod
    def talk(audio: bytes, encoding: str = "pcm16", sample_rate: int = 16000,
             channels: int = 1, request_id: int = 0) -> bytes:
        """Build a TalkCmd: raw audio bytes + format metadata. The daemon
        decodes/transcribes on-server (rana-serializer + STT hop), then
        routes the transcript through the ask hop — exactly as `ask()` does with
        text. The (untrusted) client never sees the LLM or STT."""
        b = flatbuffers.Builder(1024)
        audio_off = b.CreateByteVector(bytes(audio))
        enc_off = b.CreateString(encoding)
        fb.TalkCmdStart(b)
        fb.TalkCmdAddAudio(b, audio_off)
        fb.TalkCmdAddEncoding(b, enc_off)
        fb.TalkCmdAddSampleRate(b, sample_rate)
        fb.TalkCmdAddChannels(b, channels)
        payload = fb.TalkCmdEnd(b)
        return _envelope(b, fb.CommandPayload.TalkCmd, payload, request_id)


def _envelope(builder, union_type, payload_offset, request_id: int) -> bytes:
    fb.CommandStart(builder)
    fb.CommandAddPayloadType(builder, union_type)
    fb.CommandAddPayload(builder, payload_offset)
    fb.CommandAddRequestId(builder, request_id)
    cmd = fb.CommandEnd(builder)
    builder.Finish(cmd)
    return bytes(builder.Output())


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
    sock.sendall(struct.pack(">I", len(payload)) + payload)
    length = struct.unpack(">I", _recv_exact(sock, 4))[0]
    resp_bytes = _recv_exact(sock, length)
    r = fb.Response.GetRootAsResponse(resp_bytes, 0)
    status = r.Status()
    if status == fb.Status.FORWARD_TO_CLIENT:
        return {
            "request_id": r.RequestId(),
            "status": status,
            "status_name": "FORWARD_TO_CLIENT",
            "forward": True,
            "command": base64.b64decode(r.Message()),
        }
    return {
        "request_id": r.RequestId(),
        "status": status,
        "status_name": STATUS_NAMES.get(status, str(status)),
        "message": (r.Message() or b"").decode(),
        "forward": False,
    }