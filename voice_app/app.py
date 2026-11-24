"""Vietnamese voice assistant and ESP32 sensor dashboard."""

from __future__ import annotations

import io
import json
import os
import re
import queue
import threading
import time
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
    ("pump", "on"),
    ("pump", "off"),
    ("mode", "auto"),
    ("mode", "manual"),
    ("clarification", "required"),
    ("sensor", "read"),
    ("weather", "forecast"),
}
DEFAULT_STT_URL = "https://api.elevenlabs.io/v1/speech-to-text"
DEFAULT_TTS_URL = "https://api.elevenlabs.io/v1/text-to-speech/{voice_id}"
load_dotenv()

import logging
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(levelname)s: %(message)s")
logger = logging.getLogger("voice_app")
logger.info("Starting voice app; dotenv loaded")

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
        "Bạn phân loại yêu cầu. Chỉ trả về một JSON object, không giải thích. "
        'Bật đèn: {"device":"light","action":"on"}; tắt đèn: '
        '{"device":"light","action":"off"}; bật bơm: {"device":"pump","action":"on"}; tắt bơm: '
        '{"device":"pump","action":"off"}; chế độ tự động: {"device":"mode","action":"auto"}; chế độ thủ công: '
        '{"device":"mode","action":"manual"}; hỏi cảm biến hoặc nhiệt độ: '
        '{"device":"sensor","action":"read"}; hỏi dự báo thời tiết: '
        '{"device":"weather","action":"forecast"}; còn lại: '
        '{"device":"clarification","action":"required"}. Yêu cầu: ' + transcript
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
    import logging
    logger = logging.getLogger("voice_app")
    logger.info("STT request: model_id=scribe_v1, language_code=vi, audio_size=%d bytes", len(audio))
    response = session.post(
        url,
        headers={"xi-api-key": api_key.strip()},
        files={"file": ("recording.wav", audio, "audio/wav")},
        data={"model_id": "scribe_v1", "language_code": "vi"},
        timeout=timeout,
    )
    logger.info("STT response: status=%s", getattr(response, "status_code", "unknown"))
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
    import logging
    logger = logging.getLogger("voice_app")
    endpoint = normalize_base_url(config.openai_base_url) + "/chat/completions"
    logger.info("Chat request: model=%s, base=%s, prompt_length=%d", config.openai_model, config.openai_base_url, len(transcript))
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
    logger.info("Chat response: status=%s", getattr(response, "status_code", "unknown"))
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
    import logging
    logger = logging.getLogger("voice_app")
    logger.info("TTS request: voice_id=%s, text_length=%d", config.elevenlabs_voice_id, len(text))
    response = session.post(
        url,
        headers={"xi-api-key": config.elevenlabs_key.strip(), "Content-Type": "application/json"},
        params={"output_format": "pcm_22050"},
        json={"text": text, "model_id": "eleven_multilingual_v2"},
        timeout=timeout,
    )
    logger.info("TTS response: status=%d, content_length=%d", response.status_code, len(response.content))
    response.raise_for_status()
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(22050)
        wav.writeframes(response.content)
    return output.getvalue()


def record_wav(seconds: float = 15.0, sample_rate: int = 16_000, device: Optional[int] = None) -> bytes:
    """Record until silence after speech or a hard duration limit."""
    import numpy as np
    import sounddevice as sd
    import soundfile as sf

    if seconds <= 0:
        raise ValueError("Recording duration must be positive")
    # ponytail: fixed RMS threshold; add adaptive noise calibration when needed.
    block = int(sample_rate * 0.1)
    frames = []
    speech = False
    silent = 0
    with sd.InputStream(samplerate=sample_rate, channels=1, dtype="float32", device=device, blocksize=block) as stream:
        for _ in range(max(1, int(seconds * 10))):
            data, overflowed = stream.read(block)
            if overflowed:
                raise RuntimeError("Microphone input overflow")
            frames.append(data.copy())
            if float(np.sqrt(np.mean(data ** 2))) >= 0.015:
                speech, silent = True, 0
            else:
                silent += 1
            if (speech and silent >= 12) or (not speech and silent >= 50):
                break
    if not speech:
        raise ValueError("Không phát hiện giọng nói")
    output = io.BytesIO()
    sf.write(output, np.concatenate(frames), sample_rate, format="WAV", subtype="PCM_16")
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
    if command == {"device": "pump", "action": "on"}:
        return "Đã nhận lệnh bật bơm. Chưa gửi đến thiết bị thật."
    if command == {"device": "pump", "action": "off"}:
        return "Đã nhận lệnh tắt bơm. Chưa gửi đến thiết bị thật."
    if command == {"device": "mode", "action": "auto"}:
        return "Đã nhận lệnh chuyển chế độ tự động. Chưa gửi đến thiết bị thật."
    if command == {"device": "mode", "action": "manual"}:
        return "Đã nhận lệnh chuyển chế độ thủ công. Chưa gửi đến thiết bị thật."
    return "Tôi chưa hiểu rõ. Vui lòng nói bật/tắt đèn, bật/tắt bơm, hoặc chuyển chế độ."


