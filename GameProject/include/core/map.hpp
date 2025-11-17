#pragma once
#include <vector>
#include <random>
#include <queue>
#include <algorithm>
#include <random>
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
    void print() const;

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


    bool validateMap(const Map& map, const Coord& baseA, const Coord& baseB);

    void carveSingleRiver(const Map& map, std::vector<std::vector<float>>& heightMap, 
                        std::vector<std::vector<bool>>& riverMask, const Coord& origin);

};
