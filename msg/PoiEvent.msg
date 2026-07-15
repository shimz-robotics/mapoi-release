# Published when the robot enters/exits a route POI tolerance.xy radius, or
# when navigation pauses inside a `pause`-tagged POI. The event set is scoped
# to **route navigation** (FollowWaypoints, nav_mode == ROUTE) and
# **route-registered POIs** only; nothing is published outside route execution
# (IDLE or GOAL modes). `pause`-tagged POIs additionally generate
# `EVENT_PAUSED` once nav has stopped (cmd_vel dwell).
#
# `EVENT_PAUSED` requires the active controller to keep publishing zero
# cmd_vel during nav pause/stop (default Nav2 behavior). Controllers that
# silently stop publishing cmd_vel will not trigger `EVENT_PAUSED`.
#
# Lifecycle:
#   EVENT_ENTER -> [EVENT_PAUSED (only if pause-tagged + nav stops)] -> EVENT_EXIT
#
# Note: `EVENT_RESUMED` is NOT a separate event in this contract. nav pause
# is initiated by `mapoi_server` (auto-trigger on pause-tagged POI) and
# resumed by client request via `mapoi/nav/resume`; client / status topic
# already observes the resume timing without a dedicated PoiEvent.

uint8 EVENT_ENTER=1     # route 走行中に route 登録 POI の tolerance.xy 半径へ侵入
uint8 EVENT_PAUSED=2   # pause タグ付き POI の tolerance.xy 内で navigation 停止 (cmd_vel dwell)
uint8 EVENT_EXIT=3        # route 登録 POI から tolerance.xy * hysteresis を超えて退出

uint8 event_type                      # EVENT_ENTER / EVENT_PAUSED / EVENT_EXIT
mapoi_interfaces/PointOfInterest poi  # The POI that triggered the event
builtin_interfaces/Time stamp         # Timestamp when the event occurred
