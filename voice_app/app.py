"""Voice-controlled light command desktop app.

The app interprets Vietnamese speech/text into a deliberately small command
allowlist. It never sends commands to an ESP32; ``command_output_hook`` is an
optional observation hook for a future integration.
"""

from __future__ import annotations

import io
import json
import os
import queue
import threading
import wave
from dataclasses import dataclass
from typing import Any, Callable, Optional

import requests
import tkinter as tk
from tkinter import messagebox, ttk


ALLOWED_COMMANDS = {
    ("light", "on"),
    ("light", "off"),
    ("clarification", "required"),
}
DEFAULT_STT_URL = "https://api.elevenlabs.io/v1/speech-to-text"
DEFAULT_TTS_URL = "https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
DEFAULT_OPENAI_BASE = "https://api.openai.com/v1"


@dataclass(frozen=True)
class AppConfig:
    elevenlabs_key: str
    elevenlabs_voice_id: str
    openai_key: str
    openai_base_url: str = DEFAULT_OPENAI_BASE
    openai_model: str = "gpt-4o-mini"


def normalize_base_url(base_url: str) -> str:
    """Return an OpenAI-compatible base URL without a trailing slash."""
    value = (base_url or DEFAULT_OPENAI_BASE).strip().rstrip("/")
    if value.endswith("/chat/completions"):
        value = value[: -len("/chat/completions")]
    return value


def validate_command(value: Any) -> dict[str, str]:
    """Validate a model value and return a safe command or clarification.

    Accepted values are exactly ``{"device": "light", "action": "on"}``,
    ``off``, or ``{"device": "clarification", "action": "required"}``.
    Extra fields, unsupported devices/actions, and malformed values clarify.
    """
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return {"device": "clarification", "action": "required"}
    if not isinstance(value, dict) or set(value) != {"device", "action"}:
        return {"device": "clarification", "action": "required"}
    device = value.get("device")
    action = value.get("action")
    if not isinstance(device, str) or not isinstance(action, str) or (device, action) not in ALLOWED_COMMANDS:
        return {"device": "clarification", "action": "required"}
    return {"device": device, "action": action}


def command_prompt(transcript: str) -> str:
    """Build the constrained Vietnamese interpretation prompt."""
    return (
        "Bạn là bộ phân tích lệnh đèn. Chỉ trả về đúng một JSON object, không "
        "markdown, không giải thích. Với yêu cầu bật đèn trả về "
        '{"device":"light","action":"on"}; với tắt đèn trả về '
        '{"device":"light","action":"off"}; mọi yêu cầu khác hoặc không rõ '
        'trả về {"device":"clarification","action":"required"}. '
        "Không được điều khiển thiết bị thật. Yêu cầu người dùng: " + transcript
    )


def transcribe_audio(
    audio: bytes,
    api_key: str,
    *,
    url: str = DEFAULT_STT_URL,
    timeout: float = 30.0,
    session: Any = requests,
) -> str:
    """Transcribe WAV audio with ElevenLabs STT."""
    if not api_key.strip():
        raise ValueError("ElevenLabs API key is required")
    response = session.post(
        url,
        headers={"xi-api-key": api_key.strip()},
        files={"file": ("recording.wav", audio, "audio/wav")},
        data={"model_id": "scribe_v1"},
        timeout=timeout,
    )
    response.raise_for_status()
    payload = response.json()
    text = payload.get("text", "") if isinstance(payload, dict) else ""
    if not isinstance(text, str) or not text.strip():
        raise ValueError("Speech recognition returned no text")
    return text.strip()


