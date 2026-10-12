"""S2S2 testbench backend (started by tools/testbench/server.py)."""
import os
import sys

_TOOLS = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for _d in ("common", "config", "testbench"):                      # shared modules, the menuconfig core, saf_tool
    _p = os.path.join(_TOOLS, _d)
    if _p not in sys.path:
        sys.path.insert(0, _p)
