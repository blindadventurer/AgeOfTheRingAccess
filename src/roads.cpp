// Road network of the mission maps (roads.txt from tools\make_roads.py) and a shortest
// way over it (Dijkstra), for the scanner's route walk.
#include "roads.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include "log.h"

namespace {

struct RoadMap {
    std::string name;
    std::set<std::string> tags;  // upper-case objective tags of the map's map.ini
    std::vector<RoadPoint> nodes;
    std::vector<std::pair<int, int>> links;
    std::vector<std::vector<std::pair<int, float>>> adj;  // built on first use
};
std::vector<RoadMap> g_maps;
int g_lastMap = -1;

// Distances in world units (10 per metre). Almost every designer path is a chain of its
// own (Council of Elrond: 95 paths, 96 separate pieces), so paths closer than 40 m are
// joined by a straight step costing 1.3 times its length; a road link costs its length.
// Getting onto the roads and off them costs 3 times the length, so a way takes the road
// nearest to the unit and leaves it nearest to the target: with 1.2 the planner cut
// across (Council of Elrond, (1558, 962) to the end area gave no road point; with 3 it
// gives the cave floor waypoint (1392, 1034), tested offline 2026-11-01). The straight
// line, when allowed, costs 1.5 times: a road way wins only when it is not much longer.
constexpr float kJoin = 400.f, kAccess = 600.f;
constexpr float kJoinCost = 1.3f, kAccessCost = 3.f, kDirectCost = 1.5f;
// Road points closer than 15 m to the previous one are left out: fewer orders, same way.
constexpr float kMinSpacing = 150.f;

std::string Upper(std::string v) {
    for (char& c : v) c = (char)toupper((unsigned char)c);
    return v;
}

float Dist(const RoadPoint& a, float x, float y) { return std::hypot(a.x - x, a.y - y); }

void Build(RoadMap& m) {
    size_t n = m.nodes.size();
    m.adj.assign(n, {});
    std::set<std::pair<int, int>> linked;
    for (auto [a, b] : m.links) {
        float w = Dist(m.nodes[a], m.nodes[b].x, m.nodes[b].y);
        m.adj[a].push_back({b, w});
        m.adj[b].push_back({a, w});
        linked.insert({std::min(a, b), std::max(a, b)});
    }
    int joins = 0;
    for (int a = 0; a < (int)n; ++a)
        for (int b = a + 1; b < (int)n; ++b) {
            float w = Dist(m.nodes[a], m.nodes[b].x, m.nodes[b].y);
            if (w >= kJoin || linked.count({a, b})) continue;
            m.adj[a].push_back({b, w * kJoinCost});
            m.adj[b].push_back({a, w * kJoinCost});
            ++joins;
        }
    wchar_t t[200];
    swprintf_s(t, L"roads: %S: %zu points, %zu links, %d joins within 40 m", m.name.c_str(), n, m.links.size(), joins);
    LogLine(t);
}

int FindMap(const std::vector<std::string>& tags) {
    auto matches = [&tags](const RoadMap& m) {
        for (const std::string& t : tags)
            if (m.tags.count(Upper(t))) return true;
        return false;
    };
    if (g_lastMap >= 0 && matches(g_maps[g_lastMap])) return g_lastMap;
    for (size_t i = 0; i < g_maps.size(); ++i)
        if (matches(g_maps[i])) {
            g_lastMap = (int)i;
            LogLine(L"roads: current map " + Widen(g_maps[i].name.c_str()));
            return g_lastMap;
        }
    return -1;
}

}  // namespace

std::string RoadsMapName(const std::vector<std::string>& tags) {
    int i = FindMap(tags);
    return i >= 0 ? g_maps[i].name : std::string();
}

