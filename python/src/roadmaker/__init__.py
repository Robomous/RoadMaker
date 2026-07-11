"""RoadMaker: road-network authoring for AV simulation.

C++ kernel with ASAM OpenDRIVE I/O, clothoid authoring, mesh generation,
and glTF export. See https://github.com/robomous/roadmaker.
"""

from ._roadmaker import (  # noqa: F401
    ContactPoint,
    Diagnostic,
    Junction,
    JunctionId,
    Lane,
    LaneId,
    LaneProfile,
    LaneSection,
    LaneSectionId,
    LaneSpec,
    LaneType,
    MeshOptions,
    NetworkMesh,
    PathPoint,
    Poly3,
    ReferenceLine,
    Road,
    RoadEnd,
    RoadId,
    RoadMark,
    RoadMarkType,
    RoadNetwork,
    Severity,
    Waypoint,
    XodrVersion,
    author_clothoid_road,
    build_network_mesh,
    edit,
    export_glb,
    fit_elevation_profile,
    load_xodr,
    parse_xodr,
    save_xodr,
    validate_network,
    version,
    write_xodr,
)

__version__ = version()
__all__ = [name for name in dir() if not name.startswith("_")]
