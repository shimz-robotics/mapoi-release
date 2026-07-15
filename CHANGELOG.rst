^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This project follows `Semantic Versioning <https://semver.org/>`_. The 0.x
series is in active development; breaking changes may occur in any 0.x
release (per the SemVer 2.0.0 spec for 0.y.z initial-development phase).
From v1.0.0 onward, the public API (msgs, topics, services, launch
parameters, YAML schemas) will be backward-compatible across minor and
patch releases. See ``README.md`` for the full version policy.

For releases prior to v0.2.0, see the
`GitHub Releases page <https://github.com/shimz-robotics/mapoi/releases>`_.

Each package also has its own ``CHANGELOG.rst`` next to its ``package.xml``
(``mapoi/``, ``mapoi_interfaces/``, ``mapoi_server/``, ``mapoi_webui/``,
``mapoi_rviz_plugins/``, ``mapoi_turtlebot3_example/``). Those per-package
files are the ones bloom / buildfarm consume, are written in the flat
``catkin_pkg``-parseable format, and hold only the changes affecting that
package. From the next release onward they are appended to via
``catkin_generate_changelog``. This root file remains the project-wide
narrative (breaking-change migration guides, cross-package context) and is
not consumed by the release tooling.


Unreleased
==========


0.6.0 (2026-07-15)
==================

Added
-----

