from anomaly_detector.adaptive import pick_profile


def test_pick_cliff_finder_when_cliff_detected():
    r = pick_profile(
        regression=None, health=None, exec_quality=None,
        last_report={"cliff": {"detected": True, "rps": 250000}},
    )
    assert r.profile_name == "cliff-finder"


def test_pick_adversarial_on_regression():
    r = pick_profile(
        regression={"regression": "regressed", "p99_delta_ns": 30_000, "ks_pvalue": 0.001},
        health=None, exec_quality=None,
        last_report={"scores": {"correctness": 99}},
    )
    assert r.profile_name == "adversarial"


def test_pick_adversarial_on_high_slippage():
    r = pick_profile(
        regression=None, health=None,
        exec_quality={"slippage_bps": 15},
        last_report={"scores": {"correctness": 99}},
    )
    assert r.profile_name == "adversarial"


def test_pick_soak_on_low_correctness():
    r = pick_profile(
        regression=None, health=None, exec_quality=None,
        last_report={"scores": {"correctness": 88}},
    )
    assert r.profile_name == "soak"


def test_pick_baseline_on_anomaly_health():
    r = pick_profile(
        regression=None, health={"health": "anomaly"}, exec_quality=None,
        last_report={"scores": {"correctness": 99}},
    )
    assert r.profile_name == "baseline"


def test_pick_baseline_no_prior_data():
    r = pick_profile(regression=None, health=None, exec_quality=None, last_report=None)
    assert r.profile_name == "baseline"


def test_pick_fire_hose_when_all_ok():
    r = pick_profile(
        regression={"regression": "stable", "p99_delta_ns": 0, "ks_pvalue": 0.5},
        health={"health": "ok"},
        exec_quality={"slippage_bps": 1},
        last_report={"scores": {"correctness": 99}, "cliff": {"detected": False}},
    )
    assert r.profile_name == "fire-hose"
