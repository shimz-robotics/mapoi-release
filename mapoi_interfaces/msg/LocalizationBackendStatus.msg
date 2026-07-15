# Minimal localization backend readiness, published 1 Hz on
# `mapoi/localization/backend_status` by localization bridges such as
# `mapoi_amcl_localization_bridge` (AMCL adapter) or any custom bridge for
# slam_toolbox / NDT / 自前 localization 等。
#
# Custom bridges only need to populate these three fields. UI consumers
# (mapoi_webui, mapoi_rviz_plugins) gate the Initial Pose UI on
# `backend_ready` alone. Per-capability detail belongs to `reason` or to
# bridge-specific topics, not this message.
#
# Navigation readiness (Nav2 action / select_map service 等) は別契約
# (`NavigationBackendStatus`、issue #198 / PR #205) で扱う。両者は独立した軸で、
# WebUI / panel もそれぞれ別 indicator として表示する。
#
# QoS contract (issue #208): same as NavigationBackendStatus —
#   durability               : TRANSIENT_LOCAL
#   reliability              : RELIABLE
#   liveliness (publisher)   : MANUAL_BY_TOPIC
#   liveliness (subscriber)  : AUTOMATIC
#   liveliness_lease_duration: 5 s              (BOTH sides; publishers with infinite lease
#                                                are rejected via QoS incompatibility)
# Subscribers detect bridge death by handling the Liveliness Changed event
# (alive_count transitions to 0). See NavigationBackendStatus.msg for the full
# rationale, contract-violation behaviour, and the link to follow-up issue #213.

# Bridge identifier shown in tooltips (e.g. "amcl", "slam_toolbox", "custom_lidar_amcl").
# Free-form, only used to help operators distinguish multiple bridges.
string backend_type

# True when the bridge can accept and forward a new initial pose right now
# (= localization downstream subscriber is alive and ready).
bool backend_ready

# Optional human-readable reason populated when backend_ready is false.
string reason
