^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package mapoi_server
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This per-package file lists only the changes affecting ``mapoi_server``
(including its ``mapoi_nav2_bridge`` / ``mapoi_amcl_localization_bridge`` /
``mapoi_rviz2_publisher`` / ``mapoi_gazebo_bridge`` / ``mapoi_gz_bridge``
nodes), in the flat format expected by bloom / buildfarm. Full project-level
narrative: root ``CHANGELOG.rst`` / the GitHub Releases page.

Forthcoming
-----------

0.6.0 (2026-07-15)
------------------

* ``mapoi_rviz2_publisher``: rebuild the marker arrays only when the underlying data changed (POIs, routes, highlights, display parameters), republishing the cached arrays with a refreshed stamp on unchanged ticks — the 1 Hz republish itself is kept so late-joining ``volatile`` subscribers still receive markers — and skip the tick entirely while both marker topics have zero subscribers (#402).

0.5.0 (2026-07-10)
------------------

* Add the ``request_initial_pose`` service on ``mapoi_server`` as the sole writer of ``mapoi/initialpose_poi``; other nodes request a publish instead of publishing directly, unifying the latched cache (#211).
* Reject ``request_initial_pose`` when a non-empty ``map_name`` does not match the server's current map, closing the stale-initial-pose map-switch race (#299).
* Optionally persist and restore the last-selected map across a restart via the new ``state_path`` parameter (default empty = disabled), avoiding a teleport to the launch-parameter map's POI (#297).
* Validate the ``select_map`` service ``map_name`` as a single path segment, rejecting path-traversal config loads (#328).
* Namespace all eight ``mapoi_server`` services under ``mapoi/`` (e.g. ``get_pois_info`` -> ``mapoi/get_pois_info``); no compatibility alias (#341).
* Return ``success=false`` from ``get_route_pois`` (``GetRoutePois``) for an unknown route instead of silently returning empty lists; ``mapoi_nav2_bridge`` / ``mapoi_rviz2_publisher`` check ``response.success`` (#342).
* Add ``waypoint_arrival_mode`` parameter (``"nav2"`` default / ``"mapoi"``) on ``mapoi_nav2_bridge``; ``"mapoi"`` sends per-waypoint ``NavigateToPose`` goals and advances on tolerance match (#243, #259, #260, #261, #263, #264, #265, #266).
* Add topic ``mapoi/nav/command_rejected`` (``std_msgs/String``) on ``mapoi_nav2_bridge`` as an unconditional rejected-command event, independent of the latched ``mapoi/nav/status`` (#354).
* Add ``auto_resume_timeout_sec`` parameter (default ``0.0`` = disabled) on ``mapoi_nav2_bridge`` to auto-resume a paused route after a timeout (#231, #232).
* Publish ``rejected:<target>`` on ``mapoi/nav/status`` for commands rejected while idle (unknown goal/route, landmark-as-goal, service not ready) so operators can see they were ignored (#339, #355, #352, #356).
* Fix Jazzy ``EVENT_PAUSED`` no longer firing by adding a ``cmd_vel_msg_type`` parameter (default ``"auto"``) that selects ``Twist`` vs ``TwistStamped`` from ``ROS_DISTRO`` (#249).
* Narrow ``PoiEvent`` publishing to ``ROUTE`` navigation only (waypoints/landmarks on the active route), remove ``EVENT_STOPPED`` / ``EVENT_RESUMED``, add ``EVENT_PAUSED`` firing on a real ``cmd_vel`` dwell (#220, #227).
* Render a yaw-agnostic POI tolerance (``tolerance.yaw >= pi``) as a filled disc in ``mapoi_rviz2_publisher`` instead of omitting the sector (#267, #268, #271).
* Split ``mapoi_nav2_bridge.cpp`` (1,712 lines) into five translation units (route / goal / map-switch / backend-status / core) compiling into one executable; internal refactor, no behavior change (#345).

0.4.0 (2026-05-08)
------------------

* Rename the Nav2 bridge node from ``mapoi_nav_server`` to ``mapoi_nav2_bridge`` (executable, node, C++ class, header, and the ``with_nav_server`` -> ``with_nav2_bridge`` launch arg); no compatibility alias (#204).
* Split the AMCL adapter out of the bridge into a new ``mapoi_amcl_localization_bridge`` executable; the ``initialpose_*`` parameters and ``/initialpose`` publishing move to it, and it publishes ``LocalizationBackendStatus`` (#209).
* Hardcode the system tag definitions in ``system_tags.hpp`` (``mapoi::kSystemTags``) and remove the ``maps/tag_definitions.yaml`` file / ``maps/`` directory; ``get_tag_definitions`` contract unchanged (#191).
* Publish ``NavigationBackendStatus`` at 1 Hz on ``mapoi/nav/backend_status`` (``transient_local`` QoS) as the navigation readiness contract (#198).
* Use ``MANUAL_BY_TOPIC`` liveliness (5 s lease) on both ``backend_status`` topics so consumers detect bridge death; reject infinite-lease publishers via QoS incompatibility (#208).
* Keep the 1 Hz ``backend_status`` publish running during blocking ``LoadMap`` calls by moving the timer to a separate callback group under a ``MultiThreadedExecutor`` (#213).
* ``mapoi_gazebo_bridge``: fix AMCL drift on operator map switch by teleporting via Gazebo Classic ``delete_entity`` + ``spawn_entity`` to the first non-landmark POI (#91, #200).
* Add CLI launch examples and ``maps_path`` / ``map_name`` argument semantics to ``mapoi_server/README.md`` (#172).

0.3.0 (2026-05-02)
------------------

* Reorganize runtime topics into the ``/mapoi/...`` namespace (markers, highlight, nav, events, config_path, initialpose_poi); legacy flat ``/mapoi_*`` topics removed without aliases (#185, #70, #102).
* Remove sample maps from ``mapoi_server`` and make ``maps_path`` required (fatal if omitted); samples now live only in ``mapoi_turtlebot3_example`` (#163).
* Remove the ``pub_interval_ms`` parameter; ``config_path`` is now published explicitly at startup, on ``SwitchMap``, and on ``reload_map_info`` (``transient_local`` QoS) (#135).
* Remove the ``initial_pose`` system tag: the first non-landmark POI in a map is the default initial pose, with an explicit ``initial_poi_name`` on ``SwitchMap.srv`` (#89, #144).
* Tighten ``pause`` firing to ``nav_mode_ == ROUTE`` and active-route waypoints/landmarks only (no longer during GOAL/IDLE) (#89, #143).
* Add ``landmark`` x ``pause`` mutual exclusion; rename system tag ``goal`` -> ``waypoint``; remove system tag ``origin`` (#89, #142).
* Migrate ``PointOfInterest.radius`` handling to the ``Tolerance`` struct server-side; rename the ``radius_check_hz`` parameter to ``tolerance_check_hz`` (#87, #140).
* Remove the ``arrow_size_ratio`` parameter from ``mapoi_rviz2_publisher`` (fixed arrow scale) (#136).
* ``reload_map_info`` now publishes an explicit skip (empty ``poi_name``) to ``mapoi/initialpose_poi`` to clear a stale latched value (#154).
* Publish ``EVENT_STOPPED`` / ``EVENT_RESUMED`` from the nav bridge with new ``stopped_speed_threshold`` / ``stopped_dwell_time_sec`` / ``cmd_vel_topic`` parameters (#140).
* Render POI tolerance as circle + sector overlays in ``mapoi_rviz2_publisher`` with a ``show_tolerance_sector`` parameter and a dot-pattern pause overlay (#136, #179).
* Extract the initial-pose selection logic into a shared ``initial_pose_resolver.hpp`` unifying the server / gazebo / gz bridge implementations (#150).

0.2.0 (2026-04-29)
------------------

* ``mapoi_nav_status`` now uses ``transient_local`` QoS so late-starting subscribers receive the latest state (#96, #103).
* Extend the ``mapoi_nav_status`` payload to the ``"status:target"`` form, preserving the target name across ``succeeded`` / ``aborted`` / ``canceled`` (#104, #119).
* Localization-agnostic Tier 1 support: configurable ``initial_pose`` topic/timeout and de-duplicated AMCL ``set_initial_pose`` calls (#57, #58).
* ``/initialpose`` auto-publish now waits for subscriber readiness before sending (#33, #56).
* Guard map-switch detection on ``mapoi_config_path`` using both path and mtime (#80, #81).
* ``mapoi_gazebo_bridge`` / ``mapoi_gz_bridge``: SwitchMap performs entity swap and robot delete + respawn / atomic ``SetEntityPose`` teleport, with symmetric cleanup and queue coalescing (#30, #46, #48, #52, #53, #54).
