import json
import math
import sys

def scale_even(val, scale):
    return int(round(round(val * scale) / 2.0) * 2)

def compute_dock_position(slot_rect, taskbar_rect, is_horizontal, mini_size, center_in_slot=True):
    # slot_rect: (left, top, right, bottom)
    # taskbar_rect: (left, top, right, bottom)
    # mini_size: (cx, cy)
    slot_left, slot_top, slot_right, slot_bottom = slot_rect
    tb_left, tb_top, tb_right, tb_bottom = taskbar_rect
    mini_cx, mini_cy = mini_size

    if is_horizontal:
        tb_h = tb_bottom - tb_top
        pos_y = tb_top + (tb_h - mini_cy) // 2
        slot_w = slot_right - slot_left
        if center_in_slot and slot_w > mini_cx:
            pos_x = slot_left + (slot_w - mini_cx) // 2
        else:
            pos_x = slot_left
    else:
        tb_w = tb_right - tb_left
        if tb_w > mini_cx:
            pos_x = tb_left + (tb_w - mini_cx) // 2
        else:
            pos_x = tb_left
        slot_h = slot_bottom - slot_top
        if center_in_slot and slot_h > mini_cy:
            pos_y = slot_top + (slot_h - mini_cy) // 2
        else:
            pos_y = slot_top
    return (pos_x, pos_y)

def clamp_to_monitor_bounds(pt, sz, monitor_rect):
    x, y = pt
    cx, cy = sz
    mon_l, mon_t, mon_r, mon_b = monitor_rect
    mon_w = mon_r - mon_l
    mon_h = mon_b - mon_t

    if cx >= mon_w:
        clamped_x = mon_l
    else:
        clamped_x = max(mon_l, min(x, mon_r - cx))

    if cy >= mon_h:
        clamped_y = mon_t
    else:
        clamped_y = max(mon_t, min(y, mon_b - cy))

    return (clamped_x, clamped_y)

def is_near_dock_position(cur, dock, thresh):
    return abs(cur[0] - dock[0]) <= thresh and abs(cur[1] - dock[1]) <= thresh

