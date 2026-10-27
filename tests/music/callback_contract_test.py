"""Static guard for the production callback's no-lock/no-wait contract."""
from pathlib import Path
import re

source = (Path(__file__).resolve().parents[2] / "src/music/player.cpp").read_text()
body = source.split("static void SDLCALL mix(", 1)[1]
body = re.sub(r"//[^\n]*|/\*.*?\*/", "", body, flags=re.S)
start = body.index("{")
depth = 0
for end in range(start, len(body)):
    depth += (body[end] == "{") - (body[end] == "}")
    if depth == 0:
        break
callback = body[start:end + 1]
for forbidden in ("mutex", "lock_guard", "unique_lock", "yield(", "sleep", ".wait(",
                  "SDL_Lock", "Mix_", "decode(", "resize(", "push_back(", "new ", "delete "):
    assert forbidden not in callback, f"callback violates nonblocking contract: {forbidden}"
assert "compare_exchange_strong" in callback, "callback must use a single, non-retrying admission attempt"
assert "is_always_lock_free" in source, "atomic lock-freedom must be a compile-time contract"
print("Production callback nonblocking source contract passed")
