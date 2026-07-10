"""Kernel snapping queries (rm.edit.snap_point)."""

import math

import pytest

import roadmaker as rm


@pytest.fixture
def network():
    """One straight road along +X from (0, 0) to (100, 0)."""
    net = rm.RoadNetwork()
    rm.author_clothoid_road(
        net,
        [(0.0, 0.0), (100.0, 0.0)],
        rm.LaneProfile.two_lane_default(),
        "Straight",
        "1",
    )
    return net


def test_endpoint_beats_tangent_and_grid(network):
    result = rm.edit.snap_point(
        network, (99.5, 0.5), rm.edit.SnapOptions(radius=2.0, grid=1.0)
    )
    assert result is not None
    assert result.kind == rm.edit.SnapKind.RoadEndpoint
    assert result.position.x == pytest.approx(100.0, abs=1e-4)
    assert result.position.y == pytest.approx(0.0, abs=1e-4)
    assert result.road == network.find_road("1")
    assert result.heading is None


def test_tangent_continuation_beyond_the_end_carries_heading(network):
    result = rm.edit.snap_point(network, (105.0, 0.5))
    assert result is not None
    assert result.kind == rm.edit.SnapKind.TangentContinuation
    assert result.heading == pytest.approx(0.0, abs=1e-9)
    assert result.position.x == pytest.approx(105.0, abs=1e-4)
    assert result.position.y == pytest.approx(0.0, abs=1e-4)
    assert result.road == network.find_road("1")


def test_tangent_at_the_start_points_away_from_the_road(network):
    result = rm.edit.snap_point(network, (-5.0, 0.3))
    assert result is not None
    assert result.kind == rm.edit.SnapKind.TangentContinuation
    assert abs(math.remainder(result.heading - math.pi, math.tau)) < 1e-9


def test_grid_snap_when_no_road_is_near(network):
    result = rm.edit.snap_point(network, (50.3, 20.4), rm.edit.SnapOptions(grid=1.0))
    assert result is not None
    assert result.kind == rm.edit.SnapKind.Grid
    assert result.position.x == pytest.approx(50.0)
    assert result.position.y == pytest.approx(20.0)
    assert result.road is None
    assert result.heading is None


@pytest.mark.parametrize(
    "options",
    [
        rm.edit.SnapOptions(),  # no grid, far from the road
        rm.edit.SnapOptions(grid=10.0),  # grid point 7.07 m away > radius
    ],
)
def test_nothing_in_range_returns_none(network, options):
    assert rm.edit.snap_point(network, (45.0, 45.0), options) is None


def test_disabled_kinds_are_skipped(network):
    options = rm.edit.SnapOptions(endpoints=False, tangent=False)
    assert rm.edit.snap_point(network, (99.9, 0.1), options) is None
