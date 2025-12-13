#include "core/gameworld.hpp"
#include <iostream>
#include <string>
#include <optional>
#include <algorithm>


namespace {
    void clearScreen() {
        #ifdef _WIN32
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut == INVALID_HANDLE_VALUE) return;

            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;

            DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;
            DWORD count;
            COORD home = {0, 0};

            FillConsoleOutputCharacter(hOut, ' ', cellCount, home, &count);
            FillConsoleOutputAttribute(hOut, csbi.wAttributes, cellCount, home, &count);
            SetConsoleCursorPosition(hOut, home);
        #else
            std::cout << "\x1b[2J\x1b[H";
        #endif
    }
}

RenderSystem::RenderSystem() : fontLoaded(false) {}

void RenderSystem::ensureFontLoaded() {
    if(fontLoaded) return;
    if(hudFont.openFromFile("../assets/fonts/NotoSansMono-VariableFont_wdth,wght.ttf")) {
        fontLoaded = true;
    } else {
        fontLoaded = false;
    }
}

static void moveCursor(int row, int col) {
    std::cout << "\x1b[" << row << ";" << col << "H";
}

void RenderSystem::renderAscii(const GameWorld& world) {
    const int W = world.map.getWidth();
    const int H = world.map.getHeight();

    
    std::vector<std::string> buffer(H, std::string(W, ' '));

    // 1. 生成本帧 buffer
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto sym = world.map.getTile({x,y}).getSymbol();
            buffer[y][x] = sym.empty() ? ' ' : sym[0];
        }
    }

    if (world.baseA && !world.baseA->isDestroyed())
        buffer[world.baseA->getPos().y][world.baseA->getPos().x] = 'A';

    if (world.baseB && !world.baseB->isDestroyed())
        buffer[world.baseB->getPos().y][world.baseB->getPos().x] = 'B';

    for (auto& u : world.unitsA)
        if (u->isAlive()) {
            auto p = u->getPos();
            buffer[p.y][p.x] = u->getSymbol()[0];
        }

    for (auto& u : world.unitsB)
        if (u->isAlive()) {
            auto p = u->getPos();
            buffer[p.y][p.x] = u->getSymbol()[0];
        }

    // 2. 如果是第一次渲染/尺寸变化 → 简单粗暴清屏全画一次
    if (lastBuffer.size() != buffer.size()) {
        clearScreen();
        for (int y = 0; y < H; ++y) {
            std::cout << buffer[y] << "\n";
        }
        lastBuffer = buffer;
        std::cout.flush();
        return;
    }

    // 3. 之后只重画“变了的那几行”
    for (int y = 0; y < H; ++y) {
        if (buffer[y] != lastBuffer[y]) {
            moveCursor(y + 1, 1);      // 行号从 1 开始
            std::cout << buffer[y];
        }
    }

    lastBuffer = buffer;
    std::cout.flush();
}

RenderSystem::Layout RenderSystem::computeLayout(
    const GameWorld& world,
    const sf::RenderWindow& window
) const {
    const int W = world.map.getWidth();
    const int H = world.map.getHeight();

    sf::Vector2u winSize = window.getSize();

    const float hudWidth = 260.f; // 右侧 HUD 占的宽度
    float mapAreaWidth = std::max(100.f, static_cast<float>(winSize.x) - hudWidth);
    float mapAreaHeight = static_cast<float>(winSize.y);

    float tileSizeX = mapAreaWidth  / static_cast<float>(W);
    float tileSizeY = mapAreaHeight / static_cast<float>(H);
    float tileSize  = std::min(tileSizeX, tileSizeY);

    // 居中放在左侧那块区域
    float usedWidth  = tileSize * W;
    float usedHeight = tileSize * H;
    float offsetX = 0.5f * (mapAreaWidth  - usedWidth);
    float offsetY = 0.5f * (mapAreaHeight - usedHeight);

    Layout lay;
    lay.tileSize = tileSize;
    lay.offsetX  = offsetX;
    lay.offsetY  = offsetY;
    lay.hudX     = mapAreaWidth + 10.f; // HUD 从地图右侧往右偏一点

    return lay;
}

sf::Color RenderSystem::tileColor(TileType t) const {
    switch (t) {
    case TileType::PLAIN:    return sf::Color(90, 140, 90);
    case TileType::FOREST:   return sf::Color(30, 90, 30);
    case TileType::HILL:     return sf::Color(120, 120, 120);
    case TileType::SWAMP:    return sf::Color(60, 70, 40);
    case TileType::RIVER:    return sf::Color(40, 80, 160);
    case TileType::MOUNTAIN: return sf::Color(150, 150, 160);
    default:                 return sf::Color(100, 100, 100);
    }
}

