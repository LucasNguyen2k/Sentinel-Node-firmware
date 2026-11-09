"""A node renamed with SET name / SAVE / REBOOT re-registers under its new name: the driver must take it over."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "common"))
from edge_server import EdgeServer        # noqa: E402
from node_emulator import NodeEmulator    # noqa: E402


def test_new_name_is_taken_over_on_reregistration(tmp_path):
    srv = EdgeServer(str(tmp_path), api_key="k").start()
    node = NodeEmulator(ip="127.0.0.95", reg_port=srv.reg_port, name="EZ_SN_A", mac="00E04C000095", fw="1.6.1",
                        hb_s=1).start()
    try:
        names = lambda: [d["name"] for d in srv.get("/devices").json() if d["ip"] == "127.0.0.95"]
        deadline = time.time() + 15
        while time.time() < deadline and names() != ["EZ_SN_A"]:
            time.sleep(0.3)
        assert names() == ["EZ_SN_A"]
        node.name = "EZ_SN_B"                      # the node now registers with its new name (as after a reboot)
        node.register()
        deadline = time.time() + 10
        while time.time() < deadline and names() != ["EZ_SN_B"]:
            time.sleep(0.3)
        assert names() == ["EZ_SN_B"], names()
        folders = sorted(f for f in os.listdir(str(tmp_path)) if f.endswith("_127.0.0.95"))
        assert folders == ["EZ_SN_A_127.0.0.95"], folders          # the folder in use is kept
    finally:
        node.stop()
        srv.stop()
