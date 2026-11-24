"""ESP32 and Open-Meteo network operations."""
import logging
import math
from urllib.parse import urlsplit

import requests

logger = logging.getLogger("voice_app")


def _validate_base_url(base_url):
    parsed = urlsplit((base_url or "").strip())
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password or parsed.path not in ("", "/") or parsed.query or parsed.fragment or any(c.isspace() for c in parsed.netloc):
        raise ValueError("ESP32_BASE_URL không hợp lệ; dùng URL http(s) của ESP32")
    try:
        parsed.port
    except ValueError as exc:
        raise ValueError("ESP32_BASE_URL không hợp lệ") from exc
    return parsed.geturl().rstrip("/")


def _post_json(base_url, path, data, session):
    try:
        response = session.post(base_url + path, data=data, timeout=5, allow_redirects=False)
    except requests.Timeout as exc:
        raise TimeoutError("ESP32 không phản hồi trong thời gian chờ") from exc
    except requests.RequestException as exc:
        raise RuntimeError("Không thể kết nối ESP32") from exc
    response.raise_for_status()
    try:
        return response.json()
    except (ValueError, TypeError) as exc:
        raise ValueError("ESP32 trả về dữ liệu không hợp lệ") from exc


def fetch_status(base_url, session=requests):
    base_url = _validate_base_url(base_url)
    logger.info("ESP32 status request: url=%s", base_url)
    try:
        response = session.get(base_url + "/api/status", timeout=5)
    except requests.Timeout as exc:
        raise TimeoutError("ESP32 không phản hồi trong thời gian chờ") from exc
    except requests.RequestException as exc:
        raise RuntimeError("Không thể kết nối ESP32") from exc
    logger.info("ESP32 status response: status=%d, body=%s", response.status_code, response.text[:200])
    response.raise_for_status()
    data = response.json()
    if not isinstance(data, dict) or any(not isinstance(data.get(k), dict) for k in ("light", "soil", "water")):
        raise ValueError("Dữ liệu ESP32 không hợp lệ")
    return data


def send_lamp(base_url, action, session=requests):
    if action not in ("on", "off"):
        raise ValueError("Lệnh đèn không hợp lệ")
    data = _post_json(_validate_base_url(base_url), "/api/command", {"command": "lamp_" + action}, session)
    if not isinstance(data, dict) or data.get("ok") is not True or data.get("lamp") is not (action == "on"):
        raise ValueError("ESP32 không xác nhận trạng thái đèn yêu cầu")
    return data


def send_pump(base_url, action, session=requests):
    if action not in ("on", "off"):
        raise ValueError("Lệnh bơm không hợp lệ")
    data = _post_json(_validate_base_url(base_url), "/api/command", {"command": "pump_" + action}, session)
    if not isinstance(data, dict) or data.get("ok") is not True or data.get("pump") is not (action == "on"):
        raise ValueError("ESP32 không xác nhận trạng thái bơm yêu cầu")
    return data


def send_mode(base_url, mode, session=requests):
    """Set the ESP32 control mode and require its exact acknowledgement."""
    if mode not in ("auto", "manual"):
        raise ValueError("Chế độ không hợp lệ")
    data = _post_json(_validate_base_url(base_url), "/api/mode", {"mode": mode}, session)
    if not isinstance(data, dict) or data.get("ok") is not True or data.get("mode") != mode:
        raise ValueError("ESP32 không xác nhận chế độ yêu cầu")
    return data


def fetch_weather(latitude, longitude, session=requests):
    try:
        lat, lon = float(latitude), float(longitude)
    except (TypeError, ValueError) as exc:
        raise ValueError("Chưa cấu hình tọa độ dự báo") from exc
    if not math.isfinite(lat) or not math.isfinite(lon) or not -90 <= lat <= 90 or not -180 <= lon <= 180:
        raise ValueError("Tọa độ dự báo không hợp lệ")
    logger.info("Weather request: lat=%s, lon=%s", lat, lon)
    response = session.get("https://api.open-meteo.com/v1/forecast", params={"latitude": lat, "longitude": lon, "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max", "timezone": "auto", "forecast_days": 1}, timeout=10)
    logger.info("Weather response: status=%d, body=%s", response.status_code, response.text[:200])
    response.raise_for_status()
    data = response.json()
    try:
        daily = data["daily"]
        values = [daily[key][0] for key in ("temperature_2m_min", "temperature_2m_max", "precipitation_probability_max")]
        if any(not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v) for v in values):
            raise ValueError()
    except (KeyError, IndexError, TypeError, ValueError) as exc:
        raise ValueError("Dữ liệu dự báo không khả dụng") from exc
    return f"Dự báo hôm nay: nhiệt độ từ {values[0]} đến {values[1]} °C, xác suất mưa tối đa {values[2]}%."