def test_dock_geometry():
    tb_rect = (0, 1032, 1920, 1080)
    slot_left = (12, 1032, 152, 1080)
    mini_dual = (112, 28)
    mini_single = (56, 28)

    # 1. Exact widget found -> Centered
    dual_c = compute_dock_position(slot_left, tb_rect, True, mini_dual, center_in_slot=True)
    assert dual_c == (26, 1042), f"Expected (26, 1042), got {dual_c}"

    single_c = compute_dock_position(slot_left, tb_rect, True, mini_single, center_in_slot=True)
    assert single_c == (54, 1042), f"Expected (54, 1042), got {single_c}"

    # 2. Geometric fallback -> Left anchored
    dual_a = compute_dock_position(slot_left, tb_rect, True, mini_dual, center_in_slot=False)
    assert dual_a == (12, 1042), f"Expected (12, 1042), got {dual_a}"

    single_a = compute_dock_position(slot_left, tb_rect, True, mini_single, center_in_slot=False)
    assert single_a == (12, 1042), f"Expected (12, 1042), got {single_a}"

    # 3. Vertical taskbar
    tb_v = (0, 0, 60, 1080)
    slot_v = (0, 12, 60, 152)
    vert_c = compute_dock_position(slot_v, tb_v, False, mini_dual, center_in_slot=True)
    assert vert_c == (0, 12 + (140 - 28) // 2), f"Got {vert_c}"

    # 4. Monitor clamping
    mon = (0, 0, 1920, 1080)
    assert clamp_to_monitor_bounds((26, 1042), mini_dual, mon) == (26, 1042)
    assert clamp_to_monitor_bounds((-50, 1042), mini_dual, mon) == (0, 1042)
    assert clamp_to_monitor_bounds((1900, 1042), mini_dual, mon) == (1920 - 112, 1042)
    assert clamp_to_monitor_bounds((26, -20), mini_dual, mon) == (26, 0)
    assert clamp_to_monitor_bounds((26, 1200), mini_dual, mon) == (26, 1080 - 28)
    assert clamp_to_monitor_bounds((100, 100), (2500, 1500), mon) == (0, 0)

    # 5. Snap threshold
    assert is_near_dock_position((26, 1042), (26, 1042), 24) is True
    assert is_near_dock_position((35, 1050), (26, 1042), 24) is True
    assert is_near_dock_position((70, 1042), (26, 1042), 24) is False

    print("Geometric and clamping assertions passed!")

def test_json_config():
    with open("config-default.json", "r", encoding="utf-8") as f:
        data = json.load(f)
    settings = data.get("Settings", {})
    assert "DockToWeather" in settings, "DockToWeather missing in config-default.json"
    assert settings["DockToWeather"] is False, "DockToWeather must be false by default"
    assert settings["MiniMode"] is False, "MiniMode must be false by default"
    print("JSON config assertion passed!")

def test_taskbar_autohide_compensation():
    # When auto-hide is on, taskbar height drops to ~2px
    dpi_scale = 1.0
    collapsed_tb_rect = [0, 1078, 1920, 1080]
    tb_h = collapsed_tb_rect[3] - collapsed_tb_rect[1]
    assert tb_h == 2
    min_tb_h = scale_even(24.0, dpi_scale)
    assert tb_h < min_tb_h

    default_tb_h = scale_even(48.0, dpi_scale)
    mon_bottom = 1080
    recovered_top = mon_bottom - default_tb_h
    recovered_rect = (0, recovered_top, 1920, mon_bottom)
    assert recovered_rect == (0, 1032, 1920, 1080)

    # Docking position inside recovered taskbar
    mini_dual = (112, 28)
    slot = (12, recovered_rect[1], 12 + mini_dual[0], recovered_rect[3])
    pos = compute_dock_position(slot, recovered_rect, True, mini_dual, False)
    assert pos == (12, 1042)
    print("Auto-hide compensation assertion passed!")

def test_multi_monitor():
    # Primary monitor: (0, 0, 1920, 1080), Secondary monitor: (-1920, 0, 0, 1080)
    primary_mon = (0, 0, 1920, 1080)
    secondary_mon = (-1920, 0, 0, 1080)
    mini_dual = (112, 28)

    # Clamping on secondary monitor
    pos_sec = (-1900, 1042)
    clamped_sec = clamp_to_monitor_bounds(pos_sec, mini_dual, secondary_mon)
    assert clamped_sec == (-1900, 1042), f"Expected (-1900, 1042), got {clamped_sec}"

    # Clamping out-of-bounds on secondary monitor left
    pos_sec_off = (-2000, 1042)
    clamped_sec_off = clamp_to_monitor_bounds(pos_sec_off, mini_dual, secondary_mon)
    assert clamped_sec_off == (-1920, 1042), f"Expected (-1920, 1042), got {clamped_sec_off}"

    # Clamping out-of-bounds on secondary monitor right
    pos_sec_right = (-50, 1042)
    clamped_sec_right = clamp_to_monitor_bounds(pos_sec_right, mini_dual, secondary_mon)
    assert clamped_sec_right == (-112, 1042), f"Expected (-112, 1042), got {clamped_sec_right}"

    print("Multi-monitor assertions passed!")

def test_dpi_scaling():
    # 125% DPI scale: 120 DPI
    dpi_scale = 1.25
    tb_h = scale_even(48.0, dpi_scale) # 60
    mini_h = scale_even(28.0, dpi_scale) # 36
    assert tb_h == 60
    assert mini_h == 36

    tb_rect = (0, 1080 - tb_h, 1920, 1080)
    mini_w = scale_even(112.0, dpi_scale) # 140
    mini_sz = (mini_w, mini_h)

    # Margin 12 DIP scaled
    margin_x = scale_even(12.0, dpi_scale) # 16
    slot = (margin_x, tb_rect[1], margin_x + mini_w, tb_rect[3])

    pos = compute_dock_position(slot, tb_rect, True, mini_sz, False)
    assert pos[0] == 16, f"Expected 16, got {pos[0]}"
    # Vertically centered: tb_rect[1] + (60 - 36) // 2 = 1020 + 12 = 1032
    assert pos[1] == tb_rect[1] + 12, f"Expected {tb_rect[1] + 12}, got {pos[1]}"

    # Even-pixel symmetry check: top padding == bottom padding
    top_pad = pos[1] - tb_rect[1]
    bot_pad = tb_rect[3] - (pos[1] + mini_h)
    assert top_pad == bot_pad == 12, f"Expected symmetric padding of 12, got top={top_pad}, bot={bot_pad}"
    print("DPI scaling and pixel symmetry assertions passed!")

if __name__ == "__main__":
    test_dock_geometry()
    test_json_config()
    test_taskbar_autohide_compensation()
    test_multi_monitor()
    test_dpi_scaling()
    print("All python verification tests passed!")
