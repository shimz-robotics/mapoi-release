^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package mapoi_interfaces
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This per-package file lists only the changes affecting ``mapoi_interfaces``,
in the flat format expected by bloom / buildfarm. Full project-level
narrative: root ``CHANGELOG.rst`` / the GitHub Releases page.

Forthcoming
-----------

0.6.0 (2026-07-15)
------------------

* No changes for this package in this release.

0.5.0 (2026-07-10)
------------------

* Add service ``RequestInitialPose`` (``{map_name, poi_name} -> {success, error_message}``) for the new ``mapoi_server`` initial-pose request flow (#211).
* ``GetRoutePois`` response gains ``bool success`` / ``string error_message`` ahead of the ``pois_list`` / ``landmark_pois`` fields; recompile all clients (#342).
* ``PointOfInterest`` drops the always-zero ``int32 id`` field; key on ``name`` instead (#338).
* ``SelectMap`` response field ``initial_poi_name`` renamed to ``resolved_initial_poi_name`` (request field unchanged) (#343).
* ``PoiEvent`` simplified from four event types to three: remove ``EVENT_STOPPED`` / ``EVENT_RESUMED``, add ``EVENT_PAUSED`` (value ``2``), shifting ``EVENT_EXIT`` from ``2`` to ``3`` (#220, #227). Note the numeric values ``2`` and ``3`` are both reused (``2``: old ``EVENT_EXIT`` -> new ``EVENT_PAUSED``; ``3``: old ``EVENT_STOPPED`` -> new ``EVENT_EXIT``): rosbags recorded before 0.5.0 and clients comparing hardcoded numbers will silently misinterpret events; always compare against the named constants after recompiling.

0.4.0 (2026-05-08)
------------------

* Add ``NavigationBackendStatus`` message (``backend_type`` / ``backend_ready`` / ``reason``) for the navigation backend readiness contract (#198).
* Add ``LocalizationBackendStatus`` message for the localization readiness contract, published by the new AMCL localization bridge (#209).
* Document ``backend_ready`` computation guidance and ``reason``-string privacy rules in the ``.msg`` headers and ``README.md`` (#207).
* Add a docs-vs-implementation consistency lint that verifies every generated msg/srv has a matching ``README.md`` section (#216).

0.3.0 (2026-05-02)
------------------

* Add ``Tolerance.msg`` (``xy`` / ``yaw``) and migrate ``PointOfInterest`` from ``float64 radius`` to the ``Tolerance tolerance`` struct (#87).
* ``Tolerance`` semantics: both ``xy`` and ``yaw`` are required to be ``>= 0.001``; zero/negative disallowed, out-of-range clamped at YAML load (#138).
* Add ``EVENT_STOPPED=3`` / ``EVENT_RESUMED=4`` constants to ``PoiEvent.msg`` (publish logic deferred) (#87).
* ``GetRoutePois.srv`` response gains a ``landmark_pois`` field for radius-monitoring / route-scoped pause POIs (#143).
* ``mapoi/initialpose_poi`` payload type changed from ``std_msgs/String`` to ``InitialPoseRequest`` (``{map_name, poi_name}``) as part of the ``initial_pose`` system tag removal (#89, #144).

0.2.0 (2026-04-29)
------------------

* No changes for this package in this release.
