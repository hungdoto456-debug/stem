from unittest.mock import Mock
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import backend
import pytest
import requests


def test_lamp_acknowledgement():
    session = Mock()
    session.post.return_value.json.return_value = {"ok": True, "lamp": True}
    backend.send_lamp("http://esp32", "on", session)
    assert session.post.call_args.kwargs["data"] == {"command": "lamp_on"}
    with pytest.raises(ValueError):
        backend.send_lamp("http://esp32", "off", session)
    with pytest.raises(ValueError):
        backend.send_lamp("http://esp32", "pump_on", session)

def test_mode_acknowledgement():
    session = Mock()
    session.post.return_value.json.return_value = {"ok": True, "mode": "auto"}
    backend.send_mode("http://esp32", "auto", session)
    assert session.post.call_args.kwargs["data"] == {"mode": "auto"}
    with pytest.raises(ValueError):
        backend.send_mode("http://esp32", "invalid", session)
    session.post.return_value.json.return_value = {"ok": True, "mode": "manual"}
    backend.send_mode("http://esp32", "manual", session)
    assert session.post.call_args.kwargs["data"] == {"mode": "manual"}
    session.post.assert_called_with("http://esp32/api/mode", data={"mode": "manual"}, timeout=5, allow_redirects=False)


@pytest.mark.parametrize("payload", [{"ok": False, "mode": "auto"}, {"ok": True, "mode": "manual"}, {}, [], {"ok": 1, "mode": "auto"}])
def test_mode_rejects_bad_acknowledgement(payload):
    session = Mock()
    session.post.return_value.json.return_value = payload
    with pytest.raises(ValueError, match="xác nhận"):
        backend.send_mode("http://esp32", "auto", session)


@pytest.mark.parametrize("url", ["", "ftp://esp32", "http://", "http://esp32:bad", "http://esp32/path", "http://esp32?q=1"])
def test_mode_rejects_invalid_url(url):
    session = Mock()
    with pytest.raises(ValueError):
        backend.send_mode(url, "auto", session)
    session.post.assert_not_called()


def test_mode_network_and_json_errors():
    session = Mock()
    session.post.side_effect = requests.Timeout()
    with pytest.raises(TimeoutError, match="thời gian chờ"):
        backend.send_mode("http://esp32", "auto", session)
    session.post.side_effect = None
    session.post.return_value.raise_for_status.side_effect = requests.HTTPError("400")
    with pytest.raises(requests.HTTPError):
        backend.send_mode("http://esp32", "auto", session)
    session.post.return_value.raise_for_status.side_effect = None
    session.post.return_value.json.side_effect = ValueError("invalid JSON")
    with pytest.raises(ValueError, match="không hợp lệ"):
        backend.send_mode("http://esp32", "auto", session)


def test_weather_rejects_invalid_coordinates():
    session = Mock()
    with pytest.raises(ValueError):
        backend.fetch_weather("nan", "1", session)
    session.get.assert_not_called()