class VoiceApp:
    """Tkinter UI; all network, recording, and playback work runs in workers."""

    RECORD_SECONDS = 15.0

    def __init__(self, root: tk.Tk, command_output_hook: Optional[Callable[[dict[str, str]], None]] = None):
        self.root = root
        self.command_output_hook = command_output_hook
        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.busy = False
        self.polling = False
        self._build_ui()
        self.root.after(100, self._poll_events)
        self.root.after(0, self._poll_status)

    def _build_ui(self) -> None:
        self.root.title("Trợ lý giám sát STEM")
        self.root.geometry("960x760")
        self.root.minsize(760, 650)
        bg, ink, muted, accent, surface = "#f2f6f2", "#142a20", "#425a4c", "#245d42", "#ffffff"
        self.root.configure(bg=bg)
        frame = tk.Frame(self.root, bg=bg, padx=24, pady=16)
        frame.pack(fill="both", expand=True)
        top = tk.Frame(frame, bg=surface, padx=20, pady=12)
        top.pack(side="top", fill="x")
        self.connection = tk.StringVar(value="Đang kết nối ESP32…")
        tk.Label(top, text="VƯỜN THÔNG MINH", bg=surface, fg=accent, font=("Segoe UI", 12, "bold")).pack(anchor="w")
        tk.Label(top, textvariable=self.connection, bg=surface, fg=muted, font=("Segoe UI", 10)).pack(anchor="w")
        self.readings = tk.StringVar(value="Đang tải trạng thái cảm biến và thiết bị…")
        tk.Label(top, textvariable=self.readings, bg=surface, fg=ink, font=("Segoe UI", 16, "bold"),
                 justify="left", anchor="w", wraplength=870).pack(fill="x", pady=(8, 0))

        bottom = tk.Frame(frame, bg=surface, padx=18, pady=12)
        bottom.pack(side="bottom", fill="x", pady=(8, 0))
        tk.Label(bottom, text="YÊU CẦU GẦN NHẤT", bg=surface, fg=muted, font=("Segoe UI", 10, "bold")).pack(anchor="w")
        self.transcript = tk.StringVar(value="Chưa có yêu cầu")
        tk.Label(bottom, textvariable=self.transcript, bg=surface, fg=ink, font=("Segoe UI", 12),
                 anchor="w", wraplength=850, justify="left").pack(fill="x")
        self.response = tk.Text(bottom, height=2, wrap="word", state="disabled", relief="flat",
                                bg=surface, fg=ink, font=("Segoe UI", 11))
        self.response.pack(fill="x", pady=(3, 0))
        self.status = tk.StringVar(value="Sẵn sàng")
        tk.Label(bottom, textvariable=self.status, bg=surface, fg=muted, font=("Segoe UI", 10),
                 anchor="w", wraplength=850).pack(fill="x")
        self.progress = ttk.Progressbar(bottom, mode="indeterminate")
        self.progress.pack(fill="x", pady=(6, 0))

        controls = tk.Frame(frame, bg=bg)
        controls.pack(side="bottom", fill="x", pady=8)
        for label, device, action in (("Thủ công", "mode", "manual"), ("Tự động", "mode", "auto"),
                                      ("Bật đèn", "light", "on"), ("Tắt đèn", "light", "off"),
                                      ("Bật bơm", "pump", "on"), ("Tắt bơm", "pump", "off")):
            ttk.Button(controls, text=label, command=lambda d=device, a=action: self._manual(d, a)).pack(side="left", padx=2)
        self.fallback = ttk.Frame(frame)
        self.fallback.pack(side="bottom", fill="x")
        self.typed = ttk.Entry(self.fallback)
        self.typed.pack(side="left", fill="x", expand=True)
        self.typed.bind("<Return>", lambda _event: self._typed())
        self.typed_button = ttk.Button(self.fallback, text="Gửi văn bản", command=self._typed)
        self.typed_button.pack(side="left", padx=5)

        center = tk.Frame(frame, bg=bg)
        center.pack(fill="both", expand=True)
        self.record_button = tk.Button(center, text="🎙\nGHI ÂM", command=self._record, bg=accent, fg="white",
                                       activebackground="#19452f", activeforeground="white", disabledforeground="#d6e2d9",
                                       font=("Segoe UI Symbol", 20, "bold"), relief="flat", padx=32, pady=14,
                                       cursor="hand2", takefocus=True)
        self.record_button.pack(expand=True)
        settings = ttk.Frame(center)
        settings.pack(side="bottom", fill="x", pady=4)
        ttk.Label(settings, text="Microphone:").pack(side="left")
        self.microphones = []
        self.mic = ttk.Combobox(settings, state="readonly", width=28)
        self.mic.pack(side="left", padx=5)
        ttk.Button(settings, text="Làm mới", command=self._refresh_mics).pack(side="left")
        self.test_button = ttk.Button(settings, text="Thử micro", command=self._test_mic)
        self.test_button.pack(side="left", padx=4)
        self.tts_enabled = tk.BooleanVar(value=False)
        ttk.Checkbutton(settings, text="Đọc phản hồi", variable=self.tts_enabled).pack(side="left", padx=5)
        self._refresh_mics()

    def _manual(self, device: str, action: str) -> None:
        if self.busy:
            return

        def worker() -> None:
            try:
                from backend import send_lamp, send_mode, send_pump
                if device == "light":
                    send_lamp(os.getenv("ESP32_BASE_URL", ""), action)
                    text = "ESP32 xác nhận đã " + ("bật" if action == "on" else "tắt") + " đèn."
                elif device == "pump":
                    send_pump(os.getenv("ESP32_BASE_URL", ""), action)
                    text = "ESP32 xác nhận đã " + ("bật" if action == "on" else "tắt") + " bơm."
                else:
                    send_mode(os.getenv("ESP32_BASE_URL", ""), action)
                    text = "ESP32 xác nhận đã chuyển chế độ " + ("tự động" if action == "auto" else "thủ công") + "."
                self.events.put(("answer", text))
                self.events.put(("status", "Ready. Hoàn tất."))
            except Exception:
                self.events.put(("error", "Không thể gửi lệnh đến ESP32."))

        self._start(worker)

    def _poll_status(self) -> None:
        def worker() -> None:
            try:
                from backend import fetch_status
                self.events.put(("sensor", fetch_status(os.getenv("ESP32_BASE_URL", ""))))
            except Exception:
                self.events.put(("sensor_error", None))
        if not self.polling:
            self.polling = True
            threading.Thread(target=worker, daemon=True).start()
        self.root.after(5000, self._poll_status)

    def _refresh_mics(self) -> None:
        try:
            import sounddevice as sd
            self.microphones = [(i, d["name"]) for i, d in enumerate(sd.query_devices()) if d["max_input_channels"] > 0]
            self.mic["values"] = [f"{i}: {name}" for i, name in self.microphones]
            if self.microphones:
                self.mic.current(0)
            else:
                self.mic.set("Không tìm thấy microphone")
        except Exception:
            self.microphones = []
            self.mic.set("Không thể liệt kê microphone")

    def _selected_mic(self) -> Optional[int]:
        index = self.mic.current() if hasattr(self, "mic") else -1
        return self.microphones[index][0] if index >= 0 and index < len(self.microphones) else None

    def _test_mic(self) -> None:
        device = self._selected_mic()
        def worker() -> None:
            try:
                self.events.put(("status", "Hãy nói để thử microphone…"))
                record_wav(5, device=device)
                self.events.put(("status", "Ready. Microphone hoạt động."))
            except Exception:
                self.events.put(("input_error", "Không thể thử microphone"))
        self._start(worker)

    def _config(self) -> AppConfig:
        return AppConfig(os.getenv("ELEVENLABS_API_KEY", ""), os.getenv("ELEVENLABS_VOICE_ID", ""),
                         os.getenv("OPENAI_API_KEY", ""), os.getenv("OPENAI_BASE_URL", DEFAULT_OPENAI_BASE),
                         os.getenv("OPENAI_MODEL", "gpt-4o-mini"))

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
        self._set_text(self.response, "")
        self.status.set("Đang xử lý…")
        try:
            threading.Thread(target=worker, daemon=True).start()
        except Exception:
            self.busy = False
            self.record_button.configure(state="normal")
            self.typed_button.configure(state="normal")
            self.status.set("Không thể bắt đầu xử lý.")

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
        device = self._selected_mic()
        def worker() -> None:
            try:
                self.events.put(("status", "Đang ghi âm; tự dừng khi im lặng…"))
                audio = record_wav(self.RECORD_SECONDS, device=device)
            except Exception:
                self.events.put(("input_error", "Không thể ghi âm"))
                return
            try:
                self.events.put(("status", "Đang nhận dạng giọng nói tiếng Việt…"))
                text = transcribe_audio(audio, config.elevenlabs_key)
                if not text or not text.strip():
                    raise ValueError("Không nhận được nội dung giọng nói")
            except Exception:
                self.events.put(("input_error", "Không thể nhận dạng giọng nói"))
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
            from backend import fetch_status, fetch_weather, send_lamp, send_mode, send_pump
            device = command["device"]
            if device == "light":
                try:
                    send_lamp(os.getenv("ESP32_BASE_URL", ""), command["action"])
                    response = "ESP32 xác nhận đã " + ("bật" if command["action"] == "on" else "tắt") + " đèn; điều khiển tự động có thể thay đổi lại."
                except Exception:
                    response = "Không xác nhận được lệnh đèn; trạng thái thực tế chưa rõ."
            elif device == "pump":
                try:
                    send_pump(os.getenv("ESP32_BASE_URL", ""), command["action"])
                    response = "ESP32 xác nhận đã " + ("bật" if command["action"] == "on" else "tắt") + " bơm; điều khiển tự động có thể thay đổi lại."
                except Exception:
                    response = "Không xác nhận được lệnh bơm; trạng thái thực tế chưa rõ."
            elif device == "mode":
                try:
                    send_mode(os.getenv("ESP32_BASE_URL", ""), command["action"])
                    response = "ESP32 xác nhận đã chuyển chế độ " + ("tự động" if command["action"] == "auto" else "thủ công") + "."
                except Exception:
                    response = "Không xác nhận được lệnh chế độ; trạng thái thực tế chưa rõ."
            elif device == "sensor":
                try:
                    data = fetch_status(os.getenv("ESP32_BASE_URL", ""))
                    response = (f"Ánh sáng: {data['light'].get('lux')} lux; "
                                f"độ ẩm đất: {data['soil'].get('percent')}%; nước: {data['water'].get('percent')}%; "
                                f"nhiệt độ chip DS3231: {data.get('ds3231_temperature_c')} °C.")
                except Exception:
                    response = "Không thể đọc cảm biến ESP32."
            elif device == "weather":
                try:
                    response = fetch_weather(os.getenv("WEATHER_LATITUDE"), os.getenv("WEATHER_LONGITUDE"))
                except Exception:
                    response = "Không thể lấy dự báo thời tiết; kiểm tra tọa độ và kết nối."
            else:
                response = command_response(command)
            self.events.put(("answer", response))
            if self.command_output_hook and device in ("light", "pump", "mode"):
                self.command_output_hook(command)
            if do_tts:
                self.events.put(("status", "Đang tạo và phát phản hồi…"))
                try:
                    audio = synthesize_speech(response, config)
                    play_wav(audio)
                except Exception:
                    self.events.put(("tts_error", "Dịch vụ đọc phản hồi không khả dụng"))
                    return
            self.events.put(("status", "Ready. Hoàn tất."))
        except Exception:
            self.events.put(("error", "Dịch vụ xử lý không khả dụng"))

    def _poll_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "transcript":
                    self.transcript.set(payload)
                elif kind == "sensor":
                    self.polling = False
                    self.sensor_data = payload
                    self.sensor_time = time.monotonic()
                    self.connection.set("ESP32 đã kết nối")
                    def value(item: Any, unit: str = "") -> str:
                        return f"{item}{unit}" if item is not None else "Không khả dụng"
                    def state(item: Any) -> str:
                        return ("Bật" if item else "Tắt") if isinstance(item, bool) else "Không khả dụng"
                    mode_str = payload.get('mode', 'Không khả dụng')
                    mode_display = "TỰ ĐỘNG" if mode_str == "auto" else "THỦ CÔNG" if mode_str == "manual" else mode_str
                    self.readings.set(f"Ánh sáng: {value(payload.get('light', {}).get('lux'), ' lux')}\n"
                                      f"Đất: {value(payload.get('soil', {}).get('percent'), '%')}   Nước: {value(payload.get('water', {}).get('percent'), '%')}\n"
                                      f"Nhiệt độ chip DS3231 (không nhất thiết là nhiệt độ không khí): {value(payload.get('ds3231_temperature_c'), ' °C')}\n"
                                      f"Đèn: {state(payload.get('lamp'))}   Bơm: {state(payload.get('pump'))}   Chế độ: {mode_display}")
                elif kind == "sensor_error":
                    self.polling = False
                    self.connection.set("ESP32 không khả dụng; dữ liệu cũ nếu có")
                elif kind == "answer":
                    self._set_text(self.response, payload)
                elif kind == "result":
                    _, command = payload
                    self._set_text(self.response, command_response(command))
                elif kind == "status":
                    self.status.set(payload.removeprefix("Ready. ") if payload.startswith("Ready") else payload)
                elif kind == "input_error":
                    self.fallback.pack(fill="x", pady=5, before=self.response.master)
                    self.status.set(payload + ". Vui lòng nhập yêu cầu bằng văn bản.")
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
