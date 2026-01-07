#pragma once
#include <vector>
#include <random>
#include <queue>
#include <algorithm>
#include <random>
#include <cstdint>
#include "tile.hpp"
#include "utils/vec2.hpp"

class Map {
private:
    int width;
    int height;
    std::vector<std::vector<Tile>> tiles;

    bool bfsReachable(const Coord& start, const Coord& goal) const;

public:
    Map(int w, int h);

    int getWidth() const { return width; }
    int getHeight() const { return height; }

    bool inBounds(const Coord& c) const;
    Tile& getTile(const Coord& c);
    const Tile& getTile(const Coord& c) const ;

    void getNeighbors(const Coord& c, std::vector<Coord>& out) const ;
    static void getNeighbors(const Map& map, const Coord& c, std::vector<Coord>& out);
    static void getNeighbors8(const Map& map, const Coord& c, std::vector<Coord>& out);


    bool isReachable(const Coord& start, const Coord& goal) const;
    void findPathAStar(const Coord& start,
                   const Coord& goal,
                   std::vector<Coord>& outPath) const;
    bool hasMountainBetween(const Coord& start, const Coord& goal) const;
    bool hasRiverBetween(const Coord& start, const Coord& goal) const;

    // void print() const;

};

struct UnitStats;

struct MapQuery {
    struct ReachableGrid {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> reachable;

        bool isReachable(const Coord& c) const {
            if (c.x < 0 || c.x >= width || c.y < 0 || c.y >= height) return false;
            std::size_t idx = static_cast<std::size_t>(c.y) * width + c.x;
            return reachable[idx] != 0;
        }
    };

    static Coord clampToMap(const Map& map, Coord c);
    static Coord nearestPassable(const Map& map, Coord c, int radius);
    static ReachableGrid buildReachableGrid(const Map& map, const Coord& start);
    static Coord nearestReachableToTarget(const Map& map,
                                          const ReachableGrid& reachable,
                                          const Coord& target);
    static bool inAttackRange(const Map& map,
                              const Coord& selfPos,
                              const UnitStats& stats,
                              const Coord& targetPos);
    static bool inAttackRangeFrom(const Map& map,
                                  const Coord& from,
                                  const UnitStats& stats,
                                  const Coord& targetPos);
    static bool hasMountainBetween(const Map& map,
                                   const Coord& start,
                                   const Coord& goal);
    static bool hasRiverBetween(const Map& map,
                                const Coord& start,
                                const Coord& goal);

private:
    static bool lineHasType(const Map& map,
                            const Coord& start,
                            const Coord& goal,
                            TileType type);
};

struct PathPlanner {
    static bool findPathAStarHeat(const Map& map,
                                  const Coord& start,
                                  const Coord& goal,
                                  const std::vector<float>& heat,
                                  int unitId,
                                  std::vector<Coord>& outpath);
    static void rebuildPathStateHeat(const Map& map,
                                     const Coord& start,
                                     const Coord& target,
                                     const std::vector<float>& heat,
                                     int unitId,
                                     std::vector<Coord>& path,
                                     std::size_t& idx,
                                     float& accumulator,
                                     Coord& lastTarget,
                                     bool& hasLast);
    static void rebuildPathState(const Map& map,
                                 const Coord& start,
                                 const Coord& target,
                                 std::vector<Coord>& path,
                                 std::size_t& idx,
                                 float& accumulator,
                                 Coord& lastTarget,
                                 bool& hasLast);
};

class MapGenerator {
private:
    int width,height;
public:
    MapGenerator(int w, int h);

    Map generate();

private:
    void generateHeightMap(std::vector<std::vector<float>>& heightMap);
    void applyMountmainFormation(std::vector<std::vector<float>>& heightMap);
    void carveRivers(const Map& map, std::vector<std::vector<float>>& heightMap, std::vector<std::vector<bool>>& riverMask);

    TileType classifyHeight(float h) const;
    void applySwampArdRivers(Map& map, const std::vector<std::vector<bool>>& riverMask);


    void generateTiles(Map &map,const std::vector<std::vector<float>>& heightMap, const std::vector<std::vector<bool>>& riverMask);


    bool validateMap(const Map& map, const Coord& baseA, const Coord& baseB) const ;

    void carveSingleRiver(const Map& map, std::vector<std::vector<float>>& heightMap, 
                        std::vector<std::vector<bool>>& riverMask, const Coord& origin);

};
