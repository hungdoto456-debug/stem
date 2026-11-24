# Voice sensor dashboard

Configure the values shown in `.env.example` in a private `.env` file, then run:

```sh
python -m pip install -r requirements.txt
python app.py
```

Set `ESP32_BASE_URL` to the ESP32 address and `WEATHER_LATITUDE` / `WEATHER_LONGITUDE` to the forecast location. Service credentials are read from environment variables and are not displayed in the GUI. Do not commit `.env`.

The dashboard polls `GET /api/status` every five seconds. The DS3231 temperature is the RTC chip temperature, not necessarily air temperature. The microphone selector and test button are in the microphone section. Recording stops after silence or 15 seconds; if recognition fails, type a request in the fallback field. Response audio is optional.

Vietnamese voice requests can query sensor readings, request an Open-Meteo forecast, control the lamp and pump, or select auto/manual mode. The recognized transcript is displayed and sent to the configured OpenAI-compatible chat API. Manual controls are also available in the Python app and ESP32 web UI. Select manual mode before sending on/off commands. Manual pump commands bypass sensor and cooldown checks; the pump still stops after 30 seconds. An ESP32 acknowledgement does not prove the physical relay or load operated.

The ESP32 API is unauthenticated plain HTTP: use a trusted network and do not expose it to the Internet. Forecasts use the external Open-Meteo service; the ESP32 does not provide weather forecasts or an ambient-air temperature sensor. API, microphone, and hardware operations require live verification.

Run tests with `python -m pytest -q tests` after installing `requirements-dev.txt`.
