import io
import queue
import sys
import wave
from pathlib import Path
from unittest.mock import Mock

import pytest
import requests

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import app


CLARIFY = {"device": "clarification", "action": "required"}


@pytest.fixture
def config():
    return app.AppConfig("eleven-key", "voice-id", "chat-key", "https://example.test/v1/", "custom-model")


def response(payload=None, content=b"\x00\x00"):
    result = Mock()
    result.json.return_value = payload
    result.content = content
    return result


def chat(content):
    return {"choices": [{"message": {"content": content}}]}


@pytest.mark.parametrize("value, expected", [
    ('{"device":"light","action":"on"}', {"device": "light", "action": "on"}),
    ({"device": "light", "action": "off"}, {"device": "light", "action": "off"}),
    (CLARIFY, CLARIFY),
    ("not json", CLARIFY),
    ('```json\n{"device":"light","action":"on"}\n```', CLARIFY),
    ({"device": "pump", "action": "on"}, CLARIFY),
    ({"device": "light", "action": "toggle"}, CLARIFY),
    ({"device": "light", "action": "on", "execute": True}, CLARIFY),
    ({"device": ["light"], "action": "on"}, CLARIFY),
    ({"device": "light", "action": {"on": True}}, CLARIFY),
    ([], CLARIFY),
    (None, CLARIFY),
])
def test_validate_command_allowlist(value, expected):
    assert app.validate_command(value) == expected


def test_custom_endpoint_and_chat_request(config):
    session = Mock()
    session.post.return_value = response(chat('{"device":"light","action":"on"}'))
    assert app.interpret_transcript("bật đèn", config, timeout=9, session=session) == {"device": "light", "action": "on"}
    args, kwargs = session.post.call_args
    assert args[0] == "https://example.test/v1/chat/completions"
    assert kwargs["headers"]["Authorization"] == "Bearer chat-key"
    assert kwargs["timeout"] == 9
    assert kwargs["json"]["model"] == "custom-model"
    assert "bật đèn" in kwargs["json"]["messages"][-1]["content"]
    session.post.return_value.raise_for_status.assert_called_once()


@pytest.mark.parametrize("base", ["https://example.test/v1", "https://example.test/v1/", "https://example.test/v1/chat/completions", "https://example.test/v1/chat/completions/"])
def test_endpoint_normalization(base):
    assert app.normalize_base_url(base) == "https://example.test/v1"


def test_empty_transcript_makes_no_request(config):
    session = Mock()
    assert app.interpret_transcript("  ", config, session=session) == CLARIFY
    session.post.assert_not_called()


@pytest.mark.parametrize("payload", [{}, {"choices": []}, {"choices": [{"message": {}}]}])
def test_missing_chat_content_is_error(config, payload):
    session = Mock()
    session.post.return_value = response(payload)
    with pytest.raises(ValueError, match="content"):
        app.interpret_transcript("bật đèn", config, session=session)


def test_chat_http_errors_propagate(config):
    session = Mock()
    session.post.return_value = response()
    session.post.return_value.raise_for_status.side_effect = requests.HTTPError("service error")
    with pytest.raises(requests.HTTPError):
        app.interpret_transcript("bật đèn", config, session=session)


def test_missing_chat_key_no_request():
    session = Mock()
    with pytest.raises(ValueError, match="key"):
        app.interpret_transcript("bật đèn", app.AppConfig("", "", ""), session=session)
    session.post.assert_not_called()


def test_stt_sends_audio_and_returns_text():
    session = Mock()
    session.post.return_value = response({"text": " bật đèn "})
    assert app.transcribe_audio(b"wav", " key ", timeout=3, session=session) == "bật đèn"
    args, kwargs = session.post.call_args
    assert args[0] == app.DEFAULT_STT_URL
    assert kwargs["files"]["file"][1] == b"wav"
    assert kwargs["data"]["language_code"] == "vi"
    assert kwargs["headers"]["xi-api-key"] == "key"
    assert kwargs["timeout"] == 3


