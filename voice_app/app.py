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
from dotenv import load_dotenv
import tkinter as tk
from tkinter import ttk


ALLOWED_COMMANDS = {
    ("light", "on"),
    ("light", "off"),
    ("clarification", "required"),
}
DEFAULT_STT_URL = "https://api.elevenlabs.io/v1/speech-to-text"
DEFAULT_TTS_URL = "https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
DEFAULT_OPENAI_BASE = "https://api.openai.com/v1"

load_dotenv()


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
        data={"model_id": "scribe_v1", "language_code": "vi"},
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

    RECORD_SECONDS = 5.0

    def __init__(self, root: tk.Tk, command_output_hook: Optional[Callable[[dict[str, str]], None]] = None):
        self.root = root
        self.command_output_hook = command_output_hook
        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.busy = False
        self._build_ui()
        self.root.after(100, self._poll_events)

    def _build_ui(self) -> None:
        self.root.title("Trợ lý điều khiển đèn")
        self.root.geometry("780x720")
        self.root.minsize(650, 620)
        self.root.configure(bg="#f4f7fb")
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("Title.TLabel", font=("Segoe UI", 20, "bold"), foreground="#17324d")
        style.configure("Hint.TLabel", foreground="#526579")
        style.configure("Mic.TButton", font=("Segoe UI", 15, "bold"), padding=(22, 14), foreground="#ffffff", background="#1769aa")
        style.map("Mic.TButton", background=[("active", "#0d4f82"), ("disabled", "#9aa9b8")])
        frame = ttk.Frame(self.root, padding=18)
        frame.pack(fill="both", expand=True)
        ttk.Label(frame, text="Trợ lý điều khiển đèn", style="Title.TLabel").pack(anchor="w")
        ttk.Label(frame, text="Nói tiếng Việt hoặc nhập yêu cầu bằng văn bản. Ứng dụng không gửi lệnh đến ESP32.", style="Hint.TLabel").pack(anchor="w", pady=(3, 14))

        credentials = ttk.LabelFrame(frame, text="Cấu hình dịch vụ (chỉ lưu trong bộ nhớ)", padding=10)
        credentials.pack(fill="x", pady=(0, 12))
        fields = [("ElevenLabs API key", True), ("ElevenLabs voice ID", False), ("OpenAI-compatible API key", True), ("OpenAI base URL", False), ("Model", False)]
        self.entries: dict[str, ttk.Entry] = {}
        defaults = [os.getenv("ELEVENLABS_API_KEY", ""), os.getenv("ELEVENLABS_VOICE_ID", ""), os.getenv("OPENAI_API_KEY", ""), os.getenv("OPENAI_BASE_URL", DEFAULT_OPENAI_BASE), os.getenv("OPENAI_MODEL", "gpt-4o-mini")]
        for row, ((label, secret), default) in enumerate(zip(fields, defaults)):
            ttk.Label(credentials, text=label).grid(row=row, column=0, sticky="w", padx=(0, 10), pady=3)
            entry = ttk.Entry(credentials, show="•" if secret else "")
            entry.insert(0, default)
            entry.grid(row=row, column=1, sticky="ew", pady=3)
            self.entries[label] = entry
        credentials.columnconfigure(1, weight=1)

        controls = ttk.LabelFrame(frame, text="Yêu cầu", padding=10)
        controls.pack(fill="x", pady=(0, 12))
        self.record_button = ttk.Button(controls, text="🎤  GHI ÂM 5 GIÂY", style="Mic.TButton", command=self._record)
        self.record_button.pack(side="left", padx=(0, 12))
        ttk.Label(controls, text="hoặc", style="Hint.TLabel").pack(side="left", padx=(0, 8))
        self.typed = ttk.Entry(controls)
        self.typed.pack(side="left", fill="x", expand=True, padx=(0, 8))
        self.typed.bind("<Return>", lambda _event: self._typed())
        self.typed_button = ttk.Button(controls, text="Xử lý văn bản", command=self._typed)
        self.typed_button.pack(side="left")
        self.tts_enabled = tk.BooleanVar(value=True)
        ttk.Checkbutton(controls, text="Đọc phản hồi", variable=self.tts_enabled).pack(anchor="w", pady=(9, 0))

        output = ttk.LabelFrame(frame, text="Kết quả", padding=10)
        output.pack(fill="both", expand=True)
        ttk.Label(output, text="Bản chép lời").grid(row=0, column=0, sticky="nw", pady=4)
        self.transcript = tk.Text(output, height=3, wrap="word", state="disabled", font=("Segoe UI", 11))
        self.transcript.grid(row=0, column=1, sticky="nsew", pady=4)
        ttk.Label(output, text="Lệnh đã xác thực").grid(row=1, column=0, sticky="nw", pady=4)
        self.command = tk.Text(output, height=3, wrap="word", state="disabled", font=("Consolas", 10))
        self.command.grid(row=1, column=1, sticky="nsew", pady=4)
        ttk.Label(output, text="Phản hồi tiếng Việt").grid(row=2, column=0, sticky="nw", pady=4)
        self.response = tk.Text(output, height=3, wrap="word", state="disabled", font=("Segoe UI", 11))
        self.response.grid(row=2, column=1, sticky="nsew", pady=4)
        ttk.Label(output, text="Trạng thái").grid(row=3, column=0, sticky="w", pady=8)
        self.status = tk.StringVar(value="Sẵn sàng. Chưa gửi lệnh đến thiết bị.")
        ttk.Label(output, textvariable=self.status, wraplength=560).grid(row=3, column=1, sticky="w", pady=8)
        output.columnconfigure(1, weight=1)
        output.rowconfigure(0, weight=1)
        output.rowconfigure(1, weight=1)
        output.rowconfigure(2, weight=1)

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
        self.typed_button.configure(state="disabled")
        for widget in (self.transcript, self.command, self.response):
            self._set_text(widget, "")
        self.status.set("Đang xử lý…")
        try:
            threading.Thread(target=worker, daemon=True).start()
        except Exception as exc:
            self.busy = False
            self.record_button.configure(state="normal")
            self.typed_button.configure(state="normal")
            self.status.set("Không thể bắt đầu: " + str(exc))

    def _typed(self) -> None:
        if self.busy:
            return
        text = self.typed.get().strip()
        if not text:
            self.status.set("Vui lòng nhập yêu cầu trước khi xử lý.")
            self.typed.focus_set()
            return
        config = self._config()
        do_tts = self.tts_enabled.get()
        def worker() -> None:
            self.events.put(("transcript", text))
            self.events.put(("status", "Đang phân tích yêu cầu…"))
            self._interpret(text, config, do_tts=do_tts)
        self._start(worker)

    def _record(self) -> None:
        if self.busy:
            return
        config = self._config()
        do_tts = self.tts_enabled.get()
        def worker() -> None:
            try:
                self.events.put(("status", "Đang ghi âm trong 5 giây…"))
                audio = record_wav(self.RECORD_SECONDS)
            except Exception as exc:
                self.events.put(("input_error", "Không thể ghi âm: " + str(exc)))
                return
            try:
                self.events.put(("status", "Đang nhận dạng giọng nói tiếng Việt…"))
                text = transcribe_audio(audio, config.elevenlabs_key)
                if not text or not text.strip():
                    raise ValueError("Không nhận được nội dung giọng nói")
            except Exception as exc:
                self.events.put(("input_error", "Không thể nhận dạng giọng nói: " + str(exc)))
                return
            self.events.put(("transcript", text))
            self.events.put(("status", "Đang phân tích yêu cầu…"))
            self._interpret(text, config, do_tts=do_tts)
        self._start(worker)

    def _interpret(self, text: str, config: Optional[AppConfig] = None, do_tts: bool = True) -> None:
        """Interpret text using a UI-thread configuration snapshot."""
        if config is None:
            config = self._config()
        try:
            if not text or not text.strip():
                raise ValueError("Vui lòng nhập yêu cầu không để trống.")
            command = validate_command(interpret_transcript(text, config))
            self.events.put(("result", (text, command)))
            if self.command_output_hook and command["device"] == "light":
                self.command_output_hook(command)
            if do_tts:
                self.events.put(("status", "Đang tạo và phát phản hồi…"))
                try:
                    audio = synthesize_speech(command_response(command), config)
                    play_wav(audio)
                except Exception as exc:
                    self.events.put(("tts_error", str(exc)))
                    return
            self.events.put(("status", "Ready. No device command was sent."))
        except Exception as exc:
            self.events.put(("error", str(exc)))

    def _poll_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "transcript":
                    self._set_text(self.transcript, payload)
                elif kind == "result":
                    text, command = payload
                    self._set_text(self.transcript, text)
                    self._set_text(self.command, json.dumps(command, ensure_ascii=False))
                    self._set_text(self.response, command_response(command))
                elif kind == "status":
                    self.status.set("Sẵn sàng. Chưa gửi lệnh đến thiết bị." if payload.startswith("Ready") else payload)
                elif kind == "input_error":
                    self.status.set(payload + ". Vui lòng nhập yêu cầu vào ô văn bản và chọn Xử lý văn bản để thử lại.")
                    self.typed.focus_set()
                elif kind == "tts_error":
                    self.status.set("Đã xử lý yêu cầu, nhưng không thể đọc phản hồi: " + payload)
                elif kind == "error":
                    self.status.set("Không thể xử lý yêu cầu: " + payload + ". Vui lòng thử lại.")
                if kind in {"error", "input_error", "tts_error"} or (kind == "status" and payload.startswith("Ready")):
                    self.busy = False
                    self.record_button.configure(state="normal")
                    self.typed_button.configure(state="normal")
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
