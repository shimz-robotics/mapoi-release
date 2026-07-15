^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package mapoi_rviz_plugins
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This per-package file lists only the changes affecting ``mapoi_rviz_plugins``
(the ``MapoiPanel`` / ``PoiEditorPanel`` panels and the RViz PoseTool). The
``mapoi_rviz2_publisher`` node lives in ``mapoi_server``, so its rendering
changes are recorded there. Full project-level narrative: root
``CHANGELOG.rst`` / the GitHub Releases page.

Forthcoming
-----------

0.6.0 (2026-07-15)
------------------

* Add Undo/Redo to ``PoiEditorPanel`` via ``QUndoStack``, covering row add/copy/delete, cell edits, and row reordering (Ctrl+Z / Ctrl+Shift+Z, shortcuts scoped to panel focus) (#407).
* Add Undo/Redo buttons to the PoiEditor toolbar, enabled via ``canUndoChanged`` / ``canRedoChanged``, since the Ctrl+Z-only shortcut from #407 gave no visible affordance (#435).
* Add an unsaved-edit guard to ``PoiEditorPanel``: track a dirty flag from cell edits/row add/delete/reorder, confirm before an external ``mapoi/config_path`` update discards them, and warn before ``Save`` overwrites content that changed on disk since load (#399).
* Add a POI name search filter to the PoiEditor table (case-insensitive substring match via ``setRowHidden``, combinable with the existing tag filter) (#405).
* Add Nav/Loc backend connection status badges to ``MapoiPanel``, derived from ``NavigationBackendStatus`` / ``LocalizationBackendStatus`` liveliness and ``reason`` (#400).
* Surface operation failures and successful initial-pose requests as a transient UI notice instead of RCLCPP-log-only feedback, covering ``request_initial_pose`` reject/timeout and ``get_route_pois`` failure (#401).
* Show route-driving progress in ``MapoiPanel`` by subscribing to ``mapoi/events`` and displaying the passed POI name with an ``n/total`` count (#406).
* Show a transient notice for commands rejected while navigating by subscribing to the existing ``mapoi/nav/command_rejected`` topic, auto-clearing after 5 s (#398).
* Keep the ``Route progress: —`` / ``Notice: —`` / ``Nav status: —`` idle placeholders permanently visible instead of blank labels, via new ``SetRouteProgressIdle`` / ``SetNoticeIdle`` / ``SetNavStatusIdle`` helpers (#451).
* Translate the ``MapoiPanel`` UI strings (buttons, labels, status messages) and the few remaining Japanese PoiEditor strings (two ``ValidatePois`` messages, the name-filter placeholder) from Japanese to English, matching the already-English WebUI (#431).
* Fix an uninitialized ``is_table_color_`` bool in ``PoiEditorPanel`` that could read as true when the mapoi service is unavailable, and add a bounds check on the ``shadow_`` write in ``TableChanged`` to prevent an out-of-bounds vector write (#432).
* Guard ``YAML::LoadFile`` in ``SaveButton`` against an unreadable or malformed map file, which previously threw uncaught and crashed RViz (#429).
* Validate the RViz Fixed Frame in ``PoiPoseCallback`` (Set Mapoi Pose) and reject the pose with a warning dialog unless the frame is ``map``, preventing a Fixed-Frame-relative pose from being saved as a map-frame POI (#430).
* Fix ``PoiPoseCallback`` writing the Set Mapoi Pose result into the hardcoded column index ``2`` instead of ``kColPose``, which corrupted the tolerance cell and left the pose cell unchanged (#427).
* Add a timeout to eight ``spin_until_future_complete`` service calls in the panel and editor that could previously hang the UI thread indefinitely (#404).
* Extend the unsaved-edit confirmation guard to tag filter changes, which previously discarded edits (and cleared the undo stack) on any filter change, even reselecting the same tag (#428).
* Clear a cell's green edited-mark once Undo restores its value to the ``clean_texts_`` baseline (last full rebuild / save), instead of leaving it marked as changed (#445).
* Fix New/Copy inserting rows at the logical index instead of the visually selected position after a drag reorder, using ``visualIndex`` / ``moveSection`` to insert under the selected row (#434).
* Remove the ``PoiTable`` header's sort indicator, which implied an active ascending sort that was never actually enabled (#433).
* Add a bottom vertical spacer to ``mapoi_panel.ui`` to absorb excess dock height instead of stretching the gaps between status labels (#446).
* Add a path+mtime content-diff guard to ``ConfigPathCallback`` in both panels (mirroring ``mapoi_rviz2_publisher``'s dedup), skipping the blocking service re-fetch and full rebuild when a re-published ``mapoi/config_path`` is unchanged (#403).
* Split ``mapoi_panel.cpp`` and ``poi_editor.cpp`` into per-responsibility translation units (``mapoi_panel_nav_control.cpp`` / ``mapoi_panel_backend_status.cpp`` / ``mapoi_panel_config.cpp`` / ``mapoi_panel_nav_status.cpp`` / ``poi_editor_tags.cpp`` / ``poi_editor_save.cpp`` / ``poi_editor_display_settings.cpp``) and convert ``calcYaw`` / ``join`` / ``SplitSentence`` into free functions in ``poi_editor_helpers.hpp``; internal refactor, no behavior change (#397).

0.5.0 (2026-07-10)
------------------

* Render a yaw-agnostic POI tolerance (``tolerance.yaw >= pi``) as a filled disc in the RViz view instead of omitting the sector (#267, #268, #271).
* Update ``MapoiPanel`` to check ``response.success`` on ``GetRoutePois`` (now returning ``success`` / ``error_message``) instead of assuming a completed call succeeded (#342).
* Route the RViz panel's initial-pose publishing through the new ``request_initial_pose`` service, so ``mapoi_server`` is the sole writer of ``mapoi/initialpose_poi`` (#211).
* Split ``PoiEditorPanel::ValidatePois`` into its own translation unit and extract the pose/tolerance/tag-exclusivity checks as Qt/ROS-independent pure functions with gtest coverage; displayed messages byte-for-byte unchanged (#346).

0.4.0 (2026-05-08)
------------------

* Gate navigation operations in the panel on ``NavigationBackendStatus.backend_ready`` and detect bridge death via ``LivelinessChangedEvent`` (#198, #208).
* Show a separate Localization indicator and gate the Set Initial Pose UI on ``LocalizationBackendStatus`` (#209).

0.3.0 (2026-05-02)
------------------

* Replace the POI Editor table's ``Radius`` column with ``tolerance.xy`` + ``tolerance.yaw``; ``ValidatePois`` enforces the ``>= 0.001`` minimums (#138).
* Reorganize the POI Editor table from 6 columns to 5 (``name / pose / tolerance / tags / description``), display ``yaw`` in radians, and add a new ``yaw <= 2*pi`` max validation to catch old degree values (#158).
* Fix the POI Editor Panel Display Settings UI: replace the removed ``arrow_size_ratio`` control with a ``show_tolerance_sector`` checkbox, fixing a ``get_parameters`` / ``set_parameters`` regression (#179).

0.2.0 (2026-04-29)
------------------

* Add a "Display Settings" group to the POI Editor panel (#99, #100).
* The POI Editor panel dynamically refreshes its displayed POI list on ``mapoi_config_path`` (#75, #78).
* Remove the ``nav2_rviz_plugins`` ``Selector`` and ``Docking`` panels from the default RViz config (not present on Humble) (#37, #51).
