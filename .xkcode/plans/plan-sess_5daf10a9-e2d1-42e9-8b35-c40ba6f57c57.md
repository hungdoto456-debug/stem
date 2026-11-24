## Context
The firmware already exposes manual lamp and pump commands via POST /api/command, but the web UI and Python Tkinter dashboard lack buttons to send them. The user wants manual controls that remain active until Auto mode is explicitly selected, with pump safety limits still enforced.

## Implementation
1. Add a new manual/auto mode setting to the persisted Config in include/logic.h and firmware main.cpp, defaulting to Auto mode. Preserve existing Config layout and checksum compatibility.
2. In main.cpp, add a new POST /api/mode endpoint to switch between manual and auto modes. In auto mode, lamp and pump follow existing thresholds; in manual mode, lamp and pump follow the last manual command and ignore thresholds.
3. Extend the web UI in main.cpp with manual/auto toggle and On/Off buttons for lamp and pump, using the existing /api/command and new /api/mode endpoints. Display current mode and relay states clearly.
4. Extend the Python Tkinter app with manual/auto toggle and On/Off buttons for lamp and pump, using backend.send_lamp() pattern and a new backend.send_mode() function. Ensure UI updates are passed through the event queue.
5. Add tests for the new mode switching and manual command behavior in test/self_check.cpp and voice_app/tests/test_backend.py. Preserve existing automatic control tests.
6. Build the ESP32 firmware and run Python tests. Do not upload.

## Verification
Check that manual mode persists across reboot, commands are respected, safety limits still apply to pump, and automatic control resumes when Auto mode is selected. Test both web and Python interfaces.