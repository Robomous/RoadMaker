# Copyright 2026 Robomous
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Put a scene on the map: the world georeference (p7-s5, #324).

A RoadMaker scene is authored in a local Cartesian frame — metres from an
origin that means nothing in particular. Giving it a world georeference says
where that origin actually is, and OpenDRIVE §8.5 carries the answer in
``<header><geoReference>``, so it travels in the .xodr itself rather than in a
RoadMaker-only sidecar.

The interesting part is what this does NOT need. Converting between two
different geodetic datums needs the PROJ library, which RoadMaker does not
depend on (it arrives with the GIS import sprint, p7-s2). §8.5 offers the way
out itself:

    Alternatively define a custom projection using Transverse Mercator (TM).

``tmerc_projection`` builds exactly that — a Transverse Mercator centred on the
scene's own origin, with unit scale and no false easting or northing. In that
projection the local OpenDRIVE coordinates and the projected coordinates are
the same numbers, so the scene is georeferenced without anything ever being
transformed.

Run:  python python/examples/world_georeference.py
"""

import tempfile
from pathlib import Path

import roadmaker as rm


def main() -> None:
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (120.0, 0.0), (240.0, 60.0)],
        rm.LaneProfile.two_lane_default(),
        "Quai Branly",
        "1",
    )

    # A scene with no georeference is a plain local Cartesian frame — which is
    # what §8.5 says a reader must assume when the definition is missing.
    print(f"georeferenced at the start? {not net.georeference.empty}")

    # The Eiffel Tower, as the scene's origin.
    latitude, longitude = 48.858844, 2.294351
    geo = rm.GeoReference()
    geo.projection = rm.tmerc_projection(latitude, longitude)
    print(f"\ngenerated CRS:\n  {geo.projection}")

    # Georeferencing is an ordinary undoable edit, so it belongs on the stack
    # with everything else. It dirties nothing: saying where the scene sits does
    # not move anything in it.
    stack = rm.edit.EditStack()
    stack.push(net, rm.edit.set_georeference(net, geo))

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "quai_branly.xodr"
        rm.save_xodr(net, str(path))

        header = path.read_text().split("\n")[1:4]
        print("\nwhat the file carries:")
        for line in header:
            print(f"  {line.rstrip()}")

        reloaded, _ = rm.load_xodr(str(path))
        origin = rm.tmerc_origin(reloaded.georeference.projection)
        print(f"\nreloaded origin: {origin[0]}, {origin[1]}")
        print(f"exact round trip? {origin == [latitude, longitude]}")

    # A CRS RoadMaker did not author is carried verbatim and reported as
    # opaque. Guessing at a UTM zone's origin would be wrong by its 500 km
    # false easting, so declining is the honest answer until PROJ lands.
    foreign = "+proj=utm +zone=31 +ellps=GRS80 +units=m +no_defs"
    print(f"\norigin of {foreign!r}:\n  {rm.tmerc_origin(foreign)}")

    # <offset> is the other half of §8.5: a whole-dataset shift, applied to the
    # local coordinates before any datum conversion. Its affine is available on
    # its own, and inverts exactly.
    offset = rm.GeoOffset()
    offset.x, offset.y, offset.z = 448_000.0, 5_411_000.0, 0.0
    world = rm.geo_to_world(offset, 240.0, 60.0, 0.0)
    print(f"\nthe scene's far end in world coordinates: {world[0]:.1f}, {world[1]:.1f}")
    print(f"and back: {rm.geo_to_local(offset, *world)[:2]}")


if __name__ == "__main__":
    main()
