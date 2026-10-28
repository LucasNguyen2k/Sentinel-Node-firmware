import pytest
from edge_server import EdgeServer


@pytest.fixture(scope="module")
def edge(tmp_path_factory):
    """A running Edge Server for one test module; fails the module if the server logged a traceback."""
    srv = EdgeServer(tmp_path_factory.mktemp("edge")).start()
    yield srv
    srv.stop()
    log = srv.log()
    assert "Traceback" not in log, "Edge Server raised an exception:\n" + log[-4000:]


@pytest.fixture
def make_node(edge):
    """Factory for emulated nodes bound to 127.0.0.x; all are stopped after the test."""
    from node_emulator import NodeEmulator
    nodes = []

    def _make(octet, **kw):
        n = NodeEmulator(ip="127.0.0.%d" % octet, reg_port=edge.reg_port, mac="00E04C0000%02X" % octet, **kw)
        nodes.append(n)
        return n
    yield _make
    for n in nodes:
        n.stop()