void RoadsLoad(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::string line;
    size_t nodes = 0, links = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[0] == '#') continue;
        if (line.compare(0, 4, "map ") == 0) {
            g_maps.push_back({});
            g_maps.back().name = line.substr(4);
            continue;
        }
        if (g_maps.empty()) continue;
        RoadMap& m = g_maps.back();
        std::istringstream in(line.substr(2));
        if (line.compare(0, 5, "tags ") == 0) {
            std::istringstream tl(line.substr(5));
            std::string t;
            while (tl >> t) m.tags.insert(Upper(t));
        } else if (line[0] == 'n' && line[1] == ' ') {
            RoadPoint p{};
            if (in >> p.x >> p.y) m.nodes.push_back(p), ++nodes;
        } else if (line[0] == 'e' && line[1] == ' ') {
            int a = -1, b = -1;
            if (in >> a >> b && a >= 0 && b >= 0 && a < (int)m.nodes.size() && b < (int)m.nodes.size())
                m.links.push_back({a, b}), ++links;
        }
    }
    LogLine(L"roads: " + path + L": " + std::to_wstring(g_maps.size()) + L" maps, " + std::to_wstring(nodes) +
            L" points, " + std::to_wstring(links) + L" links");
}

bool RoadsPlan(const std::vector<std::string>& tags, float ux, float uy, float tx, float ty, float maxX,
               float maxY, bool allowDirect, std::vector<RoadPoint>& out) {
    out.clear();
    int mi = FindMap(tags);
    if (mi < 0) return false;
    RoadMap& m = g_maps[mi];
    if (m.adj.empty()) Build(m);
    const int n = (int)m.nodes.size(), S = n, G = n + 1;
    // Road points beyond the active border are in another part of the story (the border
    // starts at the map origin: TerrainLogic::setActiveBoundary, NOTEBOOK.md).
    auto inside = [&](int i) { return maxX <= 0 || (m.nodes[i].x <= maxX + 50.f && m.nodes[i].y <= maxY + 50.f); };
    std::vector<float> cost(n + 2, 1e30f);
    std::vector<int> prev(n + 2, -1);
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    auto relax = [&](int to, int from, float c) {
        if (c < cost[to]) cost[to] = c, prev[to] = from, open.push({c, to});
    };
    const float straight = std::hypot(tx - ux, ty - uy);
    cost[S] = 0;
    for (int i = 0; i < n; ++i) {
        float d = Dist(m.nodes[i], ux, uy);
        if (d < kAccess && inside(i)) relax(i, S, d * kAccessCost);
    }
    if (allowDirect) relax(G, S, straight * kDirectCost);
    while (!open.empty()) {
        auto [c, v] = open.top();
        open.pop();
        if (c > cost[v]) continue;
        if (v == G) break;
        float dg = Dist(m.nodes[v], tx, ty);
        if (dg < kAccess) relax(G, v, c + dg * kAccessCost);
        for (auto [w, lc] : m.adj[v])
            if (inside(w)) relax(w, v, c + lc);
    }
    wchar_t t[240];
    if (prev[G] < 0) {
        swprintf_s(t, L"roads: no way from (%.0f, %.0f) to (%.0f, %.0f) on %S", ux, uy, tx, ty, m.name.c_str());
        LogLine(t);
        return false;
    }
    std::vector<int> chain;
    for (int v = prev[G]; v != S && v >= 0; v = prev[v]) chain.push_back(v);
    std::reverse(chain.begin(), chain.end());
    float lx = ux, ly = uy;
    for (int v : chain) {
        const RoadPoint& p = m.nodes[v];
        if (Dist(p, lx, ly) < kMinSpacing || Dist(p, tx, ty) < kMinSpacing) continue;
        out.push_back(p);
        lx = p.x, ly = p.y;
    }
    swprintf_s(t, L"roads: way from (%.0f, %.0f) to (%.0f, %.0f): %zu road points of %zu, cost %.0f, straight %.0f%s",
               ux, uy, tx, ty, out.size(), chain.size(), cost[G], straight,
               allowDirect ? (chain.empty() ? L", straight line chosen" : L"") : L", roads only");
    LogLine(t);
    return true;
}