sf::Color RenderSystem::factionColor(Faction f) const {
    switch (f) {
    case Faction::A: return sf::Color(220, 60, 60);   // 红
    case Faction::B: return sf::Color(60, 60, 220);   // 蓝
    }
    return sf::Color::White;
}

sf::Color RenderSystem::unitTypeColor(UnitType t) const {
    switch (t) {
    case UnitType::Infantry: return sf::Color(220, 220, 220); // 步兵
    case UnitType::Archer:   return sf::Color(60, 200, 80);   // 弓
    case UnitType::Knight:   return sf::Color(230, 200, 60);  // 骑
    }
    return sf::Color::Magenta;
}

void RenderSystem::drawHpBar(sf::RenderWindow& window,
                             sf::Vector2f center,
                             float width,
                             float hp,
                             float maxHp) const {
    if (maxHp <= 0.f) return;

    float ratio = std::clamp(hp / maxHp, 0.f, 1.f);

    float barHeight = width * 0.15f;
    float barWidth  = width;

    // --- 背景条 ---
    sf::RectangleShape back;
    back.setSize(sf::Vector2f{barWidth, barHeight}); // 必须显式 Vector2f
    back.setOrigin(sf::Vector2f{barWidth * 0.5f, barHeight * 0.5f});
    back.setPosition(center);                        // center 本来就是 Vector2f
    back.setFillColor(sf::Color(40, 40, 40, 220));
    window.draw(back);

    // --- 前景条（当前血量）---
    sf::RectangleShape front;
    front.setSize(sf::Vector2f{barWidth * ratio, barHeight});
    front.setOrigin(sf::Vector2f{barWidth * 0.5f, barHeight * 0.5f});

    // 为了让条从左往右填充：整体向左偏一点
    sf::Vector2f frontPos{
        center.x - (barWidth * (1.f - ratio)) * 0.5f,
        center.y
    };
    front.setPosition(frontPos);

    // 颜色：绿 -> 黄 -> 红
    sf::Color c;
    if (ratio > 0.66f)      c = sf::Color(60, 200, 60);
    else if (ratio > 0.33f) c = sf::Color(230, 210, 40);
    else                    c = sf::Color(200, 40, 40);

    front.setFillColor(c);
    window.draw(front);
}


void RenderSystem::drawUnitIcon(sf::RenderWindow& window,
                                sf::Vector2f center,
                                UnitType type,
                                Faction /*faction*/) {
    ensureFontLoaded();
    if (!fontLoaded) return;

    char ch = '?';
    switch (type) {
    case UnitType::Infantry: ch = 'I'; break;
    case UnitType::Archer:   ch = 'A'; break;
    case UnitType::Knight:   ch = 'K'; break;
    }

    sf::Text text(hudFont);
    text.setString(sf::String(ch));   // 使用 sf::String(char) 构造，SFML 3 支持这个构造:contentReference[oaicite:4]{index=4}
    text.setCharacterSize(20);
    text.setFillColor(sf::Color::Black);

    sf::FloatRect bounds = text.getLocalBounds();

    sf::Vector2f origin{
        bounds.position.x + bounds.size.x * 0.5f,
        bounds.position.y + bounds.size.y * 0.5f
    };
    text.setOrigin(origin);
    text.setPosition(center);

    window.draw(text);
}

void RenderSystem::drawMapLayer(const GameWorld& world,
                                sf::RenderWindow& window,
                                const Layout& layout)
{
    const int W = world.map.getWidth();
    const int H = world.map.getHeight();

    sf::RectangleShape tileShape;
    tileShape.setSize(sf::Vector2f{ layout.tileSize - 1.f,
                                    layout.tileSize - 1.f });

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            Coord c{ x, y };
            const auto& tile = world.map.getTile(c);

            sf::Vector2f pos{
                layout.offsetX + x * layout.tileSize,
                layout.offsetY + y * layout.tileSize
            };
            tileShape.setPosition(pos);
            tileShape.setFillColor(tileColor(tile.getType()));

            window.draw(tileShape);
        }
    }
}


