"""tb.runner"""

import threading
from tb.bus import BUS, log
from tb import tests_extra
from tb.e2e import test_e2e
from tb.devices import SDR
from tb.links import GS, OBC, sat_fresh_start
from tb.registry import TESTS
from tb.scenarios import test_scenario
from tb.tests_digi import test_digi
from tb.tests_image import test_image, test_imageloop, test_sdrimage
from tb.tests_link import test_cwtest, test_link, test_rate, test_sdr
from tb.state import RUN, set_result
from tb.util import now
from tb import cubus
from tb.compat import cubus_on

def start_test(name, cfg):
    """Run one test in a thread (only one at a time). Clears the satellite replay floor first (RESET frame)."""
    if not RUN["lock"].acquire(blocking=False):
        raise RuntimeError(f"test '{RUN['name']}' is still running")
    need = tests_extra.NEEDS.get(name)
    if cubus_on() and name in cubus.CUBUS_BLOCKED:
        RUN["lock"].release()
        raise RuntimeError(f"not available with the CUBUS profile: this test {cubus.CUBUS_BLOCKED[name]}. The flight CPU1 runs the OBC firmware, not that app. "
                           "Use the DEBUG section (CUBUS receive test, SWD snapshot) and the ground-station tests (cmdconf, beacontime, winmap, soak).")
    if need == "gs" and not GS.connected():
        RUN["lock"].release()
        raise RuntimeError("connect the GS serial port first")
    if need == "obc" and not OBC.connected():
        RUN["lock"].release()
        raise RuntimeError("connect the Nucleo's VCP port first")
    if need is None and name not in ("cwtest", "sdrimage", "imageloop", "digi", "e2e") and not GS.connected():     # these tests only need the Nucleo (and the SDR); the GS is just a cross-check
        RUN["lock"].release()
        raise RuntimeError("connect the GS serial port first")
    RUN["abort"].clear()
    RUN["name"] = name

    def work():
        BUS.publish("test", name=name, state="running")
        try:
            if cfg.get("fresh", need is None) and OBC.connected():
                sat_fresh_start(RUN["abort"])
            TESTS[name](cfg, RUN["abort"])
        except Exception as e:
            log(f"{name} test crashed: {e}", "err")
            set_result(name, {"test": name, "when": now(), "verdict": "FAIL", "why": str(e)})
        finally:
            SDR.stop()
            BUS.publish("test", name=name, state="done")
            RUN["name"] = None
            RUN["lock"].release()

    RUN["thread"] = threading.Thread(target=work, daemon=True)
    RUN["thread"].start()


TESTS.update({"link": test_link, "image": test_image, "sdr": test_sdr, "rate": test_rate, "cwtest": test_cwtest, "sdrimage": test_sdrimage,
              "imageloop": test_imageloop, "digi": test_digi, "e2e": test_e2e})
TESTS.update(tests_extra.make_tests())
TESTS.update(cubus.TESTS)
tests_extra.NEEDS.update(cubus.NEEDS)
TESTS["scenario"] = test_scenario
