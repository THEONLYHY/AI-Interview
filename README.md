# AI Interview

A C++17 interview practice application with two frontends over the same
business layer:

- `ai_interview_voice_cli`: microphone/speaker CLI for real-time voice
  sessions.
- `ai_interview_qt`: Qt Widgets shell for selecting a resume and running a
  mock interview session.

Voice validation uses the real PortAudio input/output path or the explicit real
WebSocket smoke command.

## Requirements

- CMake 3.27+
- C++17 compiler
- vcpkg with dependencies from `vcpkg.json`
- Qt 6 Widgets
- PortAudio. On Linux/WSL, system PortAudio can be selected with
  `-DAI_INTERVIEW_USE_SYSTEM_PORTAUDIO=ON`.

## Build

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DAI_INTERVIEW_BUILD_TESTS=ON \
  -DAI_INTERVIEW_USE_SYSTEM_PORTAUDIO=ON \
  -DCMAKE_BUILD_TYPE=Debug

cmake --build build -j 4
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

The current test suite covers protocol framing, interview session question
generation, dialog session state/TTS behavior, and PCM conversion helpers.

## Configuration

`config/default_config.json` is a placeholder example. Copy it to
`config/local_config.json` and fill in real WebSocket and LLM credentials before
running real service flows.

Do not commit `config/local_config.json`.

## Real WebSocket Smoke

The smoke path requires `config/local_config.json` with real service keys.

```bash
./build/ai_interview_voice_cli --real-wss-smoke --text "hello" --wait-seconds 30
```

The smoke command connects, starts a session, sends one text query, waits for a
TTS response or terminal state, then closes the socket.

## Voice CLI

Run a real microphone/speaker session:

```bash
./build/ai_interview_voice_cli --mock-llm
```

```bash
./build/ai_interview_voice_cli --mock-llm doc/resume.pdf
```

Use `--mock-llm` to keep the voice transport real while replacing the text LLM
with deterministic questions. Omit it to use the configured real LLM endpoint.



## Qt UI

```bash
./build/ai_interview_qt
```

The Qt target currently provides a resume selection dialog, mock session start
and stop controls, state display, and transcript updates through the existing
`DialogSession` callbacks.
