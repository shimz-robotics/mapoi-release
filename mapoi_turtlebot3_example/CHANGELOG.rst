^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package mapoi_turtlebot3_example
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This per-package file lists only the changes affecting
``mapoi_turtlebot3_example`` (sample maps, client nodes, and demo launch
files), in the flat format expected by bloom / buildfarm. Full project-level
narrative: root ``CHANGELOG.rst`` / the GitHub Releases page.

Forthcoming
-----------

0.6.0 (2026-07-15)
------------------

* No changes for this package in this release.

0.5.0 (2026-07-10)
------------------

* Launch the demo with ``waypoint_arrival_mode:=mapoi`` by default and re-tune the waypoint ``tolerance.xy`` to ``0.3`` m (kept above Nav2's ``xy_goal_tolerance``) (#243, #259, #260, #261, #263, #264, #265, #266).
* Redesign the ``turtlebot3_world`` demo into a feature-catalog layout: POIs ``start`` / ``basic_waypoint`` / ``pause_waypoint`` / ``goal`` / ``audio_landmark`` and routes ``tutorial_01_basic`` / ``tutorial_02_landmark`` / ``tutorial_03_pause`` / ``tour_full`` (not a 1:1 rename of the old office-tour sample) (#230, #235).
* Add ``PoiEvent``-driven sample subscribers ``audio_guide_node`` (announce on ``EVENT_ENTER``) and ``camera_node`` (capture on ``EVENT_PAUSED``, then publish ``mapoi/nav/resume``), launched from a separate ``mapoi_event_samples.launch.yaml`` (#88, #238, #248).
* A Gazebo GUI render/driver failure no longer brings down the whole demo; navigation continues headless (#294, #295).

0.4.0 (2026-05-08)
------------------

* No changes for this package in this release.

0.3.0 (2026-05-02)
------------------

* Sample maps now live only in ``mapoi_turtlebot3_example`` (single source of truth) after being removed from ``mapoi_server`` (#163).
* Differentiate the sample ``map_file`` names per world (e.g. ``turtlebot3_world.{pgm,yaml}`` / ``turtlebot3_dqn_stage1.{pgm,yaml}``) to match the directory names (#163).
* Rework the sample YAMLs to cover the full feature matrix (consecutive pause, yaw-agnostic passes, landmark variations, composite-tag hazards); routes renamed (``route_1`` -> ``tour_full``, ``route_2`` -> ``tour_short``); new custom tags added (#146).
* Add ``scripts/check_sample_yaml_consistency.py`` plus CI hookup to validate POI/route/landmark references, tag-exclusion rules, and tolerance minimums in the sample YAMLs (#146).

0.2.0 (2026-04-29)
------------------

* Add a ``gz_sim_headless_aware.launch.yaml`` wrapper for Jazzy headless launches (#42, #49).
* Rework the ``turtlebot3_world`` and ``turtlebot3_dqn_stage1`` example configs with distinct themes and Nav2-aligned radii (#79, #82).
