#pragma once

#include "common/Grid.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// Pure game logic — no engine or GPU dependencies, so lightkeeper-leveltest
// can exercise it anywhere (including headless CI machines). The grid
// mechanics (collision, flood fill, pathfinding) live in gamegrid::GridMap,
// shared with Emberwake.
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

class Level : public gamegrid::GridMap {
public:
    static Level parse(const std::vector<std::string>& rows) {
        Level level;
        level.setRows(rows);
        for (int z = 0; z < level.height(); ++z) {
            for (int x = 0; x < level.width(); ++x) {
                const char cell = level.rows()[static_cast<size_t>(z)]
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

    glm::ivec2 playerStart() const { return m_start; }
    const std::vector<glm::ivec2>& candles() const { return m_candles; }

    // Flood fill from the player start through open cells (4-connected).
    bool allCandlesReachable() const {
        const std::vector<uint8_t> visited = reachableFrom(m_start);
        for (const glm::ivec2 candle : m_candles) {
            if (visited[index(candle)] == 0) {
                return false;
            }
        }
        return true;
    }

private:
    glm::ivec2 m_start{0};
    std::vector<glm::ivec2> m_candles;
};

} // namespace lightkeeper
