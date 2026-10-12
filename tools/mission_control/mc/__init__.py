"""S2S2 Mission Control backend (started by tools/mission_control/server.py)."""
import os
import sys

_TOOLS = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
_p = os.path.join(_TOOLS, "common")
if _p not in sys.path:
    sys.path.insert(0, _p)
