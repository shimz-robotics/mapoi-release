# Contributing to mapoi

## Development setup

Clone into a colcon workspace and build (CI uses the same `src/mapoi` layout):

```sh
# cd path/to/your_ws
git clone https://github.com/shimz-robotics/mapoi.git src/mapoi
rosdep install --from-paths src --ignore-src -r -y
colcon build --symlink-install --cmake-args -DBUILD_TESTING=ON
source install/setup.bash
```

CI ([.github/workflows/ros-test.yml](./.github/workflows/ros-test.yml)) builds and tests on **both Humble and Jazzy**, so changes must work on both distros. If you develop on a single distro, the Docker dev environment can verify the other one (e.g. `ROS_DISTRO=humble docker compose run --rm dev`; see [docs/docker.md](./docs/docker.md)).

## Workflow

1. Open an Issue first (see the issue templates) and describe the bug or proposal
2. Create a branch and open a PR that references the Issue — no direct pushes to `main`
3. All CI checks must pass before merge

## Tests

| PR gate | Workflow |
| --- | --- |
| C++ gtest + pytest (`colcon test`, `launch_test` excluded) | `ros-test.yml` (humble / jazzy matrix) |
| WebUI vitest + Playwright e2e, Python consistency scripts | `consistency-check.yml` |

The slow `launch_test` integration tests are excluded from the PR gate; they run on main push, the weekly schedule, and manual dispatch.

Whether a change needs a **new** test at all is a deliberate decision — see [docs/testing-policy.md](./docs/testing-policy.md) (Japanese) for the criteria (criticality × execution cost × maintenance load) and the extra bar for new `launch_test` / e2e files.

## Docs assets

The screenshots in `docs/images/` are captured from the Web UI running the `turtlebot3_world` demo (the ROS-independent e2e mock server under `mapoi_webui/tests/web/e2e/` can serve the same UI for capturing). Keep images small — optimized PNG, roughly under 100 KB each.

## Before opening a PR

- `colcon test --ctest-args -LE launch_test` passes locally (this matches the PR gate), then check with `colcon test-result --verbose`
- The README of every package whose behavior or interface you changed is updated (each package ships its own README)
