"""L3 unit tests: pure parsing/helper functions of the Edge Server (no network)."""
import pytest

import sentinel_node_api as srv

pytestmark = pytest.mark.req("FW-10")


@pytest.mark.req("FW-03", "FW-15")
def test_parse_fields_extended_record():
    f = srv._parse_fields("25.16,59.90,161,1791479726,812,-1.0,-43.8,-57.4,-1,1,83".split(","))
    assert f["temp_c"] == 25.16 and f["rh_pct"] == 59.9 and f["gas_raw"] == 161
    assert f["device_ts"] == 1791479726 and f["device_seq"] == 812
    assert f["gas_ppm"] == -1 and f["us"] is None          # ultrasonic not fitted -> None
    assert f["event_state"] == "SUSPECT"
    assert f["flags"] == {"aht_ok": True, "gas_ok": True, "gas_warm": False, "gas_calibrated": False, "mic_ok": True,
                          "us_present": False, "time_synced": True, "maintenance": False, "simulated": False}


@pytest.mark.req("FW-03")
def test_parse_fields_legacy_and_invalid():
    assert srv._parse_fields(["25.1", "4512.00", "312"]) == {"temp_c": 25.1, "rh_pct": 4512, "gas_raw": 312}
    f = srv._parse_fields("-999.00,-999.00,318,0,8,-1.0,-40.0,-37.5,-1,0,94".split(","))
    assert f["temp_c"] is None and f["rh_pct"] is None      # firmware invalid marker
    assert f["device_ts"] is None                            # clock not synced
    assert srv._parse_fields(["1", "2"]) is None             # unknown layout


def test_num_conversions():
    assert srv._num("12") == 12 and isinstance(srv._num("12"), int)
    assert srv._num("1.5") == 1.5
    assert srv._num("-999") is None and srv._num("nan") is None and srv._num("abc") is None


@pytest.mark.req("FW-13")
def test_status_values_typing():
    v = srv._status_values({"rssi": "-29", "int_temp_c": "null", "last_reset": "task_stall:up", "amb_temp_c": "-999.00",
                            "gas_ppm": "-1.0", "fw": "1.3.1"})
    assert v["rssi"] == -29 and v["int_temp_c"] is None and v["last_reset"] == "task_stall:up"
    assert v["amb_temp_c"] is None and v["gas_ppm"] == -1 and v["fw"] == "1.3.1"


@pytest.mark.req("FW-07")
def test_extract_jpeg_from_firmware_envelope():
    jpeg = b"\xff\xd8\xff\xe0" + b"x" * 100 + b"\xff\xd9"
    body = b"------AMB82_Boundary\r\nContent-Disposition: form-data\r\n\r\n" + jpeg + b"\r\n------AMB82_Boundary--\r\n"
    assert srv._extract_jpeg(body) == jpeg
    assert srv._extract_jpeg(b"not a picture") is None


def test_query_and_subnet_helpers():
    assert srv._qs_flat("a=1&b=&c=x%20y") == {"a": "1", "b": "", "c": "x y"}
    assert srv._in_device_subnet("192.168.100.44") is True
    assert srv._in_device_subnet("10.0.0.5") is False and srv._in_device_subnet("bogus") is False


def test_media_file_names():
    assert srv._stamp_to_iso("20261008_164042_460350.jpg").startswith("2026-10-08T16:40:42")
    assert srv._stamp_to_iso("../../etc/passwd") is None
    assert "/" not in srv._safe_name("../evil/name") and srv._safe_name("EZ SYS 01") == "EZ_SYS_01"
