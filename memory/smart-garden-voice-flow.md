---
name: smart-garden-voice-flow
description: Smart garden project uses an ESP32 web API plus a Python Tkinter voice dashboard.
metadata:
  type: project
---

The smart garden is split between ESP32 firmware and `voice_app`. ESP32 exposes `/api/status`, `/api/mode`, and `/api/command`; relay outputs are active-low. The Python app records Vietnamese speech, shows the recognized transcript, sends it to an OpenAI-compatible chat-completions API, validates the JSON command, and can control lamp/pump/mode or report sensors.

**Why:** This is the user's ongoing project architecture and workflow.

**How to apply:** Keep the Python side using URL-encoded ESP32 commands, preserve pump safety limits, and never commit `.env` credentials. Related: [[esp32-relay-active-low]].
