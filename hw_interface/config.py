"""Settings, from environment variables HWI_* (defaults are the Phase 1 values)."""
import os
from dataclasses import dataclass, field


def _f(name, default):
    return float(os.getenv(name, default))


@dataclass
class Settings:
    # SN Device Driver (sentinel_node_api.py)
    driver_api: str = field(default_factory=lambda: os.getenv("HWI_DRIVER_API", "http://127.0.0.1:8080/api/v1"))
    driver_key: str = field(default_factory=lambda: os.getenv("HWI_DRIVER_KEY", os.getenv("AMB82_API_KEY", "")))
    driver_data_dir: str = field(default_factory=lambda: os.getenv("HWI_DRIVER_DATA_DIR", os.getenv("AMB82_DATA_DIR", "data")))
    # Edge HAL interface
    data_root: str = field(default_factory=lambda: os.getenv("HWI_DATA_ROOT", "/var/lib/sentinel/data-area"))
    state_db: str = field(default_factory=lambda: os.getenv("HWI_STATE_DB", "/var/lib/sentinel/hw_interface.sqlite"))
    mqtt_host: str = field(default_factory=lambda: os.getenv("HWI_MQTT_HOST", "127.0.0.1"))   # or a Unix socket path
    mqtt_port: int = field(default_factory=lambda: int(os.getenv("HWI_MQTT_PORT", "1883")))
    mqtt_user: str = field(default_factory=lambda: os.getenv("HWI_MQTT_USER", ""))
    mqtt_pass: str = field(default_factory=lambda: os.getenv("HWI_MQTT_PASS", ""))
    mqtt_client_id: str = field(default_factory=lambda: os.getenv("HWI_MQTT_CLIENT_ID", "sentinel-hw-interface"))
    schema_version: str = "1.0"
    # dataset timing (seconds)
    chem01_s: float = field(default_factory=lambda: _f("HWI_CHEM01_S", 120))
    aco01_every_s: float = field(default_factory=lambda: _f("HWI_ACO01_EVERY_S", 300))
    aco01_len_s: float = field(default_factory=lambda: _f("HWI_ACO01_LEN_S", 5))
    img01_every_s: float = field(default_factory=lambda: _f("HWI_IMG01_EVERY_S", 300))
    part_s: float = field(default_factory=lambda: _f("HWI_PART_S", 30))
    event_img_every_s: float = field(default_factory=lambda: _f("HWI_EVENT_IMG_EVERY_S", 60))
    min_event_images: int = field(default_factory=lambda: int(os.getenv("HWI_MIN_EVENT_IMAGES", "2")))
    aco_time_accuracy_ms: int = field(default_factory=lambda: int(os.getenv("HWI_ACO_TIME_ACCURACY_MS", "1000")))
    audio_giveup_s: float = field(default_factory=lambda: _f("HWI_AUDIO_GIVEUP_S", 150))
    capture_timeout_s: float = field(default_factory=lambda: _f("HWI_CAPTURE_TIMEOUT_S", 30))
    retention_s: float = field(default_factory=lambda: _f("HWI_RETENTION_S", 24 * 3600))
    # polling of the SN Device Driver (seconds)
    poll_events_s: float = field(default_factory=lambda: _f("HWI_POLL_EVENTS_S", 0.5))
    poll_records_s: float = field(default_factory=lambda: _f("HWI_POLL_RECORDS_S", 1.0))
    poll_status_s: float = field(default_factory=lambda: _f("HWI_POLL_STATUS_S", 5.0))
    poll_devices_s: float = field(default_factory=lambda: _f("HWI_POLL_DEVICES_S", 10.0))
    chem_end_grace_s: float = field(default_factory=lambda: _f("HWI_CHEM_END_GRACE_S", 3.0))
    # event-log recovery after a node comes back (events whose messages were lost during an outage)
    recovery_window_s: float = field(default_factory=lambda: _f("HWI_RECOVERY_WINDOW_S", 900))
    recovery_every_s: float = field(default_factory=lambda: _f("HWI_RECOVERY_EVERY_S", 10))
    live_lag_s: float = field(default_factory=lambda: _f("HWI_LIVE_LAG_S", 10))
    recovery_quiet_s: float = field(default_factory=lambda: _f("HWI_RECOVERY_QUIET_S", 15))   # no late record for this long = replay done
    late_rows_max: int = field(default_factory=lambda: int(os.getenv("HWI_LATE_ROWS_MAX", "20000")))
    # chemical ADC of the boards (proposal section 5.1); today: internal 10-bit over 3.3 V
    adc_converter: str = field(default_factory=lambda: os.getenv("HWI_ADC_CONVERTER", "internal"))
    adc_bits: int = field(default_factory=lambda: int(os.getenv("HWI_ADC_BITS", "10")))
    adc_vref_v: float = field(default_factory=lambda: _f("HWI_ADC_VREF_V", 3.3))
    adc_divider: float = field(default_factory=lambda: _f("HWI_ADC_DIVIDER", 1.0))
    adc_circuit_supply_v: float = field(default_factory=lambda: _f("HWI_ADC_SUPPLY_V", 3.3))
    ultrasonic_fitted: bool = field(default_factory=lambda: os.getenv("HWI_ULTRASONIC", "0") == "1")

    @property
    def mos_resolution_mv(self) -> float:
        return round(self.adc_vref_v * self.adc_divider / (2 ** self.adc_bits - 1) * 1000, 3)

    def counts_to_mv(self, counts):
        if counts is None or counts < 0:
            return None
        return round(counts / (2 ** self.adc_bits - 1) * self.adc_vref_v * self.adc_divider * 1000, 1)


# config_set may change only these node settings (never network, server or credentials)
CONFIG_SET_ALLOWED = {"sample_ms", "upload_ms", "hb_s", "power", "maint", "gas_alarm_ppm", "gas_clear_ppm",
                      "gas_confirm_ppm", "gas_warmup_s", "ac_th_db", "ac_clear_db", "ac_windows", "fusion_s",
                      "event_hold_s", "thermal_c"}
# never published in the birth message
CONFIG_PRIVATE = {"ssid", "pass", "token", "server", "port", "sock_base", "ntp", "secure_all"}
