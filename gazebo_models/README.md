# Vendored Gazebo default models

This directory contains the minimal subset of Gazebo default models
required by `turtlebot3_*.world` files when running the Docker demo.

## Why vendor them

The Docker image sets `GAZEBO_MODEL_DATABASE_URI=` (empty) to prevent
Gazebo from blocking on `gazebosim.org` at startup. With the URI
disabled, Gazebo cannot fetch these models online, so we ship them in
the repo and `COPY` them into `~/.gazebo/models/` at image build time.

## Source and license

Copied verbatim (no modifications) from the upstream
[osrf/gazebo_models][upstream] repository.

- `ground_plane/` — flat infinite ground plane with collision
- `sun/` — directional light source

Both models are:

- Copyright 2012 Nate Koenig / Open Source Robotics Foundation
- Licensed under the [Creative Commons Attribution 3.0 Unported
  License][cc-by-3.0] (CC-BY 3.0)

Original author attribution is preserved in each `model.config`
(`<author>` element). The files are distributed here under the same
CC-BY 3.0 terms.

## Updating from upstream

Run from this directory (`mapoi_turtlebot3_example/gazebo_models/`):

```bash
for model in ground_plane sun; do
  for file in model.config model.sdf; do
    curl -fsSL -o ${model}/${file} \
      https://raw.githubusercontent.com/osrf/gazebo_models/master/${model}/${file}
  done
done
```

[upstream]: https://github.com/osrf/gazebo_models
[cc-by-3.0]: https://creativecommons.org/licenses/by/3.0/legalcode
