#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// Shared cell-grid world logic for the games: solidity, circle-vs-grid
// collision, flood fill, and BFS pathfinding over a '#'-wall / '.'-floor
// row representation. Pure logic — no engine or GPU dependencies, so the
// per-game logic tests run anywhere.
namespace gamegrid {

class GridMap {
public:
    GridMap() = default;
    explicit GridMap(std::vector<std::string> rows) { setRows(std::move(rows)); }

    int width() const { return m_width; }
    int height() const { return m_height; }
    const std::vector<std::string>& rows() const { return m_rows; }

    bool rectangular() const {
        for (const std::string& row : m_rows) {
            if (static_cast<int>(row.size()) != m_width) {
                return false;
            }
        }
        return true;
    }

    // Out of bounds counts as solid, so collision needs no boundary checks.
    bool solid(int x, int z) const {
        if (x < 0 || z < 0 || x >= m_width || z >= m_height) {
            return true;
        }
        return m_rows[static_cast<size_t>(z)][static_cast<size_t>(x)] == '#';
    }

    // XZ world position of a cell's centre (one cell = one metre).
    static glm::vec2 cellCenter(glm::ivec2 cell) {
        return {static_cast<float>(cell.x) + 0.5f,
                static_cast<float>(cell.y) + 0.5f};
    }

    static glm::ivec2 cellOf(glm::vec2 pos) {
        return {static_cast<int>(std::floor(pos.x)),
                static_cast<int>(std::floor(pos.y))};
    }