void RenderSystem::drawBaseLayer(const GameWorld& world,
                                 sf::RenderWindow& window,
                                 const Layout& layout)
{
    auto drawBaseOne = [&](const std::shared_ptr<Base>& base) {
        if (!base || base->isDestroyed()) return;

        Coord   p = base->getPos();
        Faction f = base->getFaction();

        float radius = layout.tileSize * 0.45f;

        sf::CircleShape shape;
        shape.setRadius(radius);
        shape.setOrigin(sf::Vector2f{ radius, radius });

        sf::Vector2f center{
            layout.offsetX + (p.x + 0.5f) * layout.tileSize,
            layout.offsetY + (p.y + 0.5f) * layout.tileSize
        };
        shape.setPosition(center);

        shape.setFillColor(factionColor(f));
        shape.setOutlineColor(sf::Color::Black);
        shape.setOutlineThickness(2.f);

        window.draw(shape);

        // 基地血条：在基地上方
        sf::Vector2f hpCenter{
            center.x,
            center.y - layout.tileSize * 0.6f
        };
        drawHpBar(window, hpCenter, layout.tileSize * 0.9f,
                  base->hp, base->maxHp);
    };

    drawBaseOne(world.baseA);
    drawBaseOne(world.baseB);
}

void RenderSystem::drawUnitLayer(const GameWorld& world,
                                 sf::RenderWindow& window,
                                 const Layout& layout)
{
    auto drawUnits = [&](const auto& units) {
        for (const auto& uPtr : units) {
            const Unit& u = *uPtr;
            if (!u.isAlive()) continue;

            Coord   p = u.getPos();
            Faction f = u.getFaction();
            UnitType t = u.type;

            float radius = layout.tileSize * 0.30f;

            sf::CircleShape shape;
            shape.setRadius(radius);
            shape.setOrigin(sf::Vector2f{ radius, radius });

            sf::Vector2f center{
                layout.offsetX + (p.x + 0.5f) * layout.tileSize,
                layout.offsetY + (p.y + 0.5f) * layout.tileSize
            };
            shape.setPosition(center);

            // 兵种 => 填充色；阵营 => 描边色
            shape.setFillColor(unitTypeColor(t));
            shape.setOutlineColor(factionColor(f));
            shape.setOutlineThickness(2.f);

            window.draw(shape);

            // 单位血条：单位圆上方一点
            sf::Vector2f hpCenter{
                center.x,
                center.y - layout.tileSize * 0.45f
            };
            drawHpBar(window, hpCenter, layout.tileSize * 0.8f,
                      u.hp, u.baseStats.maxHP);

            // 单位字母图标：I/A/K
            drawUnitIcon(window, center, t, f);
        }
    };

    drawUnits(world.unitsA);
    drawUnits(world.unitsB);
}


// ============ HUD ============

void RenderSystem::drawHud(const GameWorld& world,
                           sf::RenderWindow& window,
                           const Layout& layout)
{
    ensureFontLoaded();
    if (!fontLoaded) return;

    int aliveA = 0;
    int aliveB = 0;
    for (const auto& u : world.unitsA) if (u->isAlive()) ++aliveA;
    for (const auto& u : world.unitsB) if (u->isAlive()) ++aliveB;

    float hpA  = (world.baseA && !world.baseA->isDestroyed())
                 ? world.baseA->hp : 0.f;
    float maxA = (world.baseA) ? world.baseA->maxHp : 1.f;

    float hpB  = (world.baseB && !world.baseB->isDestroyed())
                 ? world.baseB->hp : 0.f;
    float maxB = (world.baseB) ? world.baseB->maxHp : 1.f;

    float x = layout.hudX;
    float y = 30.f;

    // SFML 3：Text 必须带 Font 构造
    sf::Text text(hudFont, "HUD");
    text.setCharacterSize(18);
    text.setFillColor(sf::Color::White);
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
    y += 30.f;

    // 阵营 A
    text.setFillColor(factionColor(Faction::A));
    text.setString("Faction A");
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
    y += 24.f;

    text.setFillColor(sf::Color::White);
    text.setString("Units: " + std::to_string(aliveA));
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
    y += 22.f;

    text.setString("Base HP: " +
                   std::to_string(static_cast<int>(hpA)) +
                   " / " +
                   std::to_string(static_cast<int>(maxA)));
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
    y += 32.f;

    // 阵营 B
    text.setFillColor(factionColor(Faction::B));
    text.setString("Faction B");
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
    y += 24.f;

    text.setFillColor(sf::Color::White);
    text.setString("Units: " + std::to_string(aliveB));
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
    y += 22.f;

    text.setString("Base HP: " +
                   std::to_string(static_cast<int>(hpB)) +
                   " / " +
                   std::to_string(static_cast<int>(maxB)));
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
}

// ============ 渲染总入口 ============

void RenderSystem::renderSfml(const GameWorld& world,
                              sf::RenderWindow& window) 
{
    Layout layout = computeLayout(world, window);

    drawMapLayer(world, window, layout);
    drawBaseLayer(world, window, layout);
    drawUnitLayer(world, window, layout);
    drawHud(world, window, layout);
}