def interpret_transcript(
    transcript: str,
    config: AppConfig,
    *,
    timeout: float = 30.0,
    session: Any = requests,
) -> dict[str, str]:
    """Ask an OpenAI-compatible chat endpoint and validate its response."""
    if not config.openai_key.strip():
        raise ValueError("OpenAI-compatible API key is required")
    if not transcript.strip():
        return {"device": "clarification", "action": "required"}
    endpoint = normalize_base_url(config.openai_base_url) + "/chat/completions"
    response = session.post(
        endpoint,
        headers={
            "Authorization": f"Bearer {config.openai_key.strip()}",
            "Content-Type": "application/json",
        },
        json={
            "model": config.openai_model.strip() or "gpt-4o-mini",
            "temperature": 0,
            "messages": [
                {"role": "system", "content": "Follow the requested JSON-only format."},
                {"role": "user", "content": command_prompt(transcript)},
            ],
        },
        timeout=timeout,
    )
    response.raise_for_status()
    payload = response.json()
    try:
        content = payload["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        raise ValueError("OpenAI-compatible response has no message content") from exc
    return validate_command(content)


def synthesize_speech(
    text: str,
    config: AppConfig,
    *,
    timeout: float = 30.0,
    session: Any = requests,
) -> bytes:
    """Create a WAV response with ElevenLabs TTS."""
    if not config.elevenlabs_key.strip() or not config.elevenlabs_voice_id.strip():
        raise ValueError("ElevenLabs API key and voice ID are required for speech")
    url = DEFAULT_TTS_URL.format(voice_id=config.elevenlabs_voice_id.strip())
    response = session.post(
        url,
        headers={"xi-api-key": config.elevenlabs_key.strip(), "Content-Type": "application/json"},
        params={"output_format": "pcm_22050"},
        json={"text": text, "model_id": "eleven_multilingual_v2"},
        timeout=timeout,
    )
    response.raise_for_status()
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(22050)
        wav.writeframes(response.content)
    return output.getvalue()


def record_wav(seconds: float = 5.0, sample_rate: int = 16_000) -> bytes:
    """Record microphone audio and return it as WAV bytes."""
    import sounddevice as sd
    import soundfile as sf

    if seconds <= 0:
        raise ValueError("Recording duration must be positive")
    frames = sd.rec(int(seconds * sample_rate), samplerate=sample_rate, channels=1, dtype="float32")
    sd.wait()
    output = io.BytesIO()
    sf.write(output, frames, sample_rate, format="WAV", subtype="PCM_16")
    return output.getvalue()


def play_wav(audio: bytes) -> None:
    """Play WAV bytes through the default audio output."""
    import sounddevice as sd
    import soundfile as sf

    data, rate = sf.read(io.BytesIO(audio), dtype="float32")
    sd.play(data, rate)
    sd.wait()


def command_response(command: dict[str, str]) -> str:
    """Return a short Vietnamese response for a validated command."""
    if command == {"device": "light", "action": "on"}:
        return "Đã nhận lệnh bật đèn. Chưa gửi đến thiết bị thật."
    if command == {"device": "light", "action": "off"}:
        return "Đã nhận lệnh tắt đèn. Chưa gửi đến thiết bị thật."
    return "Tôi chưa hiểu rõ. Vui lòng nói bật đèn hoặc tắt đèn."


class VoiceApp:
    """Tkinter UI; all network, recording, and playback work runs in workers."""

    def __init__(self, root: tk.Tk, command_output_hook: Optional[Callable[[dict[str, str]], None]] = None):
        self.root = root
        self.command_output_hook = command_output_hook
        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.busy = False
        self._build_ui()
        self.root.after(100, self._poll_events)

    def _build_ui(self) -> None:
        self.root.title("Điều khiển đèn bằng giọng nói")
        self.root.geometry("700x560")
        frame = ttk.Frame(self.root, padding=12)
        frame.pack(fill="both", expand=True)
        fields = [
            ("ElevenLabs API key", True), ("ElevenLabs voice ID", False),
            ("OpenAI-compatible API key", True), ("OpenAI base URL", False),
            ("Model", False),
        ]
        self.entries: dict[str, ttk.Entry] = {}
        defaults = [
            os.getenv("ELEVENLABS_API_KEY", ""),
            os.getenv("ELEVENLABS_VOICE_ID", ""),
            os.getenv("OPENAI_API_KEY", ""),
            os.getenv("OPENAI_BASE_URL", DEFAULT_OPENAI_BASE),
            os.getenv("OPENAI_MODEL", "gpt-4o-mini"),
        ]
        for row, ((label, secret), default) in enumerate(zip(fields, defaults)):
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", pady=4)
            entry = ttk.Entry(frame, show="•" if secret else "", width=62)
            entry.insert(0, default)
            entry.grid(row=row, column=1, sticky="ew", pady=4)
            self.entries[label] = entry
        frame.columnconfigure(1, weight=1)
        ttk.Label(frame, text="Typed request (microphone optional)").grid(row=5, column=0, sticky="w", pady=(14, 4))
        self.typed = ttk.Entry(frame, width=62)
        self.typed.grid(row=5, column=1, sticky="ew", pady=(14, 4))
        buttons = ttk.Frame(frame)
        buttons.grid(row=6, column=0, columnspan=2, pady=8, sticky="w")
        self.record_button = ttk.Button(buttons, text="Record 5 seconds", command=self._record)
        self.record_button.pack(side="left", padx=(0, 8))
        ttk.Button(buttons, text="Interpret typed text", command=self._typed).pack(side="left")
        ttk.Label(frame, text="Transcript").grid(row=7, column=0, sticky="nw", pady=4)
        self.transcript = tk.Text(frame, height=4, width=60, state="disabled")
        self.transcript.grid(row=7, column=1, sticky="nsew", pady=4)
        ttk.Label(frame, text="Validated command").grid(row=8, column=0, sticky="nw", pady=4)
        self.command = tk.Text(frame, height=3, width=60, state="disabled")
        self.command.grid(row=8, column=1, sticky="nsew", pady=4)
        ttk.Label(frame, text="Status").grid(row=9, column=0, sticky="w", pady=4)
        self.status = tk.StringVar(value="Ready. Credentials are kept in memory only.")
        ttk.Label(frame, textvariable=self.status, wraplength=560).grid(row=9, column=1, sticky="w", pady=4)
        frame.rowconfigure(7, weight=1)

    def _config(self) -> AppConfig:
        return AppConfig(
            self.entries["ElevenLabs API key"].get(), self.entries["ElevenLabs voice ID"].get(),
            self.entries["OpenAI-compatible API key"].get(), self.entries["OpenAI base URL"].get(),
            self.entries["Model"].get(),
        )

    def _set_text(self, widget: tk.Text, text: str) -> None:
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", text)
        widget.configure(state="disabled")

    def _start(self, worker: Callable[[], None]) -> None:
        if self.busy:
            return
        self.busy = True
        self.record_button.configure(state="disabled")
        self.status.set("Working…")
        threading.Thread(target=worker, daemon=True).start()

    def _typed(self) -> None:
        text = self.typed.get().strip()
        if not text:
            self.status.set("Enter a request first.")
            return
        config = self._config()
        self._start(lambda: self._interpret(text, config, do_tts=True))

    def _record(self) -> None:
        config = self._config()
        def worker() -> None:
            try:
                self.events.put(("status", "Recording…"))
                audio = record_wav()
                text = transcribe_audio(audio, config.elevenlabs_key)
                self._interpret(text, config, do_tts=True)
            except Exception as exc:
                self.events.put(("error", str(exc)))
        self._start(worker)

    def _interpret(self, text: str, config: Optional[AppConfig] = None, do_tts: bool = True) -> None:
        """Interpret text using a UI-thread configuration snapshot."""
        if config is None:
            config = self._config()
        try:
            command = interpret_transcript(text, config)
            self.events.put(("result", (text, command)))
            if self.command_output_hook and command["device"] == "light":
                self.command_output_hook(command)
            if do_tts:
                audio = synthesize_speech(command_response(command), config)
                play_wav(audio)
            self.events.put(("status", "Ready. No device command was sent."))
        except Exception as exc:
            self.events.put(("error", str(exc)))

    def _poll_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "result":
                    text, command = payload
                    self._set_text(self.transcript, text)
                    self._set_text(self.command, json.dumps(command, ensure_ascii=False))
                elif kind == "status":
                    self.status.set(payload)
                elif kind == "error":
                    self.status.set("Error: " + payload)
                    messagebox.showerror("Voice app error", payload)
                if kind in {"error", "status"} and (kind == "error" or payload.startswith("Ready")):
                    self.busy = False
                    self.record_button.configure(state="normal")
        except queue.Empty:
            pass
        self.root.after(100, self._poll_events)


def main() -> None:
    """Launch the desktop application."""
    root = tk.Tk()
    VoiceApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
