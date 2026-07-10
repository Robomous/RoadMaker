"""RoadMaker: road-network authoring for AV simulation.

C++ kernel with ASAM OpenDRIVE I/O, clothoid authoring, mesh generation,
and glTF export. See https://github.com/robomous/roadmaker.
"""

from ._roadmaker import (  # noqa: F401
    ContactPoint,
    Diagnostic,
    Junction,
    Lane,
    LaneProfile,
    LaneSection,
    LaneSpec,
    LaneType,
    MeshOptions,
    NetworkMesh,
    PathPoint,
    Poly3,
    ReferenceLine,
    Road,
    RoadMark,
    RoadMarkType,
    RoadNetwork,
    Severity,
    author_clothoid_road,
    build_network_mesh,
    export_glb,
    load_xodr,
    parse_xodr,
    save_xodr,
    version,
    write_xodr,
)

__version__ = version()
__all__ = [name for name in dir() if not name.startswith("_")]
