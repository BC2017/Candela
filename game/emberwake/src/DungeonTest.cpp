// Emberwake dungeon self-test: generates hundreds of seeded dungeons and
// proves the invariants gameplay depends on. Pure logic, no engine or GPU.
// Exit code 0 = pass.
#include "Dungeon.h"

#include <cstdio>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s (seed %llu)\n", __FILE__, __LINE__,    \
                        #cond, static_cast<unsigned long long>(seed));         \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

} // namespace

int main() {
    constexpr int kWidth = 44;
    constexpr int kHeight = 34;
    constexpr int kRooms = 9;
    constexpr int kSeeds = 400;

    int totalRooms = 0;
    int minRooms = 1 << 20;
    for (uint64_t seed = 1; seed <= kSeeds; ++seed) {
        const emberwake::Dungeon dungeon =
            emberwake::Dungeon::generate(seed, kWidth, kHeight, kRooms);

        // --- Shape ---
        CHECK(dungeon.rectangular());
        CHECK(dungeon.width() == kWidth && dungeon.height() == kHeight);
        // The border must stay solid (generation never carves it).
        for (int x = 0; x < kWidth; ++x) {
            CHECK(dungeon.solid(x, 0) && dungeon.solid(x, kHeight - 1));
        }
        for (int z = 0; z < kHeight; ++z) {
            CHECK(dungeon.solid(0, z) && dungeon.solid(kWidth - 1, z));
        }

        // --- Rooms ---
        const int roomCount = static_cast<int>(dungeon.rooms().size());
        CHECK(roomCount >= 5); // enough space for gameplay on every seed
        totalRooms += roomCount;
        minRooms = roomCount < minRooms ? roomCount : minRooms;
        for (const emberwake::Room& room : dungeon.rooms()) {
            for (int z = room.z; z < room.z + room.h; ++z) {
                for (int x = room.x; x < room.x + room.w; ++x) {
                    CHECK(!dungeon.solid(x, z));
                }
            }
        }

        // --- Spawn and stairs ---
        CHECK(!dungeon.solid(dungeon.spawn().x, dungeon.spawn().y));
        CHECK(!dungeon.solid(dungeon.stairs().x, dungeon.stairs().y));
        CHECK(dungeon.spawn() != dungeon.stairs());
        CHECK(!dungeon
                   .pathBetween(dungeon.spawn(), dungeon.stairs())
                   .empty());

        // --- Connectivity: every open cell reachable from spawn ---
        CHECK(dungeon.fullyConnected());

        // --- Determinism: same seed, same dungeon ---
        const emberwake::Dungeon again =
            emberwake::Dungeon::generate(seed, kWidth, kHeight, kRooms);
        CHECK(again.rows() == dungeon.rows());
        CHECK(again.spawn() == dungeon.spawn() &&
              again.stairs() == dungeon.stairs());

        // --- Placement scatter: valid open cells, far from spawn, unique ---
        emberwake::Rng rng(seed ^ 0xABCDEF);
        const auto cells = dungeon.scatter(rng, 10, 6);
        CHECK(static_cast<int>(cells.size()) >= 6);
        for (size_t i = 0; i < cells.size(); ++i) {
            CHECK(!dungeon.solid(cells[i].x, cells[i].y));
            CHECK(cells[i] != dungeon.spawn() && cells[i] != dungeon.stairs());
            CHECK(!dungeon.rooms().front().contains(cells[i]));
            for (size_t j = i + 1; j < cells.size(); ++j) {
                CHECK(cells[i] != cells[j]);
            }
        }

        if (g_failures > 0) {
            break; // one broken seed prints enough
        }
    }

    if (g_failures == 0) {
        std::printf("emberwake-dungeontest: %d seeds passed (rooms: min %d, "
                    "avg %.1f)\n",
                    kSeeds, minRooms,
                    static_cast<double>(totalRooms) / kSeeds);
        return 0;
    }
    std::printf("emberwake-dungeontest: %d FAILURES\n", g_failures);
    return 1;
}
