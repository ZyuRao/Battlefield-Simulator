#include "core/map.hpp"
#include "core/FastNoiseLite.h"
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

Tile& Map::getTile(const Coord& c) {
    return tiles[c.y][c.x];
}

const Tile& Map::getTile(const Coord& c) const {
    return tiles[c.y][c.x];
}


void Map::getNeighbors(const Coord& c, std::vector<Coord>& out) const {
    out.clear();

    static const Coord disr4[4] = {
        {-1,  0},
        { 1,  0},
        { 0,  1},
        { 0, -1}
    };

    for(auto& d : disr4) {
        Coord n = c + d;
        if(inBounds(n)) out.push_back(n);

    }
}
void Map::getNeighbors(const Map& map, const Coord& c, std::vector<Coord> & out) {
    map.getNeighbors(c, out);
}

void Map::getNeighbors8(const Map& map,
                        const Coord& c,
                        std::vector<Coord>& out)
{
    out.clear();

    static const Coord dirs[8] = {
        {-1,  0}, {1,  0}, {0, -1}, {0, 1},
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };

    for (auto& d : dirs) {
        Coord n = c + d;
        if (map.inBounds(n))
            out.push_back(n);
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

        getNeighbors(cur, nbrs);

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

// void Map::print() const
// {
//     for (int y = 0; y < height; ++y)
//     {
//         for (int x = 0; x < width; ++x)
//         {
//             const Tile& t = tiles[y][x];
//             std::cout << t.getSymbol();
//         }
//         std::cout << "\n";
//     }
// }
//调试用

void Map::findPathAStar (const Coord& start,const Coord& goal, std::vector<Coord>& outpath) const
{
    outpath.clear();
    if (!inBounds(start) || !inBounds(goal)) return;
    if (!getTile(start).isPassable() || !getTile(goal).isPassable())
        return;
    struct Node{
       Coord pos;
       float g;
       float f;//f = g + h

       bool operator<(const Node& other) const {
            return f > other.f;
       }
    };

    std::priority_queue<Node> open;
    std::vector<std::vector<float>> gScore(
        height, std::vector<float>(width, 1e9f)
    );

    std::vector<std::vector<Coord>> cameFrom(
        height, std::vector<Coord>(width, Coord(-1, -1))
    );

    gScore[start.y][start.x] = 0.0f;
    open.push({start, 0.0f, (float)Coord::mhtDistance(start, goal)});
    std::vector<Coord> nbrs;

    while(!open.empty()){
        Node cur = open.top();
        open.pop();

        if(cur.pos == goal) {
            Coord p = goal;
            while(!(p == start)) {
                outpath.push_back(p);
                p = cameFrom[p.y][p.x];
            }
            outpath.push_back(start);
            std::reverse(outpath.begin(), outpath.end());
            return;
        }
        getNeighbors(cur.pos, nbrs);
        for(const auto& n : nbrs){
            const Tile& t = getTile(n);
            if(!t.isPassable()) continue;

            float tentativeG = cur.g + t.getMoveCost();

            if(tentativeG < gScore[n.y][n.x]) {
                gScore[n.y][n.x] = tentativeG;
                cameFrom[n.y][n.x] = cur.pos;
                float f = tentativeG + Coord::mhtDistance(n, goal);
                open.push({n, tentativeG, f});
            }
        }
    } 
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

    applySwampArdRivers(map, riverMask);

    return map;
}

void MapGenerator::generateHeightMap(std::vector<std::vector<float>> &h) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, 10000000);
    int seed = dist(gen);

    FastNoiseLite noise;
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(0.02f);
    noise.SetSeed(seed);

    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x){
            float n = noise.GetNoise((float)x, (float)y);
            n = n * 0.5f + 0.5f;
            h[y][x] = n;
        }
    }
}

void MapGenerator::applyMountmainFormation(std::vector<std::vector<float>>& h) {
    for(int y = 0; y < height; ++y){
        for(int x = 0; x < width; ++x){
            float v = h[y][x];
            if(v > 0.9f){
                h[y][x] = std::min(1.0f, v * 1.15f);
            }
        }
    }
}


void MapGenerator::carveSingleRiver(const Map& map, std::vector<std::vector<float>>& h,
     std::vector<std::vector<bool>>& riverMask, const Coord& origin)
{
    Coord cur = origin;
    std::vector<Coord> nbrs;
    for(int steps = 0; steps < 2 * width; steps++) {
        
        riverMask[cur.y][cur.x] = true;
        Map::getNeighbors(map, cur, nbrs);

        float bestH = h[cur.y][cur.x];
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

void MapGenerator::carveRivers(const Map& map, std::vector<std::vector<float>>& h, 
    std::vector<std::vector<bool>>& riverMask) {
    std::random_device rd;
    std::mt19937 gen(rd());

    int riverCount = 2 + ((gen()) % 3);

    for(int i = 0; i < riverCount; i++){
        carveSingleRiver(map, h, riverMask, Coord(gen() % width, gen() % height));
    }

}

TileType MapGenerator::classifyHeight(float h) const {
    if(h < 0.40f) return TileType::PLAIN;
    else if (h < 0.60f) return TileType::FOREST;
    else if (h < 0.90f) return TileType::HILL;
    else return TileType::MOUNTAIN;
}

void MapGenerator::generateTiles(Map& map, const std::vector<std::vector<float>>& h, 
        const std::vector<std::vector<bool>>& riverMask){
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            Coord c{x, y};

            if (riverMask[y][x])
                map.getTile(c).setType(TileType::RIVER);
            else
                map.getTile(c).setType(classifyHeight(h[y][x]));
        }     
}
void MapGenerator::applySwampArdRivers(
    Map& map, const std::vector<std::vector<bool>>& riverMask
) {
    std::vector<Coord> nbrs;

    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x){
            if(!riverMask[y][x]) continue;

             Coord c(x, y);

             Map::getNeighbors8(map, c, nbrs);

             for(auto& n : nbrs) {
                auto t = map.getTile(n).getType();
                if(t == TileType::PLAIN || t == TileType::FOREST){
                    map.getTile(n).setType(TileType::SWAMP);
                }
             }
        }
}

bool MapGenerator::validateMap(const Map& map, const Coord& baseA, const Coord& baseB) const {
    return map.isReachable(baseA, baseB);
}




