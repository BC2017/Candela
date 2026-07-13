#pragma once

#include "common/Grid.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Emberwake's procedural dungeon: seeded rooms-and-corridors generation on a
// cell grid. Pure logic (shared gamegrid::GridMap mechanics) — the seed-sweep
// test proves connectivity and placement invariants across hundreds of seeds.
namespace emberwake {

// Deterministic splitmix64 — identical sequences on every platform/compiler,
// unlike <random> distributions.
struct Rng {
    uint64_t state;

    explicit Rng(uint64_t seed) : state(seed) {}

    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, bound).
    uint32_t range(uint32_t bound) {
        return static_cast<uint32_t>(next() % bound);
    }

    int rangeInt(int lo, int hi) { // inclusive
        return lo + static_cast<int>(range(static_cast<uint32_t>(hi - lo + 1)));
    }

    float unit() {
        return static_cast<float>(next() >> 40) *
               (1.0f / 16777216.0f); // 24 mantissa bits
    }
};

struct Room {
    int x = 0;
    int z = 0;
    int w = 0;
    int h = 0;

    glm::ivec2 center() const { return {x + w / 2, z + h / 2}; }

    bool intersects(const Room& other, int border) const {
        return x - border < other.x + other.w && other.x - border < x + w &&
               z - border < other.z + other.h && other.z - border < z + h;
    }

    bool contains(glm::ivec2 cell) const {
        return cell.x >= x && cell.x < x + w && cell.y >= z && cell.y < z + h;
    }
};

class Dungeon : public gamegrid::GridMap {
public:
    // Rooms placed by rejection sampling, then chained with L-shaped
    // corridors (plus one extra loop connection when there are enough
    // rooms). Every generated dungeon is fully connected by construction;
    // the seed-sweep test proves it anyway.
    static Dungeon generate(uint64_t seed, int width, int height,
                            int targetRooms) {
        Dungeon dungeon;
        dungeon.m_seed = seed;
        std::vector<std::string> rows(
            static_cast<size_t>(height),
            std::string(static_cast<size_t>(width), '#'));
        Rng rng(seed * 0x51ED2701u + 0xC0FFEEu);

        // --- Rooms ---
        std::vector<Room> rooms;
        for (int attempt = 0;
             attempt < 300 && static_cast<int>(rooms.size()) < targetRooms;
             ++attempt) {
            Room room;
            room.w = rng.rangeInt(4, 8);
            room.h = rng.rangeInt(4, 8);
            room.x = rng.rangeInt(1, width - room.w - 2);
            room.z = rng.rangeInt(1, height - room.h - 2);
            bool overlaps = false;
            for (const Room& other : rooms) {
                if (room.intersects(other, 1)) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) {
                rooms.push_back(room);
            }
        }

        auto carve = [&rows](int x, int z) {
            rows[static_cast<size_t>(z)][static_cast<size_t>(x)] = '.';
        };
        for (const Room& room : rooms) {
            for (int z = room.z; z < room.z + room.h; ++z) {
                for (int x = room.x; x < room.x + room.w; ++x) {
                    carve(x, z);
                }
            }
        }

        // --- Corridors: chain rooms in placement order, one extra loop ---
        auto corridor = [&](glm::ivec2 from, glm::ivec2 to, bool xFirst) {
            glm::ivec2 at = from;
            carve(at.x, at.y);
            auto stepX = [&](int target) {
                while (at.x != target) {
                    at.x += target > at.x ? 1 : -1;
                    carve(at.x, at.y);
                }
            };
            auto stepZ = [&](int target) {
                while (at.y != target) {
                    at.y += target > at.y ? 1 : -1;
                    carve(at.x, at.y);
                }
            };
            if (xFirst) {
                stepX(to.x);
                stepZ(to.y);
            } else {
                stepZ(to.y);
                stepX(to.x);
            }
        };
        for (size_t i = 1; i < rooms.size(); ++i) {
            corridor(rooms[i - 1].center(), rooms[i].center(),
                     rng.range(2) == 0);
        }
        if (rooms.size() >= 4) {
            corridor(rooms.front().center(),
                     rooms[rooms.size() / 2].center(), rng.range(2) == 0);
        }

        dungeon.setRows(std::move(rows));
        dungeon.m_rooms = std::move(rooms);

        // --- Spawn: first room; stairs: BFS-farthest room centre ---
        if (!dungeon.m_rooms.empty()) {
            dungeon.m_spawn = dungeon.m_rooms.front().center();
            dungeon.m_stairs = dungeon.farthestRoomCenter(dungeon.m_spawn);
        }
        return dungeon;
    }