@pytest.mark.parametrize("payload", [{}, {"text": " "}, {"text": None}, []])
def test_stt_empty_result_is_error(payload):
    session = Mock()
    session.post.return_value = response(payload)
    with pytest.raises(ValueError, match="no text"):
        app.transcribe_audio(b"wav", "key", session=session)


def test_tts_returns_decodable_wav(config):
    session = Mock()
    session.post.return_value = response(content=b"\x00\x00\x01\x00")
    audio = app.synthesize_speech("xin chào", config, timeout=4, session=session)
    with wave.open(io.BytesIO(audio), "rb") as wav:
        assert wav.getnchannels() == 1
        assert wav.getframerate() == 22050
        assert wav.readframes(2) == b"\x00\x00\x01\x00"
    assert session.post.call_args.kwargs["timeout"] == 4


def test_tts_missing_credentials_no_request():
    session = Mock()
    with pytest.raises(ValueError, match="required"):
        app.synthesize_speech("hello", app.AppConfig("", "", "key"), session=session)
    session.post.assert_not_called()


def test_unsupported_command_never_reaches_hook_or_device(config, monkeypatch):
    session = Mock()
    session.post.return_value = response(chat('{"device":"pump","action":"on"}'))
    instance = app.VoiceApp.__new__(app.VoiceApp)
    instance.events = queue.Queue()
    instance._config = lambda: config
    instance.command_output_hook = Mock()
    monkeypatch.setattr(app, "interpret_transcript", lambda text, cfg: app.interpret_transcript_original(text, cfg, session=session))
    monkeypatch.setattr(app, "interpret_transcript_original", interpret_original, raising=False)
    monkeypatch.setattr(app, "play_wav", Mock())
    instance._interpret("bật bơm", config, do_tts=False)
    instance.command_output_hook.assert_not_called()
    app.play_wav.assert_not_called()
    assert instance.events.get_nowait() == ("result", ("bật bơm", CLARIFY))
    assert "No device" in instance.events.get_nowait()[1]
    assert session.post.call_count == 1


interpret_original = app.interpret_transcript


def test_stt_failure_allows_explicit_typed_fallback(config, monkeypatch):
    instance = app.VoiceApp.__new__(app.VoiceApp)
    instance.events = queue.Queue()
    instance._config = lambda: config
    instance._start = lambda worker: worker()
    instance.busy = False
    instance.tts_enabled = Mock()
    instance.tts_enabled.get.return_value = False
    instance.typed = Mock()
    instance.typed.get.return_value = "bật đèn"
    instance.command_output_hook = None
    monkeypatch.setattr(app, "record_wav", lambda *args: b"wav")
    monkeypatch.setattr(app, "transcribe_audio", Mock(side_effect=ValueError("STT failed")))
    interpret = Mock(return_value={"device": "light", "action": "on"})
    monkeypatch.setattr(app, "interpret_transcript", interpret)
    monkeypatch.setattr(app, "synthesize_speech", Mock(return_value=b"audio"))
    monkeypatch.setattr(app, "play_wav", Mock())

    instance._record()
    assert instance.events.get_nowait()[0] == "status"
    assert instance.events.get_nowait()[0] == "status"
    kind, error = instance.events.get_nowait()
    assert kind == "input_error" and "STT failed" in error
    interpret.assert_not_called()

    instance._typed()
    interpret.assert_called_once_with("bật đèn", config)
    assert instance.events.get_nowait() == ("transcript", "bật đèn")
    assert instance.events.get_nowait()[0] == "status"
    assert instance.events.get_nowait() == ("result", ("bật đèn", {"device": "light", "action": "on"}))
    app.synthesize_speech.assert_not_called()
    app.play_wav.assert_not_called()


def test_worker_reports_errors_without_hook(config, monkeypatch):
    instance = app.VoiceApp.__new__(app.VoiceApp)
    instance.events = queue.Queue()
    instance._config = lambda: config
    instance.command_output_hook = Mock()
    monkeypatch.setattr(app, "interpret_transcript", Mock(side_effect=ValueError("bad response")))
    instance._interpret("bật đèn", config, do_tts=False)
    assert instance.events.get_nowait() == ("error", "bad response")
    instance.command_output_hook.assert_not_called()