* WebUI: pressing ``Delete`` with a POI selected removes it (working-copy only,
  revertible with Ctrl+Z); the input-focus guard already used by the Ctrl+Z /
  Escape keydown funnel keeps it from firing while typing (#374).

* WebUI: Ctrl/Cmd+S saves all dirty editors — an open edit form is first
  confirmed into the working copy — and always calls ``preventDefault()`` to
  suppress the browser's native "save page" shortcut (#375).

* WebUI: a ``beforeunload`` handler warns before closing the tab or reloading
  with unsaved edits, reusing the same dirty-editor / open-form "blocker"
  collection the SSE reload guard below is built on (#380).

* WebUI: the map now shows the cursor's world (x, y) coordinates in a corner
  readout while the mouse moves over it, rounded to the same precision as the
  POI YAML and hidden once the cursor leaves the map or the coordinates are
  non-finite (#381).

* WebUI: selecting a POI from the list, or from the Navigation section's
  dropdowns, now pans the map to it if it is off-screen; a marker click does
  not pan, since a clicked POI is already visible and an unrequested viewport
  jump would be disorienting (#382).

* WebUI: a search box above the POI list filters it by name (case-insensitive
  substring); it only affects the list rendering, not marker visibility on
  the map, which stays governed by the existing per-POI checkboxes (#383).

* WebUI: single-key shortcuts ``L`` (toggle POI position-lock) and ``U``
  (toggle UI panel visibility) mirror the existing lock / hide-UI buttons —
  each key is its target's initial letter, for a consistent mnemonic — and
  drive the same ``.click()`` code path as the buttons so there is no
  separate state-sync implementation; both are ignored while an input /
  textarea has focus, while a modifier key is held, and on key-repeat. The
  UI-toggle icon itself was also fixed to a static ☰ glyph in the same
  change, replacing an earlier eye/eye-off swap that read ambiguously over a
  map (#390).

* WebUI: a ``?`` button/key opens a Help modal (closed via the ``×`` button,
  Escape, or a background click) covering keyboard shortcuts, map
  operations, editing/saving, the POI list, and navigation controls, plus a
  note that a Vim-style browser extension can intercept the shortcuts (#391).

* Documentation received a new-user-onboarding pass: both READMEs (en/ja)
  were reordered around a newcomer's reading path (overview -> screenshots ->
  features -> Docker quickstart -> build) and gained desktop/mobile WebUI
  screenshots, captured headless against the mock e2e server's
  ``turtlebot3_world`` data, plus CI/release/license/distro badges. A new
  ``docs/architecture.md`` (English, with a ``.ja.md`` snapshot) adds a node
  diagram and two mermaid data-flow walkthroughs (go-to-POI, map switch)
  plus a full topic/service/action table cross-checked against the
  ``create_*`` call sites. The remaining ``docs/`` pages (integration,
  docker, backend-status, migration) were translated to English-primary to
  match the READMEs' v0.5.0 switch, and GitHub issue templates for bug
  reports and feature requests were added (#396).

* RViz's ``MapoiPanel`` subscribes to ``mapoi/nav/command_rejected`` and
  shows the rejected target as a transient one-line notice, auto-cleared
  after 5s, closing a gap where a command rejected mid-navigation (e.g. a
  typo goal sent while already driving) was invisible in RViz even though
  the WebUI already toasted it (#398).

* RViz's ``MapoiPanel`` now routes operation failures and initial-pose
  success through the same transient-notice mechanism introduced by #398
  above (a shared ``ShowTransientNotice()`` helper on ``CommandRejectedLabel``,
  green for success / red for failure): a failed ``get_route_pois`` or
  ``request_initial_pose`` call, a service that never came up, and a
  successful initial-pose request. Previously these paths only logged to
  ``RCLCPP_ERROR``, so an operator not watching the console could believe an
  initial pose had been set when the server had actually rejected it (#401).

* RViz's ``MapoiPanel`` gained a small connection-status badge each for Nav2
  and localization, built from a pure ``build_backend_badge_text()`` helper:
  "Connected", "Not ready (<reason>)" while the backend status message's
  ``reason`` field is set, and "Disconnected (bridge stopped)" once DDS
  liveliness is lost. Previously a backend outage only disabled six widgets
  silently, giving no way to tell why buttons were grayed out (#400).

* RViz's ``MapoiPanel`` subscribes to ``mapoi/events`` and shows "Entered:
  <poi> (n/total)" / "Paused at: <poi> (n/total)" / "Passed: <poi> (n/total)"
  on a new ``RouteProgressLabel`` while a route is driving; ``total`` comes
  from the panel's own route selection, and the display is suppressed once
  navigation ends so a late-arriving event cannot resurrect stale progress.
  ``mapoi/events`` only fires during ``ROUTE`` navigation, so the label stays
  empty for single-goal driving (#406).

* RViz's ``PoiEditor`` panel now guards against an external
  ``mapoi/config_path`` republish — another client's save, or a map switch —
  silently rebuilding the table over unsaved cell edits: a ``table_dirty_``
  flag, set on every mutating action, gates a "discard and reload / keep
  editing" confirmation before ``ConfigPathCallback`` proceeds, closing an
  asymmetry with the WebUI's existing dirty-guard / 409-conflict machinery
  (#399).

* RViz's ``PoiEditor`` table gained a name filter box (``QLineEdit`` +
  ``setRowHidden``, case-insensitive substring match) alongside the existing
  tag filter. Rows are hidden rather than removed, so ``rowCount()`` — and
  therefore Save's row loop — stays unaffected, and the two filters compose:
  the tag filter narrows the row set first, and the name filter further
  hides within it (#405).

* RViz's ``PoiEditor`` panel gained Undo/Redo via a ``QUndoStack`` covering
  row add / copy / delete, drag reorder, and cell edits (Ctrl+Z /
  Ctrl+Shift+Z), matching the Undo/Redo the WebUI got in v0.5.0 (#407).
  Because the shortcuts are ``QShortcut``-only with no on-screen affordance,
  real-hardware testing subsequently found it was mistaken for "not
  implemented": the panel needs keyboard focus first (RViz's 3D view
  usually holds it), Linux Redo is Ctrl+Shift+Z rather than Ctrl+Y, and the
  history clears on save / tag-filter change / a full table rebuild in ways
  that make "edit -> save -> Ctrl+Z" a no-op. Two toolbar buttons (disabled
  by default, enabled via ``QUndoStack::canUndoChanged``/``canRedoChanged``)
  now give Undo/Redo an explicit, always-visible affordance with tooltips
  spelling out the shortcuts (#435).

* ``MapoiPanel``'s ``NavStatusLabel`` / ``RouteProgressLabel`` /
  ``CommandRejectedLabel`` now show a gray idle placeholder ("Nav status: —"
  / "Route progress: —" / "Notice: —") instead of sitting empty, so the
  layout space reserved to avoid jumping reads as "a value goes here" rather
  than unexplained blank lines (#451).

* RViz plugin UI strings are now English throughout
  (``mapoi_panel_nav_status.cpp``, ``mapoi_panel_nav_control.cpp``, the
  ``.ui`` files, and the two remaining ``tr()``-wrapped-but-Japanese
  validation messages) — the last holdout after the WebUI (already
  English-only) and most of ``PoiEditor`` (#431).

Changed
-------

* **``mapoi_rviz_plugins`` translation-unit split continues (#397, 8 PRs),
  follow-up to the two-step ``mapoi_nav2_bridge.cpp`` split in v0.5.0.**
  ``MapoiPanel``'s and ``PoiEditorPanel``'s remaining Qt-heavy
  responsibilities move into new files —
  ``mapoi_panel_nav_status.cpp``, ``mapoi_panel_nav_control.cpp``,
  ``mapoi_panel_config.cpp``, ``mapoi_panel_backend_status.cpp``,
  ``poi_editor_save.cpp``, ``poi_editor_tags.cpp``, and
  ``poi_editor_display_settings.cpp`` — all compiled into the same
  ``mapoi_rviz_plugins`` library, with ``MapoiPanel``/``PoiEditorPanel``
  staying single classes. ``calcYaw``/``join``/``SplitSentence`` were also
  converted to Qt-independent free functions in ``poi_editor_helpers.hpp``
  (same pattern as the #158/#346 helpers) and gained gtest coverage. Net
  effect: ``mapoi_panel.cpp`` goes from 587 to 202 lines and
  ``poi_editor.cpp`` from 873 to 456 lines (the latter also grew
  concurrently from the #405/#407/#399 features above). Internal refactor,
  no behavior change.

* ``mapoi_rviz2_publisher``'s 1Hz marker timer now rebuilds POI/route
  markers only on a dirty tick (a POI/route/highlight update or a
  drawing-parameter change); a tick with no subscribers on either topic
  skips construction and publish entirely, and a subscribed-but-clean tick
  re-``publish``\ es the cached ``MarkerArray`` with just ``header.stamp``
  refreshed, since the volatile QoS still needs a steady stream for a
  late-joining subscriber. The dirty flag is never cleared while there are
  no subscribers, so a data change that happens while RViz is disconnected
  still forces one full rebuild on the first tick after a subscriber
  reappears. Removes the previously-unconditional per-second cos/sin-heavy
  sector/wedge marker construction (#402).

* Both ``MapoiPanel::ConfigPathCallback`` and
  ``PoiEditorPanel::ConfigPathCallback`` now dedup a same-map
  ``config_path`` republish by also comparing the YAML file's mtime, on top
  of the existing map-name comparison, mirroring the dedup already done in
  ``mapoi_rviz2_publisher``. A same-path event with an unchanged mtime is a
  no-op; a changed mtime still triggers the existing blocking service
  re-fetch and full table/combo-box rebuild, since that is how a same-path
  Save (#135) is detected (#403).

Fixed
-----

* **WebUI: an SSE-triggered full reload no longer silently discards a dirty
  tab's unsaved edits (#373).** ``config_changed`` — fired whenever another
  tab or an external client such as RViz saves — used to run ``loadMaps()``
  unconditionally, wiping any in-progress POI/route/tag edit, its undo
  history, and its dirty flag with no warning. The reload is now deferred
  behind a "reload (discard edits) / keep editing" confirmation whenever a
  dirty editor or an open edit form exists; choosing to keep editing leaves
  the stale ``config_version`` in place so the eventual Save still falls
  into the existing #343 409-conflict dialog instead of silently
  clobbering.

* **WebUI: a tab no longer reloads, and loses its own undo history and map
  viewport, after saving its own changes (#384).** The same
  ``config_changed`` broadcast that triggers the #373 guard above used to
  also fire on the saving tab itself, since ``mapoi_server`` republishes
  ``config_path`` after every save. The backend now includes the YAML's
  ``config_version`` (sha256) in the SSE payload, and a tab whose own
  editors already hold that version treats the event as self-originated and
  skips the reload — while still redistributing the new version to its
  editors, so a subsequent save in another section does not spuriously 409.

* **WebUI: Leaflet is now vendored instead of loaded from a CDN, restoring
  offline operation (#394, release blocker for #20).** ``index.html`` pulled
  ``leaflet.css``/``leaflet.js`` from ``unpkg.com`` at runtime with no local
  fallback, so the map — the WebUI's core function — did not render on an
  offline robot LAN, the primary target of the upcoming apt/rosdep
  distribution. Leaflet 1.9.4's dist files are now vendored under
  ``mapoi_webui/web/vendor/leaflet/`` (BSD-2-Clause, with its own
  ``LICENSE``/``README.md`` recording provenance); ``CMakeLists.txt`` needed
  no change since it already installs ``web/`` wholesale.

* WebUI: the UI-visibility toggle button's ``aria-pressed`` (and its blue
  "on" styling) is inverted to mean "UI panels are showing," matching the
  lock button's existing "pressed = default/active state" convention; the
  previous "pressed while hidden" reading was ARIA-correct but read
  backwards against the ☰ icon's "show a menu" connotation, per real-device
  feedback (#449).

* ``PoiEditorPanel::PoiPoseCallback`` (the ``PoiEditor`` side of RViz's Set
  Mapoi Pose tool) wrote into the pose column via the hardcoded literal
  ``2`` instead of ``kColPose``, a leftover from the #158 column reorder
  that moved every other call site to the ``kCol*`` constants. In practice
  the pose tool ended up overwriting the *tolerance* cell with a 3-value
  string, which then failed ``validate_tolerance_cell``'s 2-value check on
  Save (#427).

* ``PoiEditorPanel::TagFilterChanged`` discarded all unsaved table edits
  without confirmation whenever the tag-filter combo box changed —
  including re-selecting the same tag, since Qt's ``activated`` fires on
  that too — and cleared the ``QUndoStack``, making the loss unrecoverable
  even with Ctrl+Z. A ``decide_tag_filter_change()`` pure-function guard
  (same shape as the #399 config-reload guard above) now asks for
  confirmation when the table is dirty, and restores the combo box's prior
  selection if the user chooses to keep editing (#428).

* ``PoiEditorPanel::SaveButton`` no longer crashes RViz when the target YAML
  cannot be read — an unreadable placeholder file reached via a degraded
  ``onInitialize``/``InitConfigs`` path, or external deletion/corruption of
  the file. The previously bare ``YAML::LoadFile`` call is now wrapped in a
  try/catch that reports the failure through a ``QMessageBox`` instead of
  letting ``YAML::BadFile``/``ParserException`` propagate out of a Qt slot
  (#429).

* Set Mapoi Pose (the RViz pose tool feeding ``PoiEditor``) now checks that
  RViz's Fixed Frame is ``map`` (tolerating a leading ``/`` from older
  nodes) before reflecting a pose into the table, and warns and refuses
  instead of writing it when the frame does not match. Previously the
  tool's raw Fixed-Frame x/y/theta was recorded unchanged as if already in
  the ``map`` frame — plausible-looking numbers that, once saved, would
  route Nav2 to the wrong place on a robot whose Fixed Frame was not
  ``map`` (#430).

* ``PoiEditorPanel::is_table_color_`` is now initialized (``= false``)
  instead of left indeterminate, and ``TableChanged`` bounds-checks
  ``shadow_`` before writing to it. Both defects were only reachable with a
  dead panel (a service timeout during ``onInitialize``/``UpdatePoiTable``)
  whose bundled placeholder rows stay editable: an unlucky
  uninitialized-bool read could mark the panel dirty and push undo
  commands, and the unguarded ``shadow_[row][column]`` write against an
  unbuilt shadow model was an out-of-bounds vector write (#432).

* Removed ``PoiTable``'s permanently-shown ascending sort indicator
  (``setSortIndicatorShown``/``setSortIndicator``), present since the
  initial implementation despite the table never actually supporting
  sorting — row order is YAML order, optionally reordered by drag — so the
  arrow was lying about the table's actual order. Enabling real sorting was
  ruled out because Undo, the shadow model, and Save all assume logical row
  indices (#433).

* ``PoiEditor``'s New/Copy insert position is now computed from the
  selected row's *visual* index (via a new ``insert_move_target_visual()``
  helper) instead of its logical index, and the inserted row is moved to
  match. After a drag-reorder, New/Copy used to insert at an unrelated
  visual position (Qt inserts new sections at their logical index), which
  read as "the button didn't work" and encouraged repeated clicks that
  piled up duplicate rows — rows Save then persists wherever they visually
  landed (#434).

* ``PoiEditor``'s green "edited" cell highlight now clears once Undo
  returns a cell's text to its post-load baseline, instead of staying lit
  regardless of the actual value. A ``clean_texts_`` baseline snapshot,
  refreshed on every full table rebuild and kept in sync across row
  insert/delete, backs the new ``RefreshCellEditMark()`` /
  ``ClearAllEditMarks()`` helpers, so highlighting now reflects "differs
  from the saved baseline" rather than "has ever been touched" (#445).

* ``MapoiPanel``'s docked layout no longer stretches its status labels
  apart with excess vertical whitespace when there is spare height: a
  trailing expanding ``QSpacerItem`` now absorbs the slack at the bottom
  instead (#446).

* Eight blocking ``rclcpp::spin_until_future_complete`` service calls in
  ``mapoi_rviz_plugins`` (``get_maps_info`` in both panels'
  ``onInitialize``, two combo-box population calls, ``MapComboBox``,
  ``SaveButton``'s ``reload_map_info``, ``LoadTagDefinitions``, and
  ``UpdatePoiTable``) previously had no timeout of their own beyond the
  initial 3s ``wait_for_service``, so a service that accepted the request
  and then hung could freeze the whole panel indefinitely. All eight now
  pass a 5s timeout and log-and-return on expiry, matching the pattern
  already used elsewhere in the same files; ``LoadTagDefinitions`` also
  gained the ``SUCCESS`` result check it was previously missing (#404).

Internal / tests
----------------

* First release using per-package ``CHANGELOG.rst`` files: the project-wide
  history was split into ``mapoi/``, ``mapoi_interfaces/``,
  ``mapoi_server/``, ``mapoi_webui/``, ``mapoi_rviz_plugins/``, and
  ``mapoi_turtlebot3_example/`` — the ``catkin_pkg``-parseable, flat format
  bloom/buildfarm expect, with this root file remaining the project-wide
  narrative going forward (see the note at the top of this document). The
  ``mapoi`` metapackage also gained the ``LICENSE`` it had been missing
  since its v0.5.0 introduction, needed for bloom's per-package deb
  assembly, and ``mapoi_rviz_plugins/package.xml`` gained the
  ``<buildtool_depend>ament_cmake</buildtool_depend>`` declaration the other
  five packages already had — both ahead of the #20 ROS Index registration
  (#395).

* A CI step now verifies that every per-package ``CHANGELOG.rst`` parses
  via ``catkin_pkg.changelog.get_changelog_from_path`` (catching a broken
  version-heading underline or a missing ``Forthcoming`` section) and that
  every ``package.xml``-bearing directory has a co-located
  ``CHANGELOG.rst`` and ``LICENSE``, so a future regression in the
  packaging groundwork laid by #395 is caught at PR time instead of at the
  next bloom-release. Because the script parses RST from PRs, including
  forks, it explicitly disables docutils' ``file``/``raw`` insertion
  directives to keep an untrusted CHANGELOG from being able to read
  arbitrary files (#454).


0.5.0 (2026-07-10)
==================

Added
-----

* All six package ``README.md`` files (root, ``mapoi_server``, ``mapoi_webui``,
  ``mapoi_turtlebot3_example``, ``mapoi_interfaces``, ``mapoi_rviz_plugins``)
  are now English-primary, ahead of ROS Index registration (#20); the prior
  Japanese content is preserved as a same-directory ``README.ja.md``
  snapshot, with cross-links between the two at the top of each file (#162).

* A metapackage ``mapoi`` (``mapoi/package.xml`` with ``exec_depend`` on
  the four core packages), so the core suite can be installed/depended on
  as one unit — groundwork for apt/rosdep distribution (#20) (#348).
  Three deliberate deviations from the issue text: it lives in a
  ``mapoi/`` subdirectory (navigation2-style) rather than at the
  repository root, because a package manifest at the root would stop
  colcon's package discovery from descending into the sibling packages;
  it omits the REP 140 ``<metapackage/>`` export, whose catkin_pkg
  validation is ROS 1 specific and would warn ("must buildtool_depend on
  catkin") on every colcon invocation; and it excludes
  ``mapoi_turtlebot3_example``, so a robot/production install does not
  pull in the TurtleBot3 + Gazebo simulator stack (same policy as
  navigation2, whose metapackage excludes the demo ``nav2_bringup``).
  Installing ``mapoi_turtlebot3_example`` directly still brings in the
  whole demo, since the example itself ``exec_depend``-s the mapoi core
  packages.

* New service ``request_initial_pose``
  (``mapoi_interfaces/srv/RequestInitialPose``,
  ``{map_name, poi_name} -> {success, error_message}``) on ``mapoi_server``
  (#211). Requesters ask ``mapoi_server`` — the sole writer of
  ``mapoi/initialpose_poi`` — to publish an initial-pose POI request; an
  empty ``poi_name`` publishes a clear (skip) sample. Since #299 a
  non-empty ``map_name`` must match the server's current map: a mismatch
  is rejected with ``success=false`` and nothing is published, so
  requesters must check ``response.success`` instead of treating a
  completed call as success (an empty ``map_name`` is passed through
  unvalidated for requesters that do not know the current map).

* ``mapoi_webui`` REST API consistency improvements (#343, second half; first
  half in #363): every JSON error response (4xx/5xx) now carries a
  machine-readable ``code`` field alongside the existing human-readable
  ``error`` message — ``invalid_request`` (400), ``not_found`` (404),
  ``version_mismatch`` (409, unchanged from #241), ``service_unavailable``
  (503), ``internal_error`` (500). This is purely additive; existing
  ``error`` consumers are unaffected. Also, the optimistic-concurrency
  ``expected_version`` check introduced for ``POST /api/pois`` (#241) is now
  available on ``POST /api/routes`` and ``POST /api/custom_tags`` as well,
  since all three write the same ``mapoi_config.yaml``: ``GET /api/routes``
  and ``GET /api/tag_definitions`` now return ``config_version``, and the
  corresponding ``POST`` endpoints accept ``expected_version`` and return
  ``409`` + ``code: version_mismatch`` on a stale write (``expected_version``
  omission still skips the check, matching the pre-existing ``/api/pois``
  contract). The WebUI frontend (route editor, tag editor) handles the
  conflict the same way the POI editor already does.

* New topic ``mapoi/nav/command_rejected`` (``std_msgs/String``, volatile QoS,
  payload = target string) on ``mapoi_nav2_bridge`` (#354). ``mapoi/nav/status``
  is a latched state snapshot, so a rejected command while navigation is
  already in progress (``nav_mode_ != IDLE``) intentionally does not publish
  ``"rejected"`` there (#339) to avoid clobbering the in-progress status.
  That left operators with no way to notice a rejected command (e.g. a typo
  goal) sent mid-navigation, short of reading the ROS logs. The new topic is
  an independent event notification, published unconditionally every time a
  command is rejected regardless of ``nav_mode_``. ``mapoi_webui`` forwards it
  as an SSE ``command_rejected`` event, and the WebUI frontend shows a small
  auto-dismissing toast (``Command rejected: <target>``). Purely additive;
  ``mapoi/nav/status`` semantics are unchanged.

* New ``waypoint_arrival_mode`` parameter (``"nav2"`` default / ``"mapoi"``)
  on ``mapoi_nav2_bridge`` (#243, #259, #260, #261, #263, #264, #265, #266).
  In ``"mapoi"`` mode, route waypoints are no longer submitted to Nav2 in
  one ``FollowWaypoints`` batch; ``mapoi_nav2_bridge`` instead sends one
  ``NavigateToPose`` goal per waypoint and advances to the next one as soon
  as ``OR((tolerance.xy entry AND tolerance.yaw match), Nav2 SUCCEEDED)``, so
  a waypoint whose orientation already satisfies ``tolerance.yaw`` completes
  without waiting for Nav2's own goal-checker to settle, while a tight final
  goal still waits for Nav2's own arrival if the pose doesn't already match.
  ``pause``-tagged waypoints still auto-pause, and resume advances to the
  next waypoint. The default ``"nav2"`` mode is unchanged and keeps
  submitting the whole route via ``FollowWaypoints``. The
  ``mapoi_turtlebot3_example`` demo now launches with
  ``waypoint_arrival_mode:=mapoi`` by default (the ``mapoi_nav2_bridge``
  parameter's own default stays ``"nav2"``), with waypoint ``tolerance.xy``
  re-tuned to ``0.3`` m (kept above Nav2's ``xy_goal_tolerance`` of
  ``0.25`` m so ``EVENT_ENTER`` / ``EVENT_PAUSED`` still fire reliably).

* WebUI: the selected POI's marker can now be dragged on the map to move
  its position (#239, #274). Dragging is disabled by default and enabled
  only for the currently-selected POI; ``dragend`` updates the in-memory
  working copy's ``pose.x``/``pose.y`` (rounded to the same 3-decimal
  precision as the YAML) and marks the editor dirty — nothing is written
  until the user clicks Save, consistent with the existing pose-tool /
  optimistic-concurrency save flow (#241, see below). ``yaw``,
  ``tolerance``, and ``tags`` are untouched by dragging. Dragging is
  disabled while the route editor is active to avoid conflicting with
  click-to-add-waypoint.

* WebUI: a rotation handle on the tip of a selected POI's tolerance sector
  (wedge) lets ``pose.yaw`` be adjusted by dragging (#275, #276), as a
  companion to the position-drag above (#239): the arrow marker moves
  position, the sector handle rotates orientation. The handle appears only
  for a selected, drag-enabled POI whose sector is a wedge (``0 <
  tolerance.yaw < π``); a yaw-agnostic POI (rendered as a full disc, see
  below) or one with dragging disabled has no handle. ``dragend`` rounds
  the new ``pose.yaw`` to 4 decimals and marks the editor dirty;
  confirmation is via Save, not auto-POST.

* WebUI: a single click on a POI marker now only selects it; a
  double-click is required to enter the edit form (#240, #272), separating
  "select" from "edit" so a stray click no longer opens the editor
  unexpectedly. Clicking a different POI while editing exits the edit form
  back to selection. Opening the edit form now also collapses the other
  panel sections (Navigation / Tags / Routes) and hides the POI list to
  focus on the form, restoring their prior open/closed state on close.

* RViz and WebUI now render a yaw-agnostic POI tolerance
  (``tolerance.yaw >= π``, i.e. "any orientation is fine") as a filled disc
  instead of omitting the sector (#267, #268, #271). Previously the sector
  was silently skipped for yaw-agnostic POIs, which looked like a rendering
  bug; a disc that grows continuously out of the wedge as ``tolerance.yaw``
  approaches ``π`` now visually confirms "any orientation accepted". The
  WebUI's degree display for ``tolerance.yaw`` was also cleaned up (rounds
  to clean degree values like 30/45/90 instead of noisy values such as
  ``180.0002``, while still round-tripping exactly with the 4-decimal
  radian storage).

* WebUI: selecting a POI or route from the Navigation section's dropdowns
  now highlights it on the map, the same way selecting it from the
  POI/Route list already did (#262, #278).

* WebUI: the POI editor gained Undo/Redo (Ctrl/Cmd+Z, Ctrl+Shift+Z,
  Ctrl+Y), backed by a per-mutation snapshot stack capped at 50 entries
  (#300, #301). Every mutating operation (add / edit / copy / delete /
  drag-move / yaw-handle rotate) is captured before it runs; loading POIs
  or discarding resets the stack, and Save advances the baseline depth.
  POI dirty-state tracking was reworked in the same change to derive from
  the saved content instead of a separately-tracked flag, so it stays
  correct across undo/redo. Keyboard shortcuts are ignored while a text
  input/textarea has focus. Route-level undo is intentionally out of scope
  for this change.

* ``mapoi_webui``: ``GET /api/pois`` now returns a ``config_version`` (a
  sha256 of the YAML), and ``POST /api/pois`` rejects a stale write with
  ``409`` when the caller's ``expected_version`` does not match (#241,
  #245). The frontend surfaces the conflict as a reload-confirmation
  dialog instead of silently clobbering a concurrent YAML edit or RViz
  save. This is the same optimistic-concurrency pattern later extended to
  ``POST /api/routes`` and ``POST /api/custom_tags`` (#343, see above).

* WebUI: a floating button now toggles the header/panel UI fully on/off
  (one tap collapses the map to full-screen and back), and a Display
  section (collapsed by default) exposes an opt-in "floating overlay on
  map" mode plus a UI-opacity slider (30-100%, frosted-glass
  ``backdrop-filter``) (#323, #324, #325). Settings persist in
  ``localStorage``; the default (opacity 100%, docked panel) keeps the
  prior look unchanged.

* WebUI: on narrow (mobile) screens, a floating Undo icon now appears next
  to the map whenever the POI undo history is non-empty, so an accidental
  drag/edit can be reverted without opening the side panel first (#332,
  #334). The same change fixed a pre-existing bug where POI undo / redo /
  discard / save could clobber an in-progress, unsaved route-editor working
  copy; the reload guard is now centralized at the ``loadRoutes()`` call
  sites. The floating Undo button shares the same floating cluster (top
  right of the map) as the position-edit lock toggle added alongside the
  zoom-button removal (#335, see Changed).

* New ``auto_resume_timeout_sec`` parameter (default ``0.0`` = disabled) on
  ``mapoi_nav2_bridge`` (#231, #232). When positive, a paused route
  (``EVENT_PAUSED``) automatically resumes after the given number of
  seconds instead of waiting indefinitely for an external
  ``mapoi/nav/resume``; an external resume that arrives first cancels the
  pending timer. Negative, ``NaN``, and ``Inf`` values are clamped to
  ``0.0`` with an error log. Useful for unattended demos / automated runs;
  the default keeps the existing indefinite-wait behavior.

Breaking changes
----------------

* All eight ``mapoi_server`` services now live under the ``mapoi/`` namespace,
  matching the topics (already namespaced since v0.3.0). No compatibility
  alias is provided. See ``docs/migration/v0.5.0.md`` for the full migration
  guide. Rename table:

  - ``get_pois_info`` -> ``mapoi/get_pois_info``
  - ``get_route_pois`` -> ``mapoi/get_route_pois``
  - ``get_maps_info`` -> ``mapoi/get_maps_info``
  - ``get_routes_info`` -> ``mapoi/get_routes_info``
  - ``select_map`` -> ``mapoi/select_map``
  - ``request_initial_pose`` -> ``mapoi/request_initial_pose``
  - ``reload_map_info`` -> ``mapoi/reload_map_info``
  - ``get_tag_definitions`` -> ``mapoi/get_tag_definitions``

  Update any external client (``ros2 service call``, custom bridges,
  monitoring scripts) that pins one of the old bare names. Service types,
  request/response fields, topics, and launch parameters are unchanged
  (#341).

* ``mapoi_interfaces/srv/GetRoutePois`` response gains ``bool success`` /
  ``string error_message`` (same pattern as ``SelectMap``), added ahead of
  the existing ``pois_list`` / ``landmark_pois`` fields. When
  ``route_name`` does not match any route on the current map,
  ``mapoi_server`` now returns ``success=false`` with a descriptive
  ``error_message`` and logs a ``WARN``, instead of silently returning
  empty lists. A route that exists but has zero waypoints still returns
  ``success=true`` (data-distinguishable from a typo'd route name).
  Recompile all clients against the updated ``.srv``; ``mapoi_nav2_bridge``,
  ``mapoi_rviz2_publisher``, and ``mapoi_rviz_plugins`` (``MapoiPanel``)
  are updated in this change to check ``response.success`` (#342).

* ``mapoi_interfaces/msg/PointOfInterest`` drops the ``int32 id`` field. Its
  sole producer, ``MapoiServer::yaml_to_poi_msg()``, never assigned it, so
  every ``id`` surfaced via ``get_pois_info`` / ``get_route_pois`` /
  ``PoiEvent.poi`` was always ``0``; ``name`` is the de facto unique key
  already used by the WebUI and REST API. Recompile all clients against the
  updated ``.msg``. Any external code reading ``PointOfInterest.id`` must
  switch to keying on ``name`` instead. See ``docs/migration/v0.5.0.md``
  (#338).

* ``mapoi_interfaces/srv/SelectMap`` response field ``initial_poi_name`` is
  renamed to ``resolved_initial_poi_name``. Request and response previously
  reused the same field name (``initial_poi_name``) for the *requested* POI
  and the *resolved* POI, which read ambiguously at a glance; the request
  field is unchanged. The ``mapoi_webui`` REST endpoint ``POST
  /api/maps/select`` mirrors the rename in its JSON response body.
  Recompile all clients against the updated ``.srv``; ``mapoi_server`` and
  ``mapoi_nav2_bridge`` are updated in this change. See
  ``docs/migration/v0.5.0.md`` (#343).

* ``mapoi_webui`` REST API URL hierarchy: editor-only endpoints now live
  under an ``/api/editor/`` prefix, separate from ``/api/nav/`` endpoints
  that act on the running robot. ``POST /api/maps/select`` only switches
  ``mapoi_server``'s edit-context map and does not touch Nav2 or the
  running robot, whereas ``POST /api/nav/switch-map`` actually switches
  the running robot's Nav2 map; the similar naming made this distinction
  easy to miss and risked unintended map switches on a live robot (#340).
  The remaining renames unify the URL separator convention on kebab-case
  (#343, fifth checklist item). No compatibility alias is provided; old
  paths now return ``404``. Rename table:

  - ``POST /api/maps/select`` -> ``POST /api/editor/select-map``
  - ``GET /api/tag_definitions`` -> ``GET /api/tag-definitions``
  - ``POST /api/custom_tags`` -> ``POST /api/custom-tags``
  - ``POST /api/nav/initialpose`` -> ``POST /api/nav/initial-pose``

  Update any external client (curl scripts, dashboards, custom
  frontends) pinning one of the old paths. See
  ``docs/migration/v0.5.0.md`` for the full migration guide.

* ``mapoi_interfaces/msg/PoiEvent`` is simplified from four event types to
  three, and its scope is narrowed to route navigation only (#220, #227).
  ``EVENT_STOPPED`` and ``EVENT_RESUMED`` are removed; the remaining
  ``EVENT_ENTER`` / ``EVENT_EXIT`` are joined by a new ``EVENT_PAUSED``
  (constant value ``2``, taking the slot previously used by
  ``EVENT_EXIT``, which shifts ``EVENT_EXIT`` from ``2`` to ``3``).
  Events are now published only during ``ROUTE`` navigation (``nav_mode_ ==
  ROUTE`` — whether the route is being driven via a single ``FollowWaypoints``
  batch or, under ``waypoint_arrival_mode=mapoi``, per-waypoint
  ``NavigateToPose`` goals does not change this) for POIs registered on the
  active route (waypoints and landmarks); the previous behavior of firing for
  any tagged POI regardless of navigation mode (``IDLE`` / ``GOAL`` /
  ``ROUTE``) is gone.
  ``EVENT_PAUSED`` fires only for ``pause``-tagged POIs once navigation has
  actually stopped (``cmd_vel`` dwell, not merely Nav2 ``SUCCEEDED``); there
  is no dedicated resume event — resume timing is observable via
  ``mapoi/nav/resume`` / ``mapoi/nav/status`` instead. Recompile all
  clients against the updated ``.msg``. See ``docs/migration/v0.5.0.md`` for
  the field/value mapping.

Changed
-------

* **mapoi_nav2_bridge.cpp split into three translation units, step 1 of 2 (#345).**
  ``mapoi_server/src/mapoi_nav2_bridge.cpp`` had grown to 1,712 lines / ~40 methods
  covering seven-plus responsibilities. Route navigation (``FollowWaypoints`` + the
  mapoi-driven waypoint-arrival mode) and single-goal navigation (``NavigateToPose``)
  are now defined in new files ``mapoi_nav2_bridge_route.cpp`` and
  ``mapoi_nav2_bridge_goal.cpp``; all three compile into the same ``mapoi_nav2_bridge``
  executable and ``MapoiNav2Bridge`` remains a single class. This is an internal
  refactor with no behavior change: topic/service names, QoS, parameters, log
  messages, callback groups, mutex/lock granularity, and publish timing are all
  unchanged. Map switching (``LoadMap`` / ``select_map`` sync) and backend status
  publishing are deliberately left untouched for a follow-up PR (#345 step 2), as is
  the shared ``tolerance_check_callback`` POI-event judgment engine and the
  pause/resume/cancel/reset dispatchers, which read and write state from both route
  and goal navigation and are not cleanly separable without touching those.

* **mapoi_nav2_bridge.cpp split into five translation units, step 2 of 2 (final,
  #345).** Map switching (``mapoi_switch_map_cb`` / ``on_select_map_received`` /
  ``send_load_map_request`` / ``request_initial_pose``) and backend status
  publishing (``publish_backend_status``) are now defined in new files
  ``mapoi_nav2_bridge_map_switch.cpp`` and ``mapoi_nav2_bridge_backend_status.cpp``;
  all five compile into the same ``mapoi_nav2_bridge`` executable and
  ``MapoiNav2Bridge`` remains a single class. Internal refactor only, no behavior
  change (same guarantees as step 1). ``mapoi_nav2_bridge.cpp`` now holds node
  setup/constructor, the pause/resume/cancel/reset dispatchers, the shared
  ``tolerance_check_callback`` POI-event judgment engine, POI list / system tag
  fetch infrastructure, and cmd_vel monitoring — all of which are shared across
  route, goal, and map-switch navigation and are not cleanly separable per domain.
  Also removes ``clear_current_route_poi_names_``, a pre-existing dead private
  method with no remaining callers.

* **mapoi_webui frontend responsibility split (#346), internal refactor with no
  behavior change.** ``mapoi_webui/web/js/map-viewer.js`` (1,579 lines) had marker
  icon generation, POI tolerance sector/yaw-handle rendering, and route drawing all
  in one ``MapViewer`` class. Marker/route icon and color helpers (``getPoiColor``,
  ``getRouteColor``, ``routeDirectionDeg``, ``createRouteDirectionSvg``,
  ``createArrowIcon``, ``createRobotIcon``, ``createRouteLandmarkIcon``) move to the
  new ``map-icons.js`` (``MapoiMapIcons``, dual browser/Node export, continuing the
  ``geometry.js``/``poi-filter.js``/``poi-interactions.js`` pattern); POI tolerance
  sector drawing and the yaw-rotation drag handle (``_drawSectorForPoi``,
  ``_wedgePoints``, ``_circlePoints``, the yaw-handle trio, and the state they
  owned — ``sectorLayers`` / ``_poiWedgeByIndex`` / ``_yawHandle``) move to the new
  ``MapViewerSector`` class in ``map-viewer-sector.js``, which ``MapViewer`` now
  composes (``this._sector = new MapViewerSector(this)``). ``MapViewer`` keeps its
  public API and remains a single class; both new files are added to
  ``index.html``'s script load order ahead of ``map-viewer.js``.

* **app.js tag editor split into a TagEditor class (#346), internal refactor
  with no behavior change.** ``mapoi_webui/web/js/app.js`` had the custom tag
  list UI (rendering, add/delete, description disclosure, dirty tracking,
  save/discard against ``MapoiApi.saveCustomTags``) inline in its single IIFE.
  It now lives in the new ``TagEditor`` class (``tag-editor.js``), mirroring
  the ``PoiEditor``/``RouteEditor`` shape: DOM wiring in the constructor, a
  ``dirty`` field read by app.js's unsaved-changes guards, and
  ``onReload``/``onConflictReload`` callbacks so app.js still owns the actual
  ``loadTagDefinitions``/``loadPois``/``loadRoutes`` refetch sequencing after
  a save, discard, or 409 version-mismatch. ``tag-editor.js`` is added to
  ``index.html`` ahead of ``app.js``.

* **mapoi_rviz_plugins: PoiEditorPanel::ValidatePois split into its own
  translation unit, with its pose/tolerance/tag-exclusivity checks extracted
  as pure functions (#346), same staged-split approach as the
  mapoi_nav2_bridge split above (#345) and internal refactor with no behavior
  change.** ``ValidatePois()`` (152 lines) now lives in the new
  ``poi_editor_validation.cpp``, compiled into the same
  ``mapoi_rviz_plugins`` library; ``PoiEditorPanel`` remains a single class
  and its header is unchanged. Its per-cell decision logic — pose format
  ("x, y, yaw"), tolerance format/minimum/2π-overflow ("xy, yaw_rad"), and
  the waypoint/pause × landmark tag exclusivity (#85/#143) — moves to new
  Qt/ROS-independent pure functions (``validate_pose_cell``,
  ``validate_tolerance_cell``, ``check_tag_exclusivity``) in the existing
  ``poi_editor_helpers.hpp`` (alongside ``try_parse_finite_double`` /
  ``split_and_trim`` from #158). ``ValidatePois()`` itself still builds the
  exact same ``QMessageBox`` warning text from the returned status/value
  structs, so displayed messages are byte-for-byte unchanged. The three new
  functions get gtest coverage in ``test_poi_editor_validation.cpp``
  (``mapoi_rviz_plugins``), following the existing
  ``test_poi_editor_helpers.cpp`` / ``test_config_path_update_policy.cpp``
  pattern (docs/testing-policy.md §1(b): cheap, ROS/Qt-independent pure
  functions).

* WebUI: the map's zoom +/- buttons were removed (mouse wheel / pinch zoom
  still work; a test now pins that a wheel event still changes the map's
  zoom), and POI position-drag editing (#239, #274) is now **locked by
  default** — a separate toggle, in the same floating cluster as the
  mobile Undo button (see Added), must be used to unlock dragging before a
  POI can be moved on the map (#335). This reduces accidental position
  edits on touch devices, where a stray drag was easy to trigger.

* WebUI: further POI/Route editor UX refinements on top of the drag /
  yaw-handle / double-click editing above: the route editor gained a
  click-to-add-waypoint toggle and inserts a clicked POI after the
  currently-selected waypoint; route-edit selects stay in sync with
  POI-list selection; custom tag descriptions can be disclosed in full;
  active edit modes (POI / pose tool / route) can be cancelled with
  Escape; and remaining UI warning strings were translated to English
  (#306, #312, #314, #316, #318, #320, #322).

Fixed
-----

* **Rejected navigation commands are now visible on ``mapoi/nav/status``
  (#339, #355).** Command inputs that were rejected before being accepted —
  an unknown goal POI name (typo), a ``landmark``-tagged POI given as a goal,
  an empty or unknown route, or a route command sent before the
  ``mapoi/get_route_pois`` service is ready — used to fail with only an
  ERROR/WARN log, leaving the previous status (``succeeded`` / ``navigating``
  etc.) latched on ``mapoi/nav/status`` so operators could not tell the
  command was ignored. ``mapoi_nav2_bridge`` now publishes
  ``rejected:<target>`` for these paths (#352, #356). While navigation is in
  progress (``nav_mode_ != IDLE``) the rejected status is deliberately *not*
  published so it cannot masquerade as the live navigation state — that gap
  is covered by the ``mapoi/nav/command_rejected`` event topic (#354, see
  Added).

* **mapoi/initialpose_poi multi-writer race (#211).** The topic had four
  direct publishers (``mapoi_server``, ``mapoi_nav2_bridge``, WebUI, RViz
  panel). Because a ``transient_local`` cache is held *per writer*, a
  late-joining subscriber received each writer's last latched sample in
  undefined order, so ``mapoi_server``'s clear could not erase a stale POI
  still latched in another writer's cache. Publishing is now consolidated
  into ``mapoi_server`` as the **sole writer**: ``mapoi_nav2_bridge`` /
  WebUI / RViz panel request a publish via the new ``request_initial_pose``
  service instead of publishing directly, so the latched cache is unified
  and a clear is decisively last-write-wins. The timing gate that defers the
  operator-switch POI until after Nav2 ``LoadMap`` succeeds stays in
  ``mapoi_nav2_bridge``. The topic ``mapoi/initialpose_poi`` and its
  subscriber contract (``InitialPoseRequest``, ``transient_local``, empty
  ``poi_name`` = ignore) are unchanged, so custom localization bridges and
  direct external publishers keep working.

  Scope: this removes the cross-writer latched-cache race specifically. It
  does NOT make POI-name-to-pose resolution map-consistent — the localization
  bridge resolves a latched POI *name* against ``mapoi_server``'s *current*
  map (``map_name`` is intentionally unverified on the bridge side, #149
  r10), so a stale or concurrently-interleaved name could still resolve to
  a wrong pose right after a map switch. That residual is now closed on the
  server side by the ``request_initial_pose`` current-map validation (#299,
  see below); the generation/pending design considered in #155 was dropped
  as unnecessary. The crash/restart startup re-latch was fixed separately
  via ``state_path`` persistence (#297, see below).

* **Stale initial pose during a map-switch transition window (#299).**
  When operator map switches overlap (A→B), ``mapoi_server`` finishes both
  ``select_map`` calls (context = B) before ``mapoi_nav2_bridge`` runs A's
  response callback, so A's ``request_initial_pose(A, poi_A)`` used to be
  latched-published even though it no longer matches the current map. The
  service now rejects a non-empty ``map_name`` that does not match the
  server's current map (``success=false``, nothing published). The
  requesters (``mapoi_nav2_bridge`` / WebUI / RViz panel) all check
  ``response.success`` and surface the rejection as an error instead of
  a false success.

* **Last-selected map is now optionally persisted and restored across a
  restart (#297).** Previously, a crash/forced restart of ``mapoi_server``
  re-published the *launch-parameter* map's first POI as the initial pose
  to every already-running localization bridge — as a fresh DDS publisher
  joining, its ``transient_local`` sample reaches live subscribers even
  though the robot was actually operating on a different, operator-selected
  map — silently teleporting the robot and resetting map context to the
  wrong map. A new opt-in parameter ``state_path`` (default empty =
  disabled) makes ``mapoi_server`` record the current map name to
  ``<state_path>/last_selected_map`` (atomic tmp+rename) on startup and on
  every successful ``select_map``; on a restart where this state file
  exists, the server restores map context to the last-selected map and
  publishes only a clear (no teleporting POI) instead of the
  launch-parameter map's default. A missing/unreadable state file, or one
  naming a map absent from ``maps_path``, falls back to the
  launch-parameter map with a ``WARN`` (first-boot behavior, #144, is
  unaffected when ``state_path`` is unset). The state file's map name is
  validated as a single path segment before use, rejecting a
  crafted/corrupted value that could otherwise cause a path-traversal
  config load (same style of validation added to the ``select_map`` service
  input, #328, see below).

* **``select_map`` service ``map_name`` path-traversal validation (#328).**
  The ``select_map`` service now validates that ``map_name`` is a single
  path segment (no ``/``, ``\``, or a lone ``.``/``..``) before joining it
  onto ``maps_path``. Previously an operator-facing caller (WebUI, RViz
  panel, or any external client) could pass a crafted ``map_name`` to load
  an arbitrary YAML config file outside ``maps_path`` and, via the
  resulting ``nav2_map_urls``, direct Nav2's ``LoadMap`` outside the
  intended maps directory. An invalid name is rejected with
  ``success=false`` and nothing is published or loaded.

* WebUI: fixed a CSS bug where the map collapsed to zero height on mobile
  (narrow, ``max-width: 768px``) layouts — ``#map-container``'s
  ``flex-basis: 0%`` conflicted with ``main``'s ``height: auto`` even
  though ``height: 50vh`` was also set (#324, #331). Changed to
  ``flex: none`` in both the docked and UI-overlay layouts. Several
  under-sized touch targets (section toggles, check-all/none, toolbar
  buttons) were also enlarged to at least 40px on mobile.

* ``mapoi_webui``: ``POST /api/nav/switch-map`` now rejects a non-string
  ``map_name`` with ``400`` instead of coercing it with ``str(...)``
  (which turned e.g. ``null`` into the literal string ``"None"``) (refs
  #199, #281).

* WebUI: POI Save now rounds floats to the same precision used in the
  YAML (``xy``: 3 decimals, ``yaw``: 4 decimals) on both the frontend
  (``poi-editor.readForm``) and the backend
  (``yaml_handler._round_poi_floats``), preventing floating-point noise
  from ``parseFloat`` or degree/radian conversion from accumulating into
  the saved YAML (#242, #244).

* **Jazzy: ``EVENT_PAUSED`` silently stopped firing (#249).** Nav2's
  ``cmd_vel`` publisher switched to ``geometry_msgs/TwistStamped`` on
  Jazzy while ``mapoi_nav2_bridge`` still subscribed with plain
  ``Twist``, so the zero-velocity dwell check never received a message. A
  new ``cmd_vel_msg_type`` parameter (default ``"auto"``, resolved from
  ``ROS_DISTRO``) selects the correct type to subscribe with; subscribing
  both types on the same topic is not an option (it crashes with an
  invalid-allocator error in rclcpp), so exactly one is chosen. An
  explicit but unrecognized value also falls back to the
  ``ROS_DISTRO``-based default (with a ``WARN``) rather than always
  assuming ``Twist``.

* WebUI: the pose tool no longer commits a POI's position on the first
  click; position is only a preview (orange circle) until the second
  click confirms position + yaw together, fixing accidental position
  changes from a stray first click while editing (#269, #270). Escape now
  backs out of the yaw-selection phase to the position phase without
  committing anything, and is ignored while a text input/textarea has
  focus so it doesn't unexpectedly cancel the pose tool while typing.

* ``mapoi_turtlebot3_example``: a Gazebo GUI render/driver failure no
  longer brings down the whole demo (#294, #295); navigation continues
  headless. See ``docs/docker.md`` for the DRI/GPU-sharing prerequisites
  this relies on.

Samples
-------

* The ``turtlebot3_world`` demo was redesigned into a **feature-catalog**
  layout (#230, PR #235): POI count 9 -> 5, with routes restructured
  into one-feature-per-route tutorials plus an all-in-one ``tour_full``.
  This renames/removes the sample POI and route names, so launch files,
  clients, tutorials, or scripts that pin the old ``turtlebot3_world``
  names must migrate.

  - **POIs** (new): ``start`` / ``basic_waypoint`` / ``pause_waypoint``
    / ``goal`` / ``audio_landmark``. The old office-tour POIs
    (``elevator_hall``, ``corridor_a`` / ``corridor_b``,
    ``conference_room``, ``model_exhibit`` and the landmark variants)
    were removed. This is **not a 1:1 rename**: the demo was rebuilt
    around the feature-catalog layout, so old POIs have no direct
    new-name counterpart.
  - **Routes** (new): ``tutorial_01_basic`` / ``tutorial_02_landmark``
    / ``tutorial_03_pause`` / ``tour_full``. ``tour_full`` keeps its
    name but its waypoints / landmarks were rebuilt for the new POI set;
    the old ``tour_short`` was removed.
  - Consecutive-``pause`` coverage (old ``corridor_a`` + ``corridor_b``)
    moved into the ``test_poi_event_route_integration`` launch_test
    (#236) instead of the demo config.

* New ``PoiEvent``-driven sample subscribers in
  ``mapoi_turtlebot3_example``, demonstrating the custom-tag-dispatch
  pattern for downstream integrations (#88, #238, #248):
  ``audio_guide_node`` mocks an audio-guide announcement on
  ``EVENT_ENTER`` for ``audio_info``-tagged POIs, and ``camera_node``
  mocks a capture on ``EVENT_PAUSED`` for ``capture_trigger``-tagged POIs,
  then publishes ``mapoi/nav/resume`` (the message payload identifies the
  originating POI, e.g. ``"camera_node:<poi_name>"``) after a configurable
  ``capture_duration_sec`` to resume route navigation. Both subscribers
  launch from a separate ``mapoi_event_samples.launch.yaml`` (not bundled
  into the main demo launch by default, so the pattern stays discoverable
  without becoming a hidden dependency of the primary demo).
  ``camera_node`` uses an atomic compare-exchange to guard against
  re-entrant capture under a ``MultiThreadedExecutor``.

Internal / tests
----------------

* ``docs/testing-policy.md`` now documents the test-addition policy in
  writing (#347, #359): what counts as "critical core" worth CI-gating on
  humble/jazzy versus cheap non-critical coverage, and the rule for
  keeping the policy's own tables in sync as the test suite evolves.

* Continued the ``#193`` test-suite reorganization: critical-core vs.
  non-critical test triage, CI gating on humble/jazzy for the critical
  core, ``launch_test`` retry-gating outside PRs with a guard against a
  silently-empty test run, and new Playwright/jsdom/pytest coverage for
  the WebUI's Leaflet integration, POI drag/yaw-handle rendering,
  config-publish SSE path, and navigation REST API (#279-#291, plus
  feature-adjacent test additions across later PRs in this release).

* ``ros-test.yml``'s silent-test-loss guard no longer hard-codes
  ``EXPECTED_TARGETS``: it is now derived by grepping ``src/mapoi``'s
  ``CMakeLists.txt`` files for ``ament_(auto_)add_gtest`` /
  ``ament_add_pytest_test`` registrations, so the check can't silently
  drift out of sync again the way it repeatedly did in #297 / #344
  (#351).


0.4.0 (2026-05-08)
==================

**Notable for downstream users**: ``latest`` and ``vX.Y.Z`` (no distro
suffix) Docker tags now point at the **jazzy** build (was humble in
v0.3.0). Humble images remain available as ``humble`` /
``vX.Y.Z-humble``. See the "Default distro switch" entry below.

Breaking changes
----------------

* The Nav2 bridge node has been renamed from ``mapoi_nav_server`` to
  ``mapoi_nav2_bridge`` (#204, PR #218). Executable name, ROS 2 node name,
  C++ class (``MapoiNavServer`` → ``MapoiNav2Bridge``), header path
  (``mapoi_server/mapoi_nav2_bridge.hpp``), and the launch arg
  ``with_nav_server`` (now ``with_nav2_bridge``) all changed. No
  compatibility alias is provided. See ``docs/migration/v0.4.0.md`` for
  the full migration guide. Quick highlights:

  - Update launch files / scripts / docs that reference
    ``mapoi_nav_server`` (executable / node / arg name) to
    ``mapoi_nav2_bridge`` / ``with_nav2_bridge``.
  - ROS 2 launch silently ignores unknown args and applies defaults, so
    passing the old ``with_nav_server:=false`` to an updated
    ``mapoi_bringup.launch.yaml`` will be ignored and the bridge starts
    with its default. Audit your downstream launch with
    ``grep -rn 'with_nav_server:=\|mapoi_nav_server' --include='*.launch.*'``.
  - Auto-derived ROS 2 services (``/<node>/get_parameters``,
    ``/list_parameters``, ``/describe_parameters``, ``/set_parameters``)
    change with the node name. External monitoring tools like
    ``ros2 param get /mapoi_nav_server ...`` must be updated to
    ``/mapoi_nav2_bridge``.
  - ROS 2 topics, services (e.g. ``/mapoi/nav/...``,
    ``select_map``, ``get_pois_info``), explicitly declared parameters
    (``maps_path``, ``map_name``, ``config_file``, etc.), and Nav2 native
    actions (``navigate_to_pose`` etc.) are unchanged.

* AMCL adapter has been split out of the Nav2 bridge node into a new
  ``mapoi_amcl_localization_bridge`` executable in ``mapoi_server``
  (#209, PR #210). The bridge node (now called ``mapoi_nav2_bridge`` —
  see breaking entry above) is a Nav2-only navigation bridge and no
  longer publishes ``/initialpose``. Users who launch nodes individually
  must add ``mapoi_amcl_localization_bridge`` next to the bridge (the
  ``mapoi_bringup.launch.yaml`` already does this; new launch arg
  ``with_amcl_localization_bridge``, default ``true``). The following
  parameters moved from the old ``mapoi_nav_server`` to the new bridge:
  ``initial_pose_topic``, ``initialpose_retry_interval_sec``,
  ``initialpose_retry_max_attempts``,
  ``initialpose_post_subscribe_republish_count``. A new minimal contract
  message ``mapoi_interfaces/LocalizationBackendStatus`` and topic
  ``mapoi/localization/backend_status`` (``transient_local`` +
  ``MANUAL_BY_TOPIC`` liveliness, 5 s lease) are published by the bridge;
  WebUI / RViz panel now show a separate ``Localization`` indicator and
  gate the Set Initial Pose UI on it, independent from the Navigation
  indicator. See ``docs/backend-status.md`` for the full Navigation /
  Localization contract.

* System tag definitions (``waypoint`` / ``landmark`` / ``pause``) are now
  hardcoded in ``mapoi_server/include/mapoi_server/system_tags.hpp``
  (``mapoi::kSystemTags``); the
  ``mapoi_server/maps/tag_definitions.yaml`` file and the
  ``mapoi_server/maps/`` directory have been removed (#191, PR #192).
  The ``get_tag_definitions`` service contract is unchanged (3 system
  tags + user tags). Users who edited ``tag_definitions.yaml`` to
  override system tag names or descriptions must edit ``kSystemTags``
  and rebuild instead. Standard usage (no edits to system tags) is
  unaffected.

  Additional behavior changes worth noting:

  - Previously, if ``tag_definitions.yaml`` was missing or malformed,
    ``mapoi_server`` logged a warning and continued with an empty system
    tag list (a degenerate state in which ``get_tag_definitions`` returned
    0 system tags). The new implementation always exposes the 3 compiled-in
    system tags, removing the degenerate path.
  - The package install layout under ``share/mapoi_server/`` no longer
    contains a ``maps/`` directory. External scripts that listed
    ``share/mapoi_server/maps/`` must be updated.

* **Default distro switched from Humble to Jazzy** (PR #222). The
  Dockerfile ``ARG ROS_DISTRO`` default, ``docker-compose.yml``
  ``${ROS_DISTRO:-...}`` fallbacks, and the GHA tag-emit conditions for
  ``latest`` / ``vX.Y.Z`` (no distro suffix) all moved from ``humble`` to
  ``jazzy``. The motivation is that Gazebo Classic (shipped with the
  Humble image) reached EoL in 2025-01 and is no longer receiving
  upstream maintenance, while Jazzy bundles the actively maintained
  gz-sim (Gazebo Harmonic). Humble continues to be supported as a
  first-class option:

  - ``ghcr.io/shimz-robotics/mapoi:humble`` keeps shipping per-commit and
    per-release. ``vX.Y.Z-humble`` tags remain on every release.
  - ``ROS_DISTRO=humble docker compose build / up / run`` continues to
    produce ``mapoi:dev-humble`` / ``mapoi:demo-humble`` images.
  - The Dockerfile and compose file are unchanged for users who already
    pass ``ROS_DISTRO=humble`` explicitly.

  The break-on-upgrade case is users who pull ``ghcr.io/...:latest`` or
  ``ghcr.io/...:vX.Y.Z`` (no suffix) and expect a Humble build. From v0.4.0
  onward those tags resolve to a Jazzy image; pin ``:humble`` /
  ``:vX.Y.Z-humble`` to retain the old behavior.

Features
--------

* Navigation backend readiness contract (#198, PR #205). Introduced a
  minimal 3-field message ``mapoi_interfaces/NavigationBackendStatus``
  (``backend_type`` / ``backend_ready`` / ``reason``) published at 1 Hz on
  ``mapoi/nav/backend_status`` (``transient_local`` QoS). WebUI and RViz
  panel gate navigation operations on ``backend_ready`` alone, decoupling
  the UI from per-capability internals. Custom navigation bridges can
  integrate by populating the 3 fields, no other plumbing required.
  Localization readiness is exposed as a parallel
  ``LocalizationBackendStatus`` (#209 above).

* Liveliness QoS for ``backend_status`` topics (#208, PR #212). Both
  ``mapoi/nav/backend_status`` and ``mapoi/localization/backend_status``
  publishers now use ``MANUAL_BY_TOPIC`` liveliness with a 5 s lease;
  subscribers use ``AUTOMATIC`` liveliness. UI consumers detect bridge
  death via ``LivelinessChangedEvent`` (alive_count → 0) and treat the
  cached status as ``backend_ready=false`` regardless of the latched
  payload. Publishers without a finite liveliness QoS (legacy / no-config
  bridges) are rejected via QoS incompatibility (``pub.lease (∞) >
  sub.lease (5 s)``) — this is intentional, see the
  ``NavigationBackendStatus.msg`` header comment for the full rationale.

* WebUI Navigation detection and mobile-friendly focus UI (PR #197).
  WebUI now subscribes to ``mapoi/nav/backend_status`` and shows a
  ``Navigation connected / disconnected`` indicator; navigation control
  UI is enabled only when the bridge reports ``backend_ready=true``.
  Mobile (smartphone) focus interaction was reworked to match the panel
  layout for narrower screens.

* ``mapoi_gazebo_bridge`` AMCL drift fix on operator map switch (#91, PR
  #200). After a ``select_map`` flow the bridge now teleports the robot
  via Gazebo Classic ``delete_entity`` + ``spawn_entity`` re-creation to
  the first non-landmark POI, avoiding stale TF / costmap state in AMCL.

Fixes
-----

* ``backend_status`` 1 Hz publish keeps running during blocking calls
  (#213, PR #214). The Nav2 bridge node was previously a
  ``SingleThreadedExecutor`` with all callbacks in the default mutually
  exclusive group; ``send_load_map_request`` could block the executor
  for up to ~11 s per Nav2 map node and stall the ``backend_status``
  timer beyond the 5 s liveliness lease, causing false-positive
  ``LivelinessChanged`` events. The timer now runs on a separate
  ``MutuallyExclusive`` callback group within a
  ``MultiThreadedExecutor`` (thread count fixed at 2 to avoid
  ``hardware_concurrency()`` returning 1 in CPU-limited containers).

Documentation
-------------

* README split into a slim entry point + ``docs/`` directory (PR #223).
  Root ``README.md`` shrank from 430 → 95 lines (78% reduction); long
  sections moved to ``docs/migration/v0.4.0.md`` (rename + AMCL split +
  system tag hardcode), ``docs/migration/v0.3.0.md``,
  ``docs/docker.md`` (Docker demo / dev / GPU 詳細),
  ``docs/integration.md`` (自分のロボットへの導入手順),
  ``docs/backend-status.md`` (Navigation + Localization 仕様統合). Root
  README keeps package list, version policy, planned breaking changes,
  quickstart, Docker quickstart (Jazzy 1-liner), main features, and a
  table of links to the docs/.

* Custom navigation bridge ``backend_ready`` computation guidance
  (#207, PR #215). Added comments in
  ``mapoi_interfaces/msg/NavigationBackendStatus.msg``,
  ``mapoi_interfaces/README.md``, and the bridge source explaining how
  to compute ``backend_ready`` for partial-capability bridges
  (goal-only, route-only, etc.) and the privacy rules for the
  ``reason`` string (no credentials / absolute paths / internal hostnames
  / IPs / stack traces).

* CLI launch examples and ``maps_path`` / ``map_name`` argument
  semantics (#172, PR #219). Added a new section in
  ``mapoi_server/README.md`` with both ``$(ros2 pkg prefix --share ...)``
  and source-tree forms, an arguments table, and a worked example of the
  common ``maps_path``-points-to-config-file misuse with the actual
  FATAL log output.

* Docker + Intel/AMD 内蔵 GPU で RViz / Gazebo の **片方だけ** GUI を
  出す組合せ (例 ``gazebo_gui:=false``) のとき RViz が GL 初期化失敗で
  crash する問題 (#229, PR #296) の回避手段を整備。``docker-compose.dri.yml``
  (Intel/AMD iGPU 向け ``/dev/dri`` 共有 + ``group_add: [video, render]``
  の override) を新設し、``docs/docker.md`` に DRI override の使い方・
  GID 不一致時の対処・GPU 共有できない環境向けの software 経路
  (``gazebo_gui:=false`` で gz-sim を headless にして RViz だけ
  ``LIBGL_ALWAYS_SOFTWARE=1`` で起動) を追記。launch / アプリコードの
  変更はなし。

Internal / tests
----------------

* ``mapoi_interfaces`` docs ↔ implementation consistency lint (#216, PR
  #217). New CI step (``scripts/check_docs_consistency.py``) runs in the
  existing ``Consistency Check`` workflow and verifies (a) every
  ``rosidl_generate_interfaces``-listed msg / srv has a matching
  ``###`` heading in ``mapoi_interfaces/README.md``; (b)
  cross-references like ``mapoi_interfaces/msg/X.msg`` resolve to real
  files; (c) ``reason`` string literals in ``publish_backend_status``
  functions don't contain absolute paths / IPs / credentials /
  hostname-like literals. The first run of the lint also surfaced 3
  missing README sections (``TagDefinition`` / ``InitialPoseRequest`` /
  ``LocalizationBackendStatus``) which were filled in the same PR.

* Test suite reorganization and contract tests (PR #194, #195, #196).
  Test layout cleaned up (#193 in-progress); added
  ``test_system_tags_contract.cpp`` to pin ``kSystemTags`` against the
  WebUI tag list, ``test_resolve_backend_status_for_ui.py`` for the
  WebUI legacy-fallback / lost-liveliness logic, and three integration
  tests for ``EVENT_STOPPED`` / ``EVENT_RESUMED`` lifecycle (#177).


0.3.0 (2026-05-02)
==================

Breaking changes
----------------

* Runtime topic naming reorganized into the ``/mapoi/...`` namespace
  (#185, #70, #102). Legacy flat ``/mapoi_*`` runtime topics were removed
  without compatibility aliases; launch files, scripts, RViz configs, and
  external nodes must migrate to the new topic names listed below.

  - Marker topics consolidated to ``/mapoi/markers/pois`` and
    ``/mapoi/markers/routes`` in PR #186.
  - Non-marker mapoi topics moved to ``/mapoi/highlight/*`` /
    ``/mapoi/nav/*`` / ``/mapoi/events`` / ``/mapoi/config_path`` /
    ``/mapoi/initialpose_poi`` in PR #187.
  - The ``mapoi_rviz_pose`` topic from the RViz PoseTool is out of scope
    for this namespace reorganization and is unchanged.

  .. list-table::
     :header-rows: 1

     * - Old topic
       - New topic
     * - ``/mapoi_goal_marks``
       - ``/mapoi/markers/pois``
     * - ``/mapoi_event_marks``
       - ``/mapoi/markers/pois``
     * - ``/mapoi_route_marks``
       - ``/mapoi/markers/routes``
     * - ``/mapoi_highlight_goal``
       - ``/mapoi/highlight/goal``
     * - ``/mapoi_highlight_route``
       - ``/mapoi/highlight/route``
     * - ``/mapoi_goal_pose_poi``
       - ``/mapoi/nav/goal_pose_poi``
     * - ``/mapoi_route``
       - ``/mapoi/nav/route``
     * - ``/mapoi_cancel``
       - ``/mapoi/nav/cancel``
     * - ``/mapoi_pause``
       - ``/mapoi/nav/pause``
     * - ``/mapoi_resume``
       - ``/mapoi/nav/resume``
     * - ``/mapoi_switch_map``
       - ``/mapoi/nav/switch_map``
     * - ``/mapoi_nav_status``
       - ``/mapoi/nav/status``
     * - ``/mapoi_poi_events``
       - ``/mapoi/events``
     * - ``/mapoi_config_path``
       - ``/mapoi/config_path``
     * - ``/mapoi_initialpose_poi``
       - ``/mapoi/initialpose_poi``

* Sample maps removed from ``mapoi_server``; ``maps_path`` parameter
  is now required (#163 stage 1).

  - Before: ``mapoi_server`` shipped ``maps/turtlebot3_world/`` /
    ``maps/turtlebot3_dqn_stage1/``, with ``maps_path`` defaulting to
    ``{pkg_share}/maps``.
  - After: Sample maps live only in ``mapoi_turtlebot3_example``
    (single source of truth). ``mapoi_server`` no longer provides a
    default for ``maps_path``; omitting it is fatal at startup.
  - Retained: ``mapoi_server/maps/tag_definitions.yaml`` is kept as the
    package-internal system tag definition.
  - Migration: Set ``maps_path`` explicitly via launch or parameter. To
    reuse the sample, use
    ``$(find-pkg-share mapoi_turtlebot3_example)/maps``.

* ``mapoi_turtlebot3_example`` sample ``map_file`` names differentiated
  per world (#163 stage 2).

  - Before: ``turtlebot3_world/turtlebot3.{pgm,yaml}`` /
    ``turtlebot3_dqn_stage1/turtlebot3.{pgm,yaml}`` (same name in both
    worlds).
  - After: ``turtlebot3_world/turtlebot3_world.{pgm,yaml}`` /
    ``turtlebot3_dqn_stage1/turtlebot3_dqn_stage1.{pgm,yaml}`` (matches
    the directory name).
  - The ``map_file`` value and ``image:`` line in each
    ``mapoi_config.yaml`` were updated accordingly.
  - Migration: No effect unless launch files hard-code ``map_file``
    (it is normally referenced via ``mapoi_config.yaml``).
  - Related: ``scripts/check_sample_yaml_consistency.py`` no longer
    performs server/example pair-sync checks; it now only validates the
    example side individually.

* ``pub_interval_ms`` parameter removed from ``mapoi_server`` (#135).

  - Before: ``/mapoi_config_path`` was periodically published at
    ``pub_interval_ms`` (default ``5000`` ms, ``500`` ms in sample
    launches).
  - After: Published explicitly at startup, on ``SwitchMap``, and on
    ``reload_map_info``. The publisher keeps ``transient_local`` QoS
    and subscribers (``poi_editor`` / ``mapoi_panel`` /
    ``mapoi_webui_node``) were aligned to ``transient_local`` from the
    default QoS.
  - Migration: Remove ``pub_interval_ms`` from launch files and
    parameter YAMLs. (Passing it is harmless—ROS will only warn about
    an unused parameter—but it has no effect.)
  - Motivation: The periodic publish trigger rebuilt the
    ``poi_editor`` table / ``mapoi_panel`` ComboBox while the user was
    editing, interrupting selection and text input (surfaced during
    PR #168 verification). The ``transient_local`` QoS already solves
    the startup-order problem, so the heartbeat is no longer needed.

* ``initial_pose`` system tag removed (#89 stage 3, #144).

  - Before: POIs tagged ``initial_pose`` were auto-published on map
    load / switch.
  - After: The **first POI** in the new map's POI list (excluding
    ``landmark``-tagged POIs) is used as the default. The new
    ``initial_poi_name`` field on ``SwitchMap.srv`` allows explicit
    specification.
  - Publish path: When ``mapoi_server`` loads a new map, it publishes
    the POI name to ``/mapoi/initialpose_poi`` (transient_local);
    ``mapoi_nav_server`` receives it and forwards to ``/initialpose``.
  - YAML migration: Remove ``initial_pose`` from
    ``tags: [..., initial_pose]`` and place the starting POI first
    under ``poi:``.
  - ``mapoi_gazebo_bridge`` / ``mapoi_gz_bridge`` robot spawn positions
    follow the same semantics (POI list head).
  - ``reload_map_info`` service behavior change: After POI edits, it
    no longer republishes ``/mapoi/initialpose_poi`` (avoids the risk
    of rewinding the live robot pose). Re-initialization should go
    through ``SwitchMap`` (i.e., a map switch) or a manual path
    (RViz / WebUI / direct publish to ``/mapoi/initialpose_poi``).
  - ``mapoi_nav_server`` parameter changes: Removed
    ``initial_pose_subscriber_wait_timeout_sec`` and replaced it with
    an async retry-timer based design
    (``initialpose_retry_interval_sec``,
    ``initialpose_retry_max_attempts``,
    ``initialpose_post_subscribe_republish_count``). Prevents the
    regression where blocking waits stopped other callbacks.
  - The legacy ``/mapoi_initialpose_poi`` was migrated to
    ``/mapoi/initialpose_poi``, and the type changed from
    ``std_msgs/String`` to
    ``mapoi_interfaces/InitialPoseRequest`` (``{map_name, poi_name}``).
    To prevent SwitchMap-time topic synchronization races (bridges /
    ``mapoi_nav_server`` picking up a stale-generation POI name),
    subscribers now validate the generation via ``map_name``.
    Publishers: ``mapoi_server``, ``mapoi_webui`` (both updated).
    Subscribers: ``mapoi_nav_server``, ``mapoi_gazebo_bridge``,
    ``mapoi_gz_bridge`` (all updated).

* ``pause`` system tag firing conditions tightened (#89 stage 2, #143):

  - Before: Fired at any POI when ``nav_mode_ != IDLE`` (i.e., during
    GOAL or ROUTE navigation).
  - After: Fires only when ``nav_mode_ == ROUTE`` *and* the POI is in
    the active route's ``waypoints`` / ``landmarks``. Does not fire
    during GOAL navigation or IDLE.

  Behavioral change: A single ``/mapoi/nav/goal_pose_poi`` no longer
  triggers an auto-pause if the robot incidentally enters a ``pause``
  POI. Pause now runs only in route-based scenarios.

* ``GetRoutePois.srv`` response gained a ``landmark_pois`` field (#143).
  POIs referenced by ``route.landmarks`` in YAML appear here. They are
  not sent to Nav2 and are used only for radius monitoring and
  route-scoped pause firing.

* ``landmark × pause`` mutual exclusion added. ``landmark`` POIs are
  unreachable references, so pause cannot trigger on them. Rejected by
  WebUI / RViz Panel / API validation.

* System tag ``goal`` renamed to ``waypoint``. Both single navigation
  targets and route waypoints share this tag (the Nav2 navigation
  destination tag). YAML ``tags: [goal, ...]`` must be migrated to
  ``tags: [waypoint, ...]``. Topic / service / action names referring
  to "goal" (e.g. ``/mapoi/nav/goal_pose_poi``, ``goal_pose``) remain
  unchanged because they follow Nav2 native terminology.
  (#89 stage 1, #142)
* System tag ``origin`` removed. Map origin reference points should be
  expressed with the ``landmark`` system tag plus a custom tag (e.g.
  ``map_origin``) for color / semantic differentiation. Visualization
  defaults to gray; per-tag color is tracked in #70. (#89 stage 0)
* ``PointOfInterest.msg``: the ``float64 radius`` field was removed in
  favor of the new ``mapoi_interfaces/Tolerance tolerance`` struct
  (``xy`` + ``yaw``, aligned with Nav2 ``SimpleGoalChecker``). YAML
  ``poi.radius`` keys must be migrated to
  ``poi.tolerance: {xy, yaw}``. (#87)
* ``Tolerance.msg`` semantics: both ``xy`` and ``yaw`` are required to
  be ``>= 0.001`` (``xy``: 1 mm / ``yaw``: about 0.057°). Zero and
  negative values are disallowed (they would otherwise allow
  constructing a practically unresponsive POI). The "unspecified ->
  Nav2 default fallback" semantics is removed. Out-of-range values
  found at YAML load time are clamped to ``0.001`` with a WARN log.
  (#138)

* ``mapoi_nav_server`` ROS parameter ``radius_check_hz`` renamed to
  ``tolerance_check_hz`` (#140). Naming alignment for the
  ``PointOfInterest.radius`` -> ``tolerance.xy`` migration (#87).
  Default value, unit, and behavior are unchanged. Launch files and
  YAMLs that set ``radius_check_hz`` must rename it to
  ``tolerance_check_hz``.

* ``arrow_size_ratio`` parameter removed from ``mapoi_rviz2_publisher``
  (#136).

  - Before: ``arrow_size_ratio`` (double, default ``1.0``) dynamically
    sized POI arrows to ``radius × ratio``.
  - After: Arrows use a fixed scale (length ``0.15``, shaft / head
    ``0.04``) and serve as a directional cue for the sector
    visualization (a radius-coupled multiplier is no longer needed).
  - Migration: Remove any ``arrow_size_ratio`` declaration / set in
    existing launch files or YAMLs (the parameter itself is gone).

Interfaces
----------

* New ``mapoi_interfaces/msg/Tolerance.msg`` (``xy`` / ``yaw``). (#87)
* ``PoiEvent.msg``: new constants ``EVENT_STOPPED=3`` and
  ``EVENT_RESUMED=4``. Publish logic for these events is deferred to a
  follow-up issue. (#87)

WebUI / rviz_plugins
--------------------

* POI Editor (WebUI form / rviz_plugins POI Editor table) replaces the
  ``Radius`` input / column with ``tolerance.xy`` + ``tolerance.yaw``.
  Only the ``yaw`` UI input was displayed and entered in degrees;
  internal storage and YAML remained in radians (#138). The HTML
  ``min`` constraint and Panel ``ValidatePois`` enforce
  ``xy >= 0.001`` (m) / ``yaw >= 0.06`` (deg ≈ 0.001 rad).
* ``mapoi_webui_node``'s ``POST /api/pois`` validates the
  ``tolerance.{xy, yaw}`` min constraints on the backend as well, so
  frontend bypasses are caught (#138).
* RViz POI Editor column layout reorganized from 6 columns to 5 (#158):

  - Before: ``name / description / pose / tolerance.xy / tolerance.yaw (deg) / tags``.
  - After: ``name / pose (x, y, yaw rad) / tolerance (xy m, yaw rad) / tags / description``.
  - Tolerance is collapsed into one column (e.g., ``0.5, 0.7854``);
    ``yaw`` is now displayed in **radians** to stay consistent with
    ``pose.yaw`` (this reverts the temporary degree display introduced
    in #138).
  - The ``description`` column moved to the end (so long text doesn't
    crowd the table width).
  - Validation min constraints simplified to ``xy >= 0.001 m`` /
    ``yaw >= 0.001 rad`` (the old degree-conversion message is gone).
  - Validation gained a **new max constraint ``yaw <= 2π rad``**.
    Because this PR switched the UI input unit from degrees to
    radians, this guards against entering an old-style degree value
    (e.g., ``45``) that would otherwise be interpreted as ~7 turns of
    rotation. **Existing YAML files with ``yaw > 2π`` will fail to
    save**, so any out-of-range values must be corrected (yaw is
    conventionally expressed in ``[0, 2π]`` or ``[-π, π]``; values
    beyond that are practically never legitimate). A hard reject was
    chosen over a soft warning to prioritize typo prevention.

* WebUI now picks up config changes saved from RViz or external tools
  via SSE and reloads immediately (#135 (B)).

  - Before: Saving in the rviz PoiEditor did not propagate to the
    WebUI until the browser was reloaded.
  - After: ``mapoi_webui_node`` exposes a new ``/api/events`` SSE
    endpoint. When ``/mapoi/config_path`` is received, it broadcasts
    ``{type: "config_changed"}`` to all connected clients. The
    frontend (``app.js``) consumes the event via ``EventSource`` and
    re-fetches via ``loadTagDefinitions / loadPois / loadRoutes``.
  - Related: ``Flask app.run(threaded=True)`` is now set explicitly
    (so long-lived SSE connections and other requests run
    concurrently).
  - Closes #135 in combination with #135 (A) addressed in PR #168
    (PoiEditor / MapoiPanel callback fixes).

* POI tolerance is rendered as a sector marker in
  ``mapoi_rviz2_publisher`` and ``mapoi_webui`` (#136).

  - Radius = ``tolerance.xy``; sector angle = ``2 * tolerance.yaw``;
    centerline = ``pose.yaw``.
  - ``waypoint`` = filled sector, ``landmark`` = hollow sector,
    ``pause`` = dashed outline overlay (a thin solid line is used for
    the primary glyph for contrast).
  - The sector is drawn only when ``0 < tolerance.yaw < π``; otherwise
    (yaw-agnostic) the POI is treated as a full circle (allowing
    ``pass_through`` to be expressed).
  - New ``show_tolerance_sector`` parameter on
    ``mapoi_rviz2_publisher`` (bool, default ``true``) toggles the
    tolerance visualization at runtime.
  - Subsequently #179 split this into a circle (xy detection region)
    + sector (yaw constraint) overlay (see below).

* POI tolerance split into circle + sector overlays, and the pause
  overlay reworked into a dot pattern in
  ``mapoi_rviz2_publisher`` / ``mapoi_webui`` (#179, addressing user
  feedback from #178).

  - Circle outline (thin solid line, faint): always shown to indicate
    the robot's ``tolerance.xy`` entry-detection region. Drawn
    yaw-agnostically as a visual cue for the detection logic
    (Euclidean ``< tolerance.xy``).
  - Sector (filled or hollow): emphasizes the yaw constraint. Drawn
    only when ``0 < tolerance.yaw < π`` to avoid information
    redundancy with the full circle.
  - **Visual change**: Previously (#178), yaw-agnostic POIs
    (``tolerance.yaw == 0`` or ``>= π``) were also drawn as a sector
    that happened to cover the full circle (filled green for
    ``waypoint``, hollow gray for ``landmark``). With this change,
    yaw-agnostic POIs show **only the thin faint circle outline**
    without the filled or hollow stroke. This matches the detection
    semantics (xy only), but viewers used to the old display will
    notice a shift (worth checking before reusing demos / screenshots).
  - Pause overlay: The old dashed style (``dashArray: '6, 4'`` /
    RViz segment 0.05 m, 1:1 ratio) was reported as "doesn't read as
    dots, just looks crushed" (#178 PR comments), so it was changed
    to a sparse dot pattern. WebUI uses ``dashArray: '2, 6'`` with
    ``lineCap: round`` (25 % cycle on); RViz uses dots 0.02 m long
    with 0.10 m gaps and a 0.04 m line (20 % cycle on). The dots are
    overlaid along the xy circle so the boundary aligns with the
    pause-firing condition (inside the xy circle).
  - The ``show_tolerance_sector`` parameter keeps its old name but now
    controls the entire circle + sector + pause overlay (preserves
    backward compatibility).
  - **Maintenance note**: The drawing spec is duplicated in
    ``mapoi_rviz2_publisher.cpp`` (RViz) and ``map-viewer.js``
    (WebUI). Spec changes must be applied to both (continuing from
    #136; unifying the two is being considered together with #70).
  - **POI Editor Panel Display Settings UI fix**: When PR #178
    removed ``arrow_size_ratio`` from the publisher, the Panel's
    DoubleSpinBox UI was left in place, causing
    ``get_parameters`` / ``set_parameters`` calls to fail (a
    regression). This PR replaces that UI with a checkbox for
    ``show_tolerance_sector`` so the runtime display-layer toggle is
    available from the Panel.

* Legacy ``event`` tag ``elif`` branch removed from
  ``mapoi_rviz2_publisher`` (#180).

  - ``event`` was originally a system tag but had been demoted to a
    custom tag in #89 stage 1. The RViz publisher still had a
    hard-coded branch drawing a blue arrow + label for it, which
    PR #178's cursor review flagged as high-priority "legacy
    leftover" follow-up.
  - This PR removes the ``elif`` branch. POIs with the ``event`` tag
    are now drawn (sector + arrow) only if they also carry another
    system tag (``waypoint`` / ``landmark``); otherwise they are
    omitted from the visualization, matching the behavior of other
    custom tags.
  - Composite-tag POIs such as ``hazard_south`` are reorganized to
    ``[landmark, hazard]`` (sample-side change, see Samples).
  - Color / glyph reorganization is tracked in #70.

Samples
-------

* Sample YAMLs (``turtlebot3_world`` / ``turtlebot3_dqn_stage1``)
  reworked to cover the full feature matrix (#146). Strengthens
  regression coverage for new features (``route.landmarks`` #143,
  sector rendering #136, ``tolerance.yaw`` #138).

  - ``turtlebot3_world`` (office tour scenario): POI count 5 -> 9.
    Adds consecutive ``pause`` (``corridor_a`` + ``corridor_b``), a
    photography stop (``conference_room`` with
    ``tolerance.yaw=0.10``), yaw-agnostic passes (``corridor_*`` with
    ``tolerance.yaw=π``), three landmark variations (pure
    ``landmark`` / ``landmark + audio_info`` /
    ``landmark + capture_target``), and a contrast between
    ``tour_full`` (with both waypoints and landmarks) and
    ``tour_short`` (no landmarks). New custom tags
    ``capture_trigger`` / ``capture_target``.
  - ``turtlebot3_dqn_stage1`` (obstacle-avoidance sandbox): POI count
    5 -> 7. Adds composite-tag hazards (``landmark + hazard`` -
    originally ``[event, landmark, hazard]`` but reorganized to
    ``[landmark, hazard]`` after the ``event`` tag removal in #180),
    yaw-agnostic pass-through points (``checkpoint_west`` /
    ``checkpoint_east`` with ``tolerance.yaw=π``, used in lieu of a
    ``pass_through`` tag), a pause checkpoint
    (``pause_intersection``), and a contrast between ``avoidance_a``
    (waypoints + landmarks) and ``avoidance_b`` (waypoints only). New
    custom tag ``observation``.
  - ``mapoi_turtlebot3_example/README.md`` gained a sample summary
    table listing the features / POI-tag composition each scenario
    exercises.

  **Route renames** (old -> new):

  - ``turtlebot3_world``: ``route_1`` -> ``tour_full``, ``route_2`` ->
    ``tour_short``.
  - ``turtlebot3_dqn_stage1``: unchanged (``avoidance_a`` /
    ``avoidance_b``).

  ``tour_short`` is functionally compatible with the old ``route_2``
  (``[elevator_hall, corridor_a, conference_room]``); the path is the
  same. Launch files, clients, and internal scripts that reference
  ``route_1`` / ``route_2`` directly must migrate to the new names.
  External tutorials that pin the sample YAML snapshot also need
  updates because the POI count, names, and tag combinations have
  expanded significantly.

* New ``scripts/check_sample_yaml_consistency.py`` plus CI hookup
  (#146). On pull-request and push, it automatically verifies POI
  name / route waypoint / landmark reference consistency, tag
  exclusion rules (``waypoint × landmark``, ``landmark × pause``),
  and the ``tolerance.{xy, yaw} >= 0.001`` minimum constraint in
  sample YAMLs.

Fixes
-----

* ``mapoi_server`` ``reload_map_info`` service now explicitly publishes
  a skip message (empty ``poi_name``) to ``/mapoi/initialpose_poi``
  (#154). This eliminates the stale-message problem where the
  ``transient_local`` (depth=1) latched value retained the pre-reload
  POI name, causing late-starting ``mapoi_nav_server`` instances or
  reconnecting RViz panels to receive a pre-edit POI name. Subscribers
  (``mapoi_nav_server`` / ``mapoi_gazebo_bridge`` / ``mapoi_gz_bridge``)
  ignore empty ``poi_name`` by convention, so the live pose is not
  rewound (the invariant established in #149 round 4 is preserved).

Navigation server
-----------------

* ``mapoi_nav_server`` now publishes ``EVENT_STOPPED`` /
  ``EVENT_RESUMED`` (#140). Activates the constants added to
  ``PoiEvent.msg`` in #87. Two OR-combined trigger sources:

  - **Nav2 action SUCCEEDED**: When the ``FollowWaypoints`` /
    ``NavigateToPose`` result callback detects ``SUCCEEDED``,
    ``EVENT_STOPPED`` is published immediately for every POI in the
    inside state (without waiting for the ``cmd_vel`` dwell).
  - **cmd_vel based**: When both linear and angular velocity stay
    below ``stopped_speed_threshold`` (default ``0.01``) for at
    least ``stopped_dwell_time_sec`` (default ``1.0`` s),
    ``EVENT_STOPPED`` is published. Evaluated on every
    ``tolerance_check_callback`` tick.
  - **EVENT_RESUMED**: When a STOPPED POI's velocity goes back above
    the threshold, ``EVENT_RESUMED`` is published immediately (no
    dwell required). It is also published for all STOPPED POIs upon
    receiving a new ``/mapoi/nav/goal_pose_poi`` / ``/mapoi/nav/route``
    so subscribers can detect "pause released" without delay.
  - **On EXIT**: ``poi_inside_state_`` -> false also resets
    ``poi_stopped_state_``. The EXIT event implicitly covers "pause
    released", so RESUMED is not published here. The lifecycle is
    organized as ENTER -> STOPPED -> EXIT.

  New ROS parameters:

  - ``stopped_speed_threshold`` (default ``0.01`` m/s equivalent).
  - ``stopped_dwell_time_sec`` (default ``1.0``).
  - ``cmd_vel_topic`` (default ``cmd_vel``).

  The state-machine pure function ``compute_stopped_transition`` was
  extracted, with five new unit tests added in
  ``test_nav_server_unit``.

Internal
--------

* Initial-pose selection logic extracted into a shared header
  ``mapoi_server/include/mapoi_server/initial_pose_resolver.hpp``,
  unifying the three duplicate implementations across
  ``mapoi_server`` / ``mapoi_gazebo_bridge`` / ``mapoi_gz_bridge``
  (#150). Prevents future divergence between simulator spawn behavior
  and ``/initialpose`` if the ``initial_poi_name`` specification
  (exclusion rules / fallback) evolves. Behavior is preserved (pure
  function rename and file relocation only; existing unit tests still
  pass).


0.2.0 (2026-04-29)
==================

WebUI
-----

* New ``robot_radius`` ROS parameter on ``mapoi_webui_node`` (default
  ``0.15``). Determines the robot marker size and the active-route
  connector arrival threshold; previously hard-coded at 0.15. Value is
  also propagated to the frontend through ``/api/nav/status``. (#116,
  #117, #118, #126)
* Active-route highlight now propagates to direction arrows and order
  labels, and non-active routes are dimmed for focus. (#69, #105)
* Parallel-offset rendering for overlapping route polylines.
  Direction-independent canonical normal places reverse-direction routes
  on opposite sides of the shared segment. (#69, #128)
* Directional connector arrow from the current robot position to the
  first waypoint of the active route. Visual endpoint follows the offset
  polyline; physical reach detection uses the original POI coordinates.
  (#106, #107)
* POI / route name uniqueness validation, enforced on both the client
  side and the backend ``POST /api/pois`` / ``POST /api/routes``
  endpoints. (#109, #120)
* Sidebar UX: drag-resize handle with width persisted in
  ``localStorage``, sticky form-actions footer for narrow viewports, and
  ellipsis with hover tooltip for long names. (#122, #123, #124, #125)
* Navigation dropdowns auto-select the currently focused POI or route.
  (#111, #114)
* Robot marker color matches the active route's color. (#110, #113)
* POI editor: visible "SAVED" feedback after save, and table rebuilt
  post-save to keep row numbering aligned with visual order. (#74, #76,
  #77, #95)
* ``tag_definitions`` is fetched through the ``get_tag_definitions``
  service rather than read from yaml directly. (#39, #59)
* New ``mapoi_editor.launch.yaml`` for headless POI editing without
  ``mapoi_nav_server``; service / publisher calls degrade gracefully.
  (#60, #62)

Navigation server
-----------------

* ``mapoi_nav_status`` now uses ``transient_local`` QoS so late-starting
  rviz panels and the WebUI immediately receive the latest state. (#96,
  #103)
* ``mapoi_nav_status`` payload extended to the ``"status:target"`` form,
  preserving the target name through ``succeeded`` / ``aborted`` /
  ``canceled`` transitions. (#104, #119)
* Localization-agnostic Tier 1 support: ``initial_pose`` topic and
  timeout are configurable; AMCL ``set_initial_pose`` calls are
  de-duplicated. (#57, #58)
* ``/initialpose`` auto-publish now waits for subscriber readiness
  before sending. (#33, #56)
* Map switch detection on ``mapoi_config_path`` uses both path and
  mtime as the change guard. (#80, #81)

RViz plugins / publisher
------------------------

* New "Display Settings" group in the POI Editor panel. (#99, #100)
* POI radius rendered as a floor-plane circle (LINE_STRIP) for all
  POIs. (#67, #71)
* POI arrow size scales with radius via the new ``arrow_size_ratio``
  parameter. (#65, #72)
* POI labels are configurable and include line numbers. (#66, #73)
* All routes published as LINE_STRIP via ``mapoi_route_marks``; rviz
  display names made descriptive. (#68, #83, #101)
* ``mapoi_config_path`` subscription dynamically refreshes the
  displayed POI list. (#75, #78)
* Removed ``nav2_rviz_plugins/Selector`` and ``Docking`` panels from
  the default rviz config (not present on Humble). (#37, #51)

Simulation bridges
------------------

* ``mapoi_gazebo_bridge`` (Humble / Gazebo Classic): SwitchMap now
  performs Gazebo entity swap and robot delete + respawn. (#30, #46)
* ``mapoi_gz_bridge`` (Jazzy / gz-sim): SwitchMap performs entity swap
  with ``SetEntityPose`` for atomic robot teleport. (#48, #52)
* Symmetric ``NoGazeboSection`` cleanup and queue coalescing across
  both bridges. (#53, #54)

Docker / development
--------------------

* Distro-specific dev / demo image tags (``mapoi:dev-${ROS_DISTRO}``,
  ``mapoi:demo-${ROS_DISTRO}``) so parallel humble / jazzy runs do not
  invalidate each other's build cache. ``ROS_DISTRO`` inherit / override
  behaviour documented in ``README.md``. (#90, #92, #93)
* ``CYCLONEDDS_URI`` is forced to empty inside containers to prevent
  host configuration leak; ``RMW_IMPLEMENTATION`` and ``ROS_DOMAIN_ID``
  inherit host values for host-container discovery. (#84, #94)
* ``.env.example`` auto-sets ``USER_ID`` / ``GROUP_ID`` for bind-mount
  ownership. (#97, #98)

Examples
--------

* New ``gz_sim_headless_aware.launch.yaml`` wrapper for Jazzy
  headless launches. (#42, #49)
* TurtleBot3 ``turtlebot3_world`` and ``turtlebot3_dqn_stage1``
  example configs reworked with distinct themes and Nav2-aligned
  radii. (#79, #82)

CI / tests
----------

* New ``robot_radius`` drift check that compares the WebUI launch arg
  against the Nav2 ``burger.yaml`` ``robot_radius`` for both humble
  and jazzy parameter sets. (#127, #131)
* Vitest unit-test infrastructure and pure ``geometry.js`` helper
  extracted from ``MapViewer._offsetLatLngs``. (#129, #132)

Fixes
-----

* Fixed route editing lifecycle: ``MapViewer`` selection state could
  drift from the active route. (#112)
* Fixed ``mapoi_route_cb`` ``std::bind`` compile error on Humble by
  switching to a lambda capture. (#121)
* Removed redefinition warnings in ``test_nav_server_unit.cpp``.
  (#47, #50)

Internal
--------

* Stripped historical PR references and stale comments from
  production code. (#63, #64)

Known issues
------------

* AMCL drift after SwitchMap on Gazebo Classic (Humble) only;
  reproduces on Classic but not on gz-sim (Jazzy). Workaround: use
  Jazzy / gz-sim, or manually re-set the initial pose in rviz.
  (#91)
* Switching maps from the WebUI dropdown does not update the
  backend's current map context, so POI / route lists may show
  stale data. Workaround: set ``map_name`` via launch parameter or
  call the ``/switch_map`` service directly. (#130)
* The project is not yet registered with rosdep / rosdistro;
  ``apt`` / ``rosdep`` installation is not yet available. (#20)

Contributors: Shunsuke Kimura