    // Pushes a circle (XZ position, radius) out of every solid cell it
    // overlaps. Iterates so corner pushes settle against both walls.
    glm::vec2 resolveCollision(glm::vec2 pos, float radius) const {
        for (int iteration = 0; iteration < 3; ++iteration) {
            const int cx = static_cast<int>(std::floor(pos.x));
            const int cz = static_cast<int>(std::floor(pos.y));
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = cx + dx;
                    const int z = cz + dz;
                    if (!solid(x, z)) {
                        continue;
                    }
                    const glm::vec2 lo{static_cast<float>(x),
                                       static_cast<float>(z)};
                    const glm::vec2 hi = lo + 1.0f;
                    const glm::vec2 closest = glm::clamp(pos, lo, hi);
                    const glm::vec2 delta = pos - closest;
                    const float dist2 = glm::dot(delta, delta);
                    if (dist2 >= radius * radius) {
                        continue;
                    }
                    if (dist2 > 1e-8f) {
                        pos = closest + delta * (radius / std::sqrt(dist2));
                    } else {
                        // Centre inside the cell (degenerate, e.g. exactly on
                        // a wall corner). Eject along whichever axis exits
                        // into open space — a blind dominant-axis push can
                        // land inside the diagonal neighbour and oscillate
                        // there forever. Shortest open push wins; if fully
                        // boxed in, shortest push of all.
                        const glm::vec2 ejects[4] = {
                            {lo.x - radius, pos.y},
                            {hi.x + radius, pos.y},
                            {pos.x, lo.y - radius},
                            {pos.x, hi.y + radius}};
                        const bool open[4] = {
                            !solid(x - 1, z), !solid(x + 1, z),
                            !solid(x, z - 1), !solid(x, z + 1)};
                        auto cost = [&pos, &ejects](int i) {
                            const glm::vec2 d = ejects[i] - pos;
                            return glm::dot(d, d);
                        };
                        int best = -1;
                        for (int i = 0; i < 4; ++i) {
                            if (open[i] &&
                                (best < 0 || cost(i) < cost(best))) {
                                best = i;
                            }
                        }
                        if (best < 0) {
                            best = 0;
                            for (int i = 1; i < 4; ++i) {
                                if (cost(i) < cost(best)) {
                                    best = i;
                                }
                            }
                        }
                        pos = ejects[best];
                    }
                }
            }
        }
        return pos;
    }

    // Cells reachable from `start` through open cells (4-connected), as a
    // width*height mask.
    std::vector<uint8_t> reachableFrom(glm::ivec2 start) const {
        std::vector<uint8_t> visited(
            static_cast<size_t>(m_width * m_height), 0);
        if (solid(start.x, start.y)) {
            return visited;
        }
        visited[index(start)] = 1;
        std::deque<glm::ivec2> frontier{start};
        while (!frontier.empty()) {
            const glm::ivec2 cell = frontier.front();
            frontier.pop_front();
            const glm::ivec2 neighbors[4] = {{cell.x + 1, cell.y},
                                             {cell.x - 1, cell.y},
                                             {cell.x, cell.y + 1},
                                             {cell.x, cell.y - 1}};
            for (const glm::ivec2 next : neighbors) {
                if (solid(next.x, next.y) || visited[index(next)] != 0) {
                    continue;
                }
                visited[index(next)] = 1;
                frontier.push_back(next);
            }
        }
        return visited;
    }

    // Shortest open-cell path (4-connected BFS) between two cells, both
    // endpoints included. Empty when unreachable.
    std::vector<glm::ivec2> pathBetween(glm::ivec2 from, glm::ivec2 to) const {
        if (solid(from.x, from.y) || solid(to.x, to.y)) {
            return {};
        }
        std::vector<int> cameFrom(
            static_cast<size_t>(m_width * m_height), -1);
        cameFrom[index(from)] = static_cast<int>(index(from));
        std::deque<glm::ivec2> frontier{from};
        while (!frontier.empty() && cameFrom[index(to)] < 0) {
            const glm::ivec2 cell = frontier.front();
            frontier.pop_front();
            const glm::ivec2 neighbors[4] = {{cell.x + 1, cell.y},
                                             {cell.x - 1, cell.y},
                                             {cell.x, cell.y + 1},
                                             {cell.x, cell.y - 1}};
            for (const glm::ivec2 next : neighbors) {
                if (solid(next.x, next.y) || cameFrom[index(next)] >= 0) {
                    continue;
                }
                cameFrom[index(next)] = static_cast<int>(index(cell));
                frontier.push_back(next);
            }
        }
        if (cameFrom[index(to)] < 0) {
            return {};
        }
        std::vector<glm::ivec2> path;
        for (int at = static_cast<int>(index(to));
             at != cameFrom[static_cast<size_t>(at)];
             at = cameFrom[static_cast<size_t>(at)]) {
            path.push_back({at % m_width, at / m_width});
        }
        path.push_back(from);
        std::reverse(path.begin(), path.end());
        return path;
    }

    // True when the straight segment between two XZ points crosses no solid
    // cell (2D DDA) — line-of-sight for AI and aimed shots.
    bool lineClear(glm::vec2 from, glm::vec2 to) const {
        glm::ivec2 cell = cellOf(from);
        const glm::ivec2 goal = cellOf(to);
        if (solid(cell.x, cell.y) || solid(goal.x, goal.y)) {
            return false;
        }
        const glm::vec2 delta = to - from;
        const glm::ivec2 step{delta.x > 0.0f ? 1 : -1,
                              delta.y > 0.0f ? 1 : -1};
        // Parametric distance to the next cell boundary on each axis.
        auto boundaryT = [](float origin, float d, int cellCoord, int s) {
            if (std::abs(d) < 1e-9f) {
                return 1e30f;
            }
            const float edge =
                static_cast<float>(cellCoord) + (s > 0 ? 1.0f : 0.0f);
            return (edge - origin) / d;
        };
        float tx = boundaryT(from.x, delta.x, cell.x, step.x);
        float tz = boundaryT(from.y, delta.y, cell.y, step.y);
        const float txStep = std::abs(delta.x) < 1e-9f
                                 ? 1e30f
                                 : 1.0f / std::abs(delta.x);
        const float tzStep = std::abs(delta.y) < 1e-9f
                                 ? 1e30f
                                 : 1.0f / std::abs(delta.y);
        while (cell != goal) {
            if (tx < tz) {
                cell.x += step.x;
                tx += txStep;
            } else {
                cell.y += step.y;
                tz += tzStep;
            }
            if (solid(cell.x, cell.y)) {
                return false;
            }
        }
        return true;
    }

    // Like lineClear, but for a moving circle: true when a disc of `radius`
    // can travel the whole segment without touching a solid cell. Grid LOS
    // is zero-width — a projectile that hugs a doorway corner needs this.
    bool corridorClear(glm::vec2 from, glm::vec2 to, float radius) const {
        const float length = glm::distance(from, to);
        const int steps = std::max(1, static_cast<int>(length / 0.15f));
        for (int i = 0; i <= steps; ++i) {
            const glm::vec2 point =
                glm::mix(from, to,
                         static_cast<float>(i) / static_cast<float>(steps));
            const glm::vec2 resolved = resolveCollision(point, radius);
            if (glm::distance(resolved, point) > 1e-4f) {
                return false;
            }
        }
        return true;
    }

protected:
    void setRows(std::vector<std::string> rows) {
        m_rows = std::move(rows);
        m_height = static_cast<int>(m_rows.size());
        m_width = m_height > 0 ? static_cast<int>(m_rows[0].size()) : 0;
    }

    size_t index(glm::ivec2 cell) const {
        return static_cast<size_t>(cell.y * m_width + cell.x);
    }

    std::vector<std::string> m_rows;
    int m_width = 0;
    int m_height = 0;
};

} // namespace gamegrid