    uint64_t seed() const { return m_seed; }
    const std::vector<Room>& rooms() const { return m_rooms; }
    glm::ivec2 spawn() const { return m_spawn; }
    glm::ivec2 stairs() const { return m_stairs; }

    bool fullyConnected() const {
        const std::vector<uint8_t> visited = reachableFrom(m_spawn);
        for (int z = 0; z < height(); ++z) {
            for (int x = 0; x < width(); ++x) {
                if (!solid(x, z) && visited[index({x, z})] == 0) {
                    return false;
                }
            }
        }
        return true;
    }

    // Deterministically scatters `count` open cells across rooms (never the
    // spawn room, never on the spawn/stairs cells, min BFS distance from
    // spawn, no duplicates). Used for enemies and pickups.
    std::vector<glm::ivec2> scatter(Rng& rng, int count,
                                    int minSpawnDistance) const {
        std::vector<glm::ivec2> cells;
        if (m_rooms.size() < 2) {
            return cells;
        }
        const std::vector<int> distance = bfsDistances(m_spawn);
        for (int attempt = 0;
             attempt < count * 40 &&
             static_cast<int>(cells.size()) < count;
             ++attempt) {
            const Room& room =
                m_rooms[1 + rng.range(static_cast<uint32_t>(
                            m_rooms.size() - 1))];
            const glm::ivec2 cell{room.x + rng.rangeInt(0, room.w - 1),
                                  room.z + rng.rangeInt(0, room.h - 1)};
            if (solid(cell.x, cell.y) || cell == m_spawn ||
                cell == m_stairs) {
                continue;
            }
            const int dist = distance[index(cell)];
            if (dist < 0 || dist < minSpawnDistance) {
                continue;
            }
            bool duplicate = false;
            for (const glm::ivec2 existing : cells) {
                if (existing == cell) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                cells.push_back(cell);
            }
        }
        return cells;
    }

private:
    // BFS distance (in cells) from `start` to every open cell; -1 unreachable.
    std::vector<int> bfsDistances(glm::ivec2 start) const {
        std::vector<int> distance(
            static_cast<size_t>(width() * height()), -1);
        if (solid(start.x, start.y)) {
            return distance;
        }
        distance[index(start)] = 0;
        std::deque<glm::ivec2> frontier{start};
        while (!frontier.empty()) {
            const glm::ivec2 cell = frontier.front();
            frontier.pop_front();
            const glm::ivec2 neighbors[4] = {{cell.x + 1, cell.y},
                                             {cell.x - 1, cell.y},
                                             {cell.x, cell.y + 1},
                                             {cell.x, cell.y - 1}};
            for (const glm::ivec2 next : neighbors) {
                if (solid(next.x, next.y) || distance[index(next)] >= 0) {
                    continue;
                }
                distance[index(next)] = distance[index(cell)] + 1;
                frontier.push_back(next);
            }
        }
        return distance;
    }

    glm::ivec2 farthestRoomCenter(glm::ivec2 from) const {
        const std::vector<int> distance = bfsDistances(from);
        glm::ivec2 best = from;
        int bestDistance = -1;
        for (const Room& room : m_rooms) {
            const glm::ivec2 center = room.center();
            const int dist = distance[index(center)];
            if (dist > bestDistance) {
                bestDistance = dist;
                best = center;
            }
        }
        return best;
    }

    uint64_t m_seed = 0;
    std::vector<Room> m_rooms;
    glm::ivec2 m_spawn{0};
    glm::ivec2 m_stairs{0};
};

} // namespace emberwake
