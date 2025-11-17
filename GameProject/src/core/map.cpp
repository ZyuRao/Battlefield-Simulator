#include "map.hpp"
#include "FastNoiseLite.h"
//https://github.com/Auburn/FastNoiseLite


//Map


Map::Map(int w, int h)
    : width(w), height(h)
{
    tiles.resize(height, std::vector<Tile>(width));
}

bool Map::inBounds(const Coord& c) const {
    return c.x >= 0 && c.x < width &&
           c.y >= 0 && c.y < height;
}

const Tile& Map::getTile(const Coord& c) const {
    return tiles[c.y][c.x];
}

static void getNeighbors(const Map& map, const Coord& c, std::vector<Coord> & out) {
    out.clear();

    static const Coord disr4[4] = {
        {-1,  0},
        { 1,  0},
        { 0,  1},
        { 0, -1}
    };

    for(auto& d : disr4) {
        Coord n = c + d;
        if(map.inBounds(n)) out.push_back(n);

    }
}


bool Map::bfsReachable(const Coord& start, const Coord& goal) const {
    std::queue<Coord> q;
    std::vector<std::vector<bool>> visited(height,
        std::vector<bool>(width, false));
    std::vector<Coord> nbrs;
    q.push(start);
    visited[start.y][start.x] = true;

    while(!q.empty()) {
        Coord cur = q.front();
        q.pop();

        if(cur == goal) return true;

        getNeighbors(this, cur, nbrs);

        for(auto& n : nbrs) {
            if(!visited[n.y][n.x] && getTile(n).isPassable()) {
                visited[n.y][n.x] = true;
                q.push(n);
            }
        }
    }

    return false;

}

bool Map::isReachable(const Coord& start, const Coord& goal) const {
    if(!inBounds(start) || !inBounds(goal)) return false;
    if(!getTile(start).isPassable() || !getTile(goal).isPassable())
        return false;
        
    return bfsReachable(start, goal);
}



//Mapgenerator



MapGenerator::MapGenerator(int w, int h) : width(w), height(h) {}

Map MapGenerator::generate() {
    std::vector<std::vector<float>> heightMap(
        height,std::vector<float>(width)
    );

    generateHeightMap(heightMap);

    applyMountmainFormation(heightMap);


    Map map(width, height);
    std::vector<std::vector<bool>> riverMask(
        height, std::vector<bool>(width, false));

    carveRivers(map, heightMap, riverMask);

    
    generateTiles(map, heightMap, riverMask);

    applySwampArdRivers(map);

    return map;
}

void MapGenerator::generateHeightMap(std::vector<std::vector<float>> &h) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, 10000000);
    int seed = dist(gen);

    FastNoiseLite noise;
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(0.005f);
    noise.SetSeed(seed);

    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x){
            float n = noise.GetNoise(x, y);
            n = n * 0.5f + 0.5f;
            h[y][x] = n;
        }
    }
}

void MapGenerator::applyMountmainFormation(std::vector<std::vector<float>>& h) {
    for(int y = 0; y < height; ++y){
        for(int x = 0; x < width; ++x){
            float v = h[y][x];
            if(v > 0.75f){
                h[y][x] = std::min(1.0f, v * 1.15f);
            }
        }
    }
}


void MapGenerator::carveSingleRiver(const Map& map, std::vector<std::vector<float>>& h,
                                    const Coord& origin)
{
    Coord cur = origin;
    for(int steps = 0; steps < 2 * width; steps++) {
        h[cur.y][cur.x] = 0.0f;

        std::vector<Coord> nbrs;
        Map::getNeighbors(map, cur, nbrs);

        float bestH = 1.0f;
        Coord best = cur;

        for(auto n : nbrs){
            if (n.x < 0 || n.x >= width ||
                n.y < 0 || n.y >= height)
                continue;

            if (h[n.y][n.x] < bestH) {
                bestH = h[n.y][n.x];
                best = n;
            }
        }

        if(best == cur) break;

        cur = best;
    }
}

void MapGenerator::carveRivers(const Map& map, std::vector<std::vector<float>>& h) {
    std::random_device rd;
    std::mt19937 gen(rd());

    int riverCount = 2 + ((gen()) % 3);

    for(int i = 0; i < riverCount; i++){
        carveSingleRiver(map, h, Coord(gen() % width, gen() % height));
    }

}




