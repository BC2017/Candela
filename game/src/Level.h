#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

// Pure game logic — no engine or GPU dependencies, so lightkeeper-leveltest
// can exercise it anywhere (including headless CI machines).
namespace lightkeeper {

// The shrine. '#' wall, '.' open floor, 'P' player start, 'c' candle.
// One cell = one metre. Edit freely — the level self-test proves every
// candle stays reachable from the start.
inline const std::vector<std::string>& defaultMaze() {
    static const std::vector<std::string> rows = {
        "#####################",
        "#P..#.....#...#....c#",
        "#.#.#.###.#.#.#.###.#",
        "#.#...#c..#.#...#...#",
        "#.#####.###.#####.###",
        "#.....#.#...#c....#c#",
        "#####.#.#.###.#####.#",
        "#c..#.#.#.....#.....#",
        "#.#.#.#.#####.#.#####",
        "#.#...#.....#...#..c#",
        "#.#########.#.#.#.#.#",
        "#c........#...#...#.#",
        "#####################",
    };
    return rows;
}

class Level {
public:
    static Level parse(const std::vector<std::string>& rows) {
        Level level;
        level.m_rows = rows;
        level.m_height = static_cast<int>(rows.size());
        level.m_width =
            level.m_height > 0 ? static_cast<int>(rows[0].size()) : 0;
        for (int z = 0; z < level.m_height; ++z) {
            for (int x = 0; x < level.m_width; ++x) {
                const char cell = rows[static_cast<size_t>(z)]
                                      [static_cast<size_t>(x)];
                if (cell == 'P') {
                    level.m_start = {x, z};
                } else if (cell == 'c') {
                    level.m_candles.push_back({x, z});
                }
            }
        }
        return level;
    }

    int width() const { return m_width; }
    int height() const { return m_height; }
    glm::ivec2 playerStart() const { return m_start; }
    const std::vector<glm::ivec2>& candles() const { return m_candles; }

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

    // XZ world position of a cell's centre.
    static glm::vec2 cellCenter(glm::ivec2 cell) {
        return {static_cast<float>(cell.x) + 0.5f,
                static_cast<float>(cell.y) + 0.5f};
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
                        // Centre ended up inside the cell (degenerate, e.g.
                        // exactly on a wall corner). Eject along whichever
                        // axis exits into open space — a blind dominant-axis
                        // push can land inside the diagonal neighbour and
                        // oscillate there forever. Shortest open push wins;
                        // if fully boxed in, shortest push of all.
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

    // Shortest open-cell path (4-connected BFS) between two cells, both
    // endpoints included. Empty when unreachable. Drives the autoplay
    // attract mode and the headless gameplay test.
    std::vector<glm::ivec2> pathBetween(glm::ivec2 from, glm::ivec2 to) const {
        if (solid(from.x, from.y) || solid(to.x, to.y)) {
            return {};
        }
        const int cellCount = m_width * m_height;
        std::vector<int> cameFrom(static_cast<size_t>(cellCount), -1);
        auto index = [this](glm::ivec2 cell) {
            return cell.y * m_width + cell.x;
        };
        cameFrom[static_cast<size_t>(index(from))] = index(from);
        std::deque<glm::ivec2> frontier{from};
        while (!frontier.empty() &&
               cameFrom[static_cast<size_t>(index(to))] < 0) {
            const glm::ivec2 cell = frontier.front();
            frontier.pop_front();
            const glm::ivec2 neighbors[4] = {{cell.x + 1, cell.y},
                                             {cell.x - 1, cell.y},
                                             {cell.x, cell.y + 1},
                                             {cell.x, cell.y - 1}};
            for (const glm::ivec2 next : neighbors) {
                if (solid(next.x, next.y) ||
                    cameFrom[static_cast<size_t>(index(next))] >= 0) {
                    continue;
                }
                cameFrom[static_cast<size_t>(index(next))] = index(cell);
                frontier.push_back(next);
            }
        }
        if (cameFrom[static_cast<size_t>(index(to))] < 0) {
            return {};
        }
        std::vector<glm::ivec2> path;
        for (int at = index(to); at != cameFrom[static_cast<size_t>(at)];
             at = cameFrom[static_cast<size_t>(at)]) {
            path.push_back({at % m_width, at / m_width});
        }
        path.push_back(from);
        std::reverse(path.begin(), path.end());
        return path;
    }

    // Flood fill from the player start through open cells (4-connected).
    bool allCandlesReachable() const {
        std::vector<uint8_t> visited(
            static_cast<size_t>(m_width * m_height), 0);
        auto index = [this](glm::ivec2 cell) {
            return static_cast<size_t>(cell.y * m_width + cell.x);
        };
        std::deque<glm::ivec2> frontier{m_start};
        visited[index(m_start)] = 1;
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
        for (const glm::ivec2 candle : m_candles) {
            if (visited[index(candle)] == 0) {
                return false;
            }
        }
        return true;
    }

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<std::string> m_rows;
    glm::ivec2 m_start{0};
    std::vector<glm::ivec2> m_candles;
};

} // namespace lightkeeper
