# Minimal navigation backend readiness, published 1 Hz on
# `mapoi/nav/backend_status` by navigation bridges such as
# `mapoi_nav2_bridge` (Nav2 bridge) or any custom bridge.
#
# Custom bridges only need to populate these three fields. UI consumers
# (mapoi_webui, mapoi_rviz_plugins) gate navigation operations on
# `backend_ready` alone. Per-capability detail belongs to `reason` or to
# bridge-specific topics, not this message.
#
# How to populate `backend_ready` (custom bridge implementers — issue #207):
#   AND only the capabilities your bridge actually exposes. Do NOT copy
#   `mapoi_nav2_bridge`'s `goal_ready && route_ready && switch_map_ready`
#   verbatim — that AND is correct only for a bridge that exposes all three
#   (NavigateToPose + FollowWaypoints + map switch). For a single-capability
#   bridge, ANDing in capabilities you don't expose pins `backend_ready` to
#   `false` forever and the UI permanently shows "Navigation unavailable".
#   Examples:
#     - Goal-only bridge:  backend_ready = goal_ready
#     - Route-only bridge: backend_ready = route_ready
#     - Goal + map switch: backend_ready = goal_ready && switch_map_ready
#   Convention for `reason` (recommended for operator UX consistency):
#   when `backend_ready` is false, list missing capabilities joined by ", "
#   prefixed with "not ready: " (see mapoi_nav2_bridge::publish_backend_status
#   for the canonical format).
#   Privacy: `reason` is surfaced in the operator UI and may be logged by
#   bag recorders or remote dashboards. Bridges MUST NOT include sensitive
#   runtime info — credentials/tokens, absolute filesystem paths, internal
#   hostnames/IPs, user identifiers, or stack traces. Stick to capability
#   names and short status verbs (e.g. "not ready: navigate_to_pose action").
#
# Localization readiness (initial pose / AMCL etc.) is intentionally NOT
# part of this contract; it will be exposed by a separate
# `LocalizationBackendStatus` topic — see issue #209.
#
# QoS contract (issue #208) — BOTH publishers and subscribers MUST set these:
#   durability               : TRANSIENT_LOCAL  (late-joiner UI receives the latest status)
#   reliability              : RELIABLE
#   liveliness (publisher)   : MANUAL_BY_TOPIC  (each publish() asserts liveliness on this
#                                                topic; subscribers handle Liveliness Changed
#                                                events to detect publisher death)
#   liveliness (subscriber)  : AUTOMATIC        (pub=MANUAL_BY_TOPIC × sub=AUTOMATIC is the
#                                                only compatible policy combination given that
#                                                pub=AUTOMATIC × sub=MANUAL_BY_TOPIC is incompatible)
#   liveliness_lease_duration: 5 s              (the QoS rule `pub.lease <= sub.lease` MUST hold;
#                                                we enforce 5 s on both sides for simplicity)
#
# Contract violations rejected by QoS incompatibility:
#   - publisher with no liveliness QoS (= infinite lease) → connection refused by subscribers,
#     because the QoS rule sub.lease (5 s) >= pub.lease (∞) cannot be satisfied.
# Note: a publisher using `liveliness=AUTOMATIC` with lease <= 5 s is technically QoS-compatible
# with our `AUTOMATIC` subscribers, but its staleness detection is weaker (Node-level liveness:
# any publisher in the same node keeps the topic "alive"). Bridges SHOULD use MANUAL_BY_TOPIC
# so that backend_status timer freeze is detected even when the node has other publishers.
# See test_backend_status_liveliness.py for the incompatibility tests that pin the
# infinite-lease rejection.
#
# UI gating: subscribers MUST treat a lost-liveliness state as `backend_ready=false`
# regardless of the latched payload (see `_resolve_backend_status_for_ui` in mapoi_webui).
#
# NOTE (issue #213): the lease must exceed any blocking call inside the publisher
# node's executor. mapoi_nav2_bridge's `select_map` flow can block the
# SingleThreadedExecutor for up to ~11 s per Nav2 map node during `send_load_map_request`,
# which can trigger false-positive liveliness lost events even at 5 s lease.
# Issue #213 tracks splitting backend_status_timer into a Reentrant callback group
# + MultiThreadedExecutor so the timer keeps publishing during such blocking calls.

# Bridge identifier shown in tooltips (e.g. "nav2", "custom_lidar_planner").
# Free-form, only used to help operators distinguish multiple bridges.
string backend_type

# True when the bridge can accept and execute navigation commands
# (goal / route / pause / resume / cancel / map switch) right now.
bool backend_ready

# Optional human-readable reason populated when backend_ready is false.
string reason
