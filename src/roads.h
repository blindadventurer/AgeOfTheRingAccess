#pragma once
#include <string>
#include <vector>

// Road network of the mission maps: the waypoint paths the designers drew for their own
// units (marches, patrols, cutscene walks), written by tools\make_roads.py into roads.txt
// (one section per map, found by its objective tags). The mod joins paths that come
// within 40 m of each other, so separate paths make one network.
void RoadsLoad(const std::wstring& path);

struct RoadPoint { float x, y; };

// Points to walk through, along the roads of the current map, from the unit (ux, uy) to the
// target (tx, ty); neither end is included. tags: the current objective tags (the map is the
// one whose section lists one of them). maxX/maxY: the active map border in world units
// (0: unknown); road points beyond it are not used. With allowDirect the straight line is a
// choice too and wins unless the roads are less than half again as long (out stays empty);
// without it the roads are the only way (a straight move order has already stalled).
// Returns false when no map matches or no road gets close to both ends.
// The folder of the map whose objectives these are ("map good weathertop"), empty when unknown.
std::string RoadsMapName(const std::vector<std::string>& tags);
bool RoadsPlan(const std::vector<std::string>& tags, float ux, float uy, float tx, float ty, float maxX,
               float maxY, bool allowDirect, std::vector<RoadPoint>& out);
