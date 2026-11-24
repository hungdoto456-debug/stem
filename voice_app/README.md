# Voice app

This directory contains the optional Python desktop voice-command client for the ESP32 project. It interprets Vietnamese speech or typed text into a deliberately small, validated command format. **It does not connect to, control, or claim to execute commands on the ESP32.**

## Setup

Python 3.10+ is recommended. From this directory:

```text
python -m venv .venv
# Windows: .venv\Scripts\activate
# macOS/Linux: source .venv/bin/activate
python -m pip install -r requirements.txt
```

For development and tests:

```text
python -m pip install -r requirements-dev.txt
pytest -q
```

The app loads environment variables from an optional `.env` file at startup using `python-dotenv`, and also accepts normal process environment variables. Copy `.env.example` to `.env`, fill in the keys, and start with `python app.py`. Values are shown as editable UI defaults and remain in memory; the app does not write credentials back to disk. Never commit `.env` or real keys.

## API configuration

The app expects:

- ElevenLabs speech-to-text: `POST https://api.elevenlabs.io/v1/speech-to-text`, multipart field `file`, with `xi-api-key`; the app requests model `scribe_v1` and explicitly sets `language_code=vi` for Vietnamese.
- OpenAI-compatible chat: `POST <OPENAI_BASE_URL>/chat/completions`, bearer authentication, and a JSON body containing `model`, `temperature: 0`, and `messages`. A custom base URL may include `/chat/completions`; the app normalizes that suffix before appending it.
- ElevenLabs text-to-speech: `POST https://api.elevenlabs.io/v1/text-to-speech/<voice_id>` with `xi-api-key`, `output_format=pcm_22050`, and a JSON body. The raw PCM response is wrapped locally as a WAV file for playback.

All network calls use a timeout. Non-2xx responses are surfaced as errors. The app requires the chat response shape `choices[0].message.content`; malformed content becomes a clarification command rather than an executable command.

## Usage

1. Enter the OpenAI-compatible key, base URL, and model. Enter an ElevenLabs key for speech recognition; a voice ID is only needed for response audio.
2. Type a Vietnamese request, such as `bật đèn`, and choose **Xử lý văn bản**, or choose **Ghi âm 5 giây** to record speech.
3. Review the transcript, validated JSON command, and Vietnamese response in the interface. Response audio is optional and requires ElevenLabs credentials, a voice ID, and a working output device.
4. If speech recognition fails or returns no text, enter the request in the typed-input field and interpret it instead; no microphone or STT credentials are needed for typed input.

Only these validated objects are accepted:

```json
{"device":"light","action":"on"}
{"device":"light","action":"off"}
{"device":"clarification","action":"required"}
```

The GUI presents three stages clearly: the recognized or typed **Transcript**, the model output after allowlist validation as **Validated command**, and the resulting Vietnamese **Response** (including the explicit no-device-execution message). STT errors are shown as errors; they do not trigger an accidental command. Use the typed-input fallback explicitly after an STT failure.

The model is instructed to use Vietnamese and return JSON only. Arbitrary text, extra JSON fields, unsupported devices, and unsupported actions are reduced to clarification. No request is sent to an ESP32 endpoint. The `command_output_hook` is an observation/integration hook for a future owner; this project does not provide a device transport.

## Audio and limitations

Recording requires a working microphone and the optional `sounddevice`/`soundfile` dependencies. Playback requires a working system audio output. Device permissions, PortAudio installation, API credentials, rate limits, network availability, and model behavior can affect the live workflow. Tests mock network clients and do not prove live API, microphone, speaker, or ESP32 behavior.

The ESP32 firmware currently has no direct voice-command endpoint. Any future integration must add an authenticated, separately reviewed transport and preserve command validation and the explicit no-device-execution behavior of this client.
