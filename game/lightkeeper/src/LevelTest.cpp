// Lightkeeper level self-test: pure logic, no engine or GPU, so it runs on
// any machine (build verification boxes included). Exit code 0 = pass.
#include "Level.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// True when the circle at pos overlaps no solid cell.
bool clearOfWalls(const lightkeeper::Level& level, glm::vec2 pos,
                  float radius) {
    const int cx = static_cast<int>(std::floor(pos.x));
    const int cz = static_cast<int>(std::floor(pos.y));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = cx + dx;
            const int z = cz + dz;
            if (!level.solid(x, z)) {
                continue;
            }
            const glm::vec2 lo{static_cast<float>(x), static_cast<float>(z)};
            const glm::vec2 closest = glm::clamp(pos, lo, lo + 1.0f);
            const glm::vec2 delta = pos - closest;
            if (glm::dot(delta, delta) < radius * radius - 1e-4f) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    const lightkeeper::Level level =
        lightkeeper::Level::parse(lightkeeper::defaultMaze());
    constexpr float kRadius = 0.28f;

    // --- Level shape ---
    CHECK(level.rectangular());
    CHECK(level.width() >= 5 && level.height() >= 5);
    for (int x = 0; x < level.width(); ++x) {
        CHECK(level.solid(x, 0) && level.solid(x, level.height() - 1));
    }
    for (int z = 0; z < level.height(); ++z) {
        CHECK(level.solid(0, z) && level.solid(level.width() - 1, z));
    }
    CHECK(!level.solid(level.playerStart().x, level.playerStart().y));
    CHECK(!level.candles().empty());
    for (const glm::ivec2 candle : level.candles()) {
        CHECK(!level.solid(candle.x, candle.y));
    }

    // --- Every candle must be reachable from the start ---
    CHECK(level.allCandlesReachable());

    // --- Pathfinding: a valid path exists from the start to every candle ---
    for (const glm::ivec2 candle : level.candles()) {
        const std::vector<glm::ivec2> path =
            level.pathBetween(level.playerStart(), candle);
        CHECK(!path.empty());
        if (path.empty()) {
            continue;
        }
        CHECK(path.front() == level.playerStart());
        CHECK(path.back() == candle);
        for (size_t i = 0; i < path.size(); ++i) {
            CHECK(!level.solid(path[i].x, path[i].y));
            if (i > 0) {
                const glm::ivec2 step = path[i] - path[i - 1];
                CHECK(std::abs(step.x) + std::abs(step.y) == 1);
            }
        }
    }
    // Unreachable pairs (wall to wall, or into a wall) return empty.
    CHECK(level.pathBetween(level.playerStart(), {0, 0}).empty());

    // --- Collision: free positions stay put ---
    const glm::vec2 startCenter =
        lightkeeper::Level::cellCenter(level.playerStart());
    const glm::vec2 resolved = level.resolveCollision(startCenter, kRadius);
    CHECK(glm::length(resolved - startCenter) < 1e-5f);

    // --- Collision: positions inside walls get pushed clear ---
    for (int z = 0; z < level.height(); ++z) {
        for (int x = 0; x < level.width(); ++x) {
            if (level.solid(x, z)) {
                continue;
            }
            // Try to stand right on each cell corner (worst case: wall
            // corners) and expect a wall-free resolved position nearby.
            for (int corner = 0; corner < 4; ++corner) {
                const glm::vec2 pos{
                    static_cast<float>(x + (corner & 1)),
                    static_cast<float>(z + (corner >> 1))};
                const glm::vec2 out = level.resolveCollision(pos, kRadius);
                CHECK(clearOfWalls(level, out, kRadius));
                CHECK(glm::length(out - pos) < 1.5f);
            }
        }
    }

    // --- Collision: a long slide along every wall never tunnels through ---
    glm::vec2 pos = startCenter;
    const glm::vec2 directions[] = {{1.0f, 0.0f},
                                    {0.0f, 1.0f},
                                    {-0.7f, 0.7f},
                                    {0.9f, -0.4f}};
    for (const glm::vec2 dir : directions) {
        pos = startCenter;
        for (int step = 0; step < 2000; ++step) {
            pos += dir * 0.02f; // 2 cm steps, far below the wall thickness
            pos = level.resolveCollision(pos, kRadius);
            CHECK(clearOfWalls(level, pos, kRadius));
            if (g_failures > 0) {
                break;
            }
        }
        if (g_failures > 0) {
            break;
        }
    }

    if (g_failures == 0) {
        std::printf("lightkeeper-leveltest: all checks passed (%d candles, "
                    "%dx%d cells)\n",
                    static_cast<int>(level.candles().size()), level.width(),
                    level.height());
        return 0;
    }
    std::printf("lightkeeper-leveltest: %d FAILURES\n", g_failures);
    return 1;
}
