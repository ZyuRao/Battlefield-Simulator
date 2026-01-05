#include "core/gameworld.hpp"
#include "core/render_config.hpp"
#include <iostream>
#include <string>
#include <optional>
#include <sstream>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>


namespace {
    float measureTextWidth(const sf::Font& font,
                            const std::string& text,
                            unsigned int size)
    {
        sf::Text measure(font, text);
        measure.setCharacterSize(size);
        return measure.getLocalBounds().size.x;
    }

    std::vector<std::string> wrapText(const sf::Font& font,
                                      const std::string& text,
                                      unsigned int size,
                                      float maxWidth)
    {
        std::vector<std::string> lines;
        if (text.empty()) {
            lines.emplace_back("");
            return lines;
        }

        std::istringstream stream(text);
        std::string word;
        std::string current;

        std::function<void(const std::string&)> appendWord;
        appendWord = [&](const std::string& token) {
            if (token.empty()) return;
            if (current.empty()) {
                if (measureTextWidth(font, token, size) <= maxWidth) {
                    current = token;
                    return;
                }
                std::string chunk;
                for (char ch : token) {
                    std::string candidate = chunk + ch;
                    if (measureTextWidth(font, candidate, size) <= maxWidth) {
                        chunk = candidate;
                    } else {
                        if (!chunk.empty()) {
                            lines.push_back(chunk);
                        }
                        chunk = std::string(1, ch);
                    }
                }
                current = chunk;
                return;
            }

            std::string candidate = current + " " + token;
            if (measureTextWidth(font, candidate, size) <= maxWidth) {
                current = candidate;
                return;
            }
            lines.push_back(current);
            current.clear();
            appendWord(token);
        };

        while (stream >> word) {
            appendWord(word);
        }
        if (!current.empty()) {
            lines.push_back(current);
        }
        if (lines.empty()) {
            lines.emplace_back("");
        }
        return lines;
    }

    std::filesystem::path resolveAssetPath(const std::filesystem::path& relative) {
        std::filesystem::path base = std::filesystem::current_path();
        for (int i = 0; i < 4; ++i) {
            auto candidate = base / relative;
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
            if (!base.has_parent_path()) break;
            base = base.parent_path();
        }
        return relative;
    }

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

    sf::IntRect rectFromPixels(int x, int y, int w, int h) {
        return sf::IntRect{{x, y}, {w, h}};
    }
}

RenderSystem::RenderSystem() : fontLoaded(false) {
    clock.restart();
    initAtlasMapping();
    loadTextures();
}

void RenderSystem::ensureFontLoaded() {
    if(fontLoaded) return;
    auto path = resolveAssetPath("assets/NotoSansMono-VariableFont_wdth,wght.ttf");
    if(hudFont.openFromFile(path.string())) {
        fontLoaded = true;
    } else {
        fontLoaded = false;
    }
}

void RenderSystem::loadTextures() {
    if (texturesLoaded) return;
    auto tilesheetPath = resolveAssetPath(
        "assets/kenney/medieval_rts/Tilesheet/medieval_tilesheet.png");
    bool tilesOk = tilesheetTexture.loadFromFile(tilesheetPath.string());
    texturesLoaded = tilesOk;
    if (!texturesLoaded) {
        std::cerr << "[RenderSystem] Failed to load Kenney tilesheet from "
                  << tilesheetPath << "\n";
        return;
    }
    tilesheetTexture.setSmooth(false);
}

void RenderSystem::initAtlasMapping() {
    tileAtlas.plain        = rectFromPixels(126,  30, 68, 68);
    tileAtlas.swamp        = rectFromPixels(318,  30, 68, 68);
    tileAtlas.river        = rectFromPixels(126, 222, 68, 68);
    tileAtlas.hillBase     = rectFromPixels(222, 126, 68, 68);
    tileAtlas.mountBase    = rectFromPixels(318, 222, 68, 68);
    tileAtlas.forest       = rectFromPixels(222, 414, 68, 68);
    tileAtlas.hillOverlay  = rectFromPixels(718, 430, 36, 38);
    tileAtlas.mountOverlay = rectFromPixels(910, 430, 36, 38);

    tileRects[static_cast<std::size_t>(TileType::PLAIN)] =
        tileAtlas.plain;
    tileRects[static_cast<std::size_t>(TileType::FOREST)] =
        tileAtlas.forest;
    tileRects[static_cast<std::size_t>(TileType::HILL)] =
        tileAtlas.hillBase;
    tileRects[static_cast<std::size_t>(TileType::SWAMP)] =
        tileAtlas.swamp;
    tileRects[static_cast<std::size_t>(TileType::RIVER)] =
        tileAtlas.river;
    tileRects[static_cast<std::size_t>(TileType::MOUNTAIN)] =
        tileAtlas.mountBase;

    baseRect = rectFromPixels(1568, 56, 64, 42);

    unitRectsA[static_cast<std::size_t>(UnitType::Infantry)] =
        rectFromPixels(1110, 434, 20, 28);
    unitRectsA[static_cast<std::size_t>(UnitType::Archer)] =
        rectFromPixels(1300, 432, 22, 30);
    unitRectsA[static_cast<std::size_t>(UnitType::Knight)] =
        rectFromPixels(1492, 434, 22, 28);

    unitRectsB[static_cast<std::size_t>(UnitType::Infantry)] =
        rectFromPixels(1110, 338, 20, 28);
    unitRectsB[static_cast<std::size_t>(UnitType::Archer)] =
        rectFromPixels(1300, 336, 22, 30);
    unitRectsB[static_cast<std::size_t>(UnitType::Knight)] =
        rectFromPixels(1492, 338, 22, 28);
}

sf::IntRect RenderSystem::tilesheetRect(int col, int row) const {
    const int step = RenderConfig::TILE_PITCH;
    const int x = RenderConfig::TILE_ORIGIN + col * step;
    const int y = RenderConfig::TILE_ORIGIN + row * step;
    return sf::IntRect{{x, y},
                       {RenderConfig::NATIVE_TILE_SIZE,
                        RenderConfig::NATIVE_TILE_SIZE}};
}

sf::IntRect RenderSystem::tileRectFor(TileType t) const {
    return tileRects[static_cast<std::size_t>(t)];
}

sf::IntRect RenderSystem::unitRectFor(UnitType t, Faction f) const {
    if (f == Faction::A) {
        return unitRectsA[static_cast<std::size_t>(t)];
    }
    return unitRectsB[static_cast<std::size_t>(t)];
}

sf::Vector2f RenderSystem::tileTopLeft(const Layout& layout, const Coord& c) const {
    return sf::Vector2f{
        layout.offsetX + static_cast<float>(c.x) * layout.tileSize,
        layout.offsetY + static_cast<float>(c.y) * layout.tileSize
    };
}

sf::Vector2f RenderSystem::tileCenter(const Layout& layout, const Coord& c) const {
    return sf::Vector2f{
        layout.offsetX + (static_cast<float>(c.x) + 0.5f) * layout.tileSize,
        layout.offsetY + (static_cast<float>(c.y) + 0.5f) * layout.tileSize
    };
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

    const float hudWidth = RenderConfig::HUD_WIDTH; // 右侧 HUD 占的宽度
    float mapAreaWidth = std::max(100.f, static_cast<float>(winSize.x) - hudWidth);
    float mapAreaHeight = static_cast<float>(winSize.y);

    float tileSize = static_cast<float>(RenderConfig::TILE_SIZE);

    // 居中放在左侧那块区域
    float usedWidth  = tileSize * W;
    float usedHeight = tileSize * H;
    float offsetX = 0.5f * (mapAreaWidth  - usedWidth);
    float offsetY = 0.5f * (mapAreaHeight - usedHeight);

    Layout lay;
    lay.tileSize = tileSize;
    lay.offsetX  = offsetX;
    lay.offsetY  = offsetY;
    lay.hudX     = mapAreaWidth + 16.f; // HUD 从地图右侧往右偏一点

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
    front.setOrigin(sf::Vector2f{0.f, barHeight * 0.5f});
    front.setPosition({center.x - barWidth * 0.5f, center.y});

    // 颜色：绿 -> 黄 -> 红
    sf::Color c;
    if (ratio > 0.66f)      c = sf::Color(60, 200, 60);
    else if (ratio > 0.33f) c = sf::Color(230, 210, 40);
    else                    c = sf::Color(200, 40, 40);

    front.setFillColor(c);
    window.draw(front);
}

void RenderSystem::drawSelectionRing(sf::RenderWindow& window,
                                     sf::Vector2f center,
                                     float radius,
                                     Faction f) {
    drawFactionRing(window, center, radius, 3.f, f);
}

void RenderSystem::drawFactionRing(sf::RenderWindow& window,
                                   sf::Vector2f center,
                                   float radius,
                                   float thickness,
                                   Faction f) {
    sf::CircleShape ring;
    ring.setRadius(radius);
    ring.setOrigin(sf::Vector2f{radius, radius});
    ring.setPosition(center);
    ring.setFillColor(sf::Color::Transparent);
    auto color = factionColor(f);
    color.a = 220;
    ring.setOutlineColor(color);
    ring.setOutlineThickness(thickness);
    window.draw(ring);
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
    if (!texturesLoaded) return;
    const int W = world.map.getWidth();
    const int H = world.map.getHeight();
    const bool hasBaseA = world.baseA && !world.baseA->isDestroyed();
    const bool hasBaseB = world.baseB && !world.baseB->isDestroyed();

    const float tileScale = layout.tileSize / 68.f;
    const float overlayFactor = 1.5f;
    const float overlayScale = tileScale * overlayFactor;
    sf::Sprite tileSprite(tilesheetTexture);
    tileSprite.setScale({tileScale, tileScale});
    tileSprite.setOrigin({0.f, 0.f});
    sf::Sprite overlaySprite(tilesheetTexture);
    overlaySprite.setScale({overlayScale, overlayScale});
    overlaySprite.setOrigin({0.f, 0.f});

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            Coord c{ x, y };
            const auto& tile = world.map.getTile(c);
            const bool hasBase =
                (hasBaseA && world.baseA->getPos() == c) ||
                (hasBaseB && world.baseB->getPos() == c);

            TileType type = tile.getType();
            sf::IntRect rect = tileRectFor(type);
            bool drawOverlay = (type == TileType::HILL || type == TileType::MOUNTAIN);

            if (hasBase && type == TileType::FOREST) {
                rect = tileAtlas.plain;
                drawOverlay = false;
            } else if (hasBase && type == TileType::HILL) {
                rect = tileAtlas.hillBase;
                drawOverlay = false;
            }

            tileSprite.setTextureRect(rect);
            sf::Vector2f topLeft = tileTopLeft(layout, c);
            tileSprite.setPosition(topLeft);
            window.draw(tileSprite);

            if (drawOverlay) {
                const sf::IntRect overlayRect =
                    (type == TileType::HILL)
                        ? tileAtlas.hillOverlay
                        : tileAtlas.mountOverlay;
                overlaySprite.setTextureRect(overlayRect);
                const float overlayW =
                    static_cast<float>(overlayRect.size.x) * overlayScale;
                const float overlayH =
                    static_cast<float>(overlayRect.size.y) * overlayScale;
                const float posX =
                    topLeft.x + (layout.tileSize - overlayW) * 0.5f;
                const float posY =
                    topLeft.y + (layout.tileSize - overlayH) * 0.5f;
                overlaySprite.setPosition({posX, posY});
                window.draw(overlaySprite);
            }
        }
    }
}


void RenderSystem::drawBaseLayer(const GameWorld& world,
                                 sf::RenderWindow& window,
                                 const Layout& layout)
{
    if (!texturesLoaded) return;
    std::vector<const Base*> bases;
    if (world.baseA && !world.baseA->isDestroyed()) bases.push_back(world.baseA.get());
    if (world.baseB && !world.baseB->isDestroyed()) bases.push_back(world.baseB.get());

    std::sort(bases.begin(), bases.end(), [](const Base* a, const Base* b) {
        if (a->getPos().y != b->getPos().y) return a->getPos().y < b->getPos().y;
        return a->getPos().x < b->getPos().x;
    });

    sf::Sprite baseSprite(tilesheetTexture);
    baseSprite.setTextureRect(baseRect);
    const float baseScaleX =
        (layout.tileSize / static_cast<float>(baseRect.size.x)) * 0.95f;
    const float baseScaleY =
        layout.tileSize / static_cast<float>(baseRect.size.y);
    baseSprite.setScale({baseScaleX, baseScaleY});
    baseSprite.setOrigin({0.f, 0.f});
    const float baseW = static_cast<float>(baseRect.size.x) * baseScaleX;
    const float baseH = static_cast<float>(baseRect.size.y) * baseScaleY;

    for (const Base* base : bases) {
        Coord p = base->getPos();
        sf::Vector2f center = tileCenter(layout, p);
        sf::Vector2f topLeft = tileTopLeft(layout, p);
        const float posX = topLeft.x + (layout.tileSize - baseW) * 0.5f;
        const float posY = topLeft.y + layout.tileSize - baseH;

        baseSprite.setPosition({posX, posY});
        window.draw(baseSprite);

        sf::Vector2f hpCenter{
            center.x,
            center.y - layout.tileSize * 0.65f
        };
        drawHpBar(window, hpCenter, layout.tileSize * 0.9f,
                  base->hp, base->maxHp);
        if (world.awaitingProductionChoice) {
            auto selected = world.productionChoiceBase.lock();
            if (selected && selected.get() == base) {
                sf::RectangleShape box;
                box.setSize({layout.tileSize, layout.tileSize});
                box.setPosition(topLeft);
                box.setFillColor(sf::Color::Transparent);
                box.setOutlineThickness(2.f);
                box.setOutlineColor(sf::Color(240, 220, 120));
                window.draw(box);
            }
        }
    }
}

void RenderSystem::drawUnitLayer(const GameWorld& world,
                                 sf::RenderWindow& window,
                                 const Layout& layout)
{
    if (!texturesLoaded) return;
    std::vector<const Unit*> units;
    units.reserve(world.unitsA.size() + world.unitsB.size());
    for (const auto& uPtr : world.unitsA) {
        if (uPtr && uPtr->isAlive()) units.push_back(uPtr.get());
    }
    for (const auto& uPtr : world.unitsB) {
        if (uPtr && uPtr->isAlive()) units.push_back(uPtr.get());
    }

    std::sort(units.begin(), units.end(), [](const Unit* a, const Unit* b) {
        if (a->getPos().y != b->getPos().y) return a->getPos().y < b->getPos().y;
        return a->getPos().x < b->getPos().x;
    });

    sf::Sprite unitSprite(tilesheetTexture);
    const float tileScale = layout.tileSize / 68.f;
    const float unitFactor = 2.2f;
    const float unitScale = tileScale * unitFactor;
    unitSprite.setScale({unitScale, unitScale});

    for (const Unit* unit : units) {
        Coord p = unit->getPos();
        Faction f = unit->getFaction();
        UnitType t = unit->type;

        sf::IntRect rect = unitRectFor(t, f);
        unitSprite.setTextureRect(rect);
        unitSprite.setOrigin({0.f, 0.f});

        sf::Vector2f topLeft = tileTopLeft(layout, p);
        const float spriteW = static_cast<float>(rect.size.x) * unitScale;
        const float spriteH = static_cast<float>(rect.size.y) * unitScale;
        const float posX = topLeft.x + (layout.tileSize - spriteW) * 0.5f;
        const float posY = topLeft.y + (layout.tileSize - spriteH) * 0.5f +
                           layout.tileSize * 0.10f;
        unitSprite.setPosition({posX, posY});
        window.draw(unitSprite);
        sf::Vector2f center = tileCenter(layout, p);
        sf::Vector2f hpCenter{
            center.x,
            center.y - layout.tileSize * 0.55f
        };
        drawHpBar(window, hpCenter, layout.tileSize * 0.8f,
                  unit->hp, unit->baseStats.maxHP);
    }

    if (world.controlMode == GameWorld::ControlMode::Targeting &&
        world.pendingTarget.has_value()) {
        Coord targetTile = world.pendingTarget->tile;
        sf::Vector2f topLeft = tileTopLeft(layout, targetTile);
        sf::RectangleShape box;
        box.setSize({layout.tileSize, layout.tileSize});
        box.setPosition(topLeft);
        box.setFillColor(sf::Color::Transparent);
        box.setOutlineThickness(2.f);
        box.setOutlineColor(sf::Color(120, 200, 240));
        window.draw(box);
    }
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

    if (world.isPaused()) {
        text.setFillColor(sf::Color(230, 200, 120));
        text.setString("Status: Paused");
        text.setPosition(sf::Vector2f{ x, y });
        window.draw(text);
        y += 24.f;
    } else if (world.awaitingProductionChoice) {
        text.setFillColor(sf::Color(200, 180, 120));
        text.setString("Enter digits to choose production\n(1=Infantry, 2=Archer, 3=Knight),\n\tpress Enter to confirm");
        text.setPosition(sf::Vector2f{ x, y });
        window.draw(text);
        y += 24.f;
    }

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
    y += 30.f;

    const auto& selected = world.getSelection();
    std::string selStr = "Selected: ";
    if (selected.empty()) selStr += "None";
    else {
        for (size_t i = 0; i < selected.size(); ++i) {
            selStr += "#" + std::to_string(selected[i]);
            if (i + 1 < selected.size()) selStr += ", ";
        }
    }
    text.setFillColor(sf::Color(200, 220, 230));
    text.setString(selStr);
    text.setPosition(sf::Vector2f{ x, y });
    window.draw(text);
}

void RenderSystem::drawCommandPanel(const GameWorld& world,
                                    sf::RenderWindow& window,
                                    const Layout& layout)
{
    ensureFontLoaded();
    if (!fontLoaded) return;

    const unsigned int titleSize = 12;
    const unsigned int inputSize = 16;
    const unsigned int bodySize = 13;
    const float padding = 12.f;
    const float labelGap = 8.f;
    const float boxWidth = std::max(280.f, static_cast<float>(window.getSize().x) - layout.hudX - 24.f);

    const float labelWidth = std::max(
        measureTextWidth(hudFont, "CMD>", inputSize),
        std::max(measureTextWidth(hudFont, "LAST", bodySize),
                 measureTextWidth(hudFont, "STATUS", bodySize))
    );

    const float contentWidth = std::max(80.f, boxWidth - padding * 2.f - labelWidth - labelGap);
    const auto inputLines = wrapText(hudFont,
                                     inputBuffer + (inputActive ? "_" : ""),
                                     inputSize,
                                     contentWidth);
    const auto lastLines = wrapText(hudFont, world.getLastCommandInput(), bodySize, contentWidth);
    const auto statusLines = wrapText(hudFont, world.getLastCommandFeedback(), bodySize, contentWidth);

    const float titleLine = hudFont.getLineSpacing(titleSize);
    const float inputLine = hudFont.getLineSpacing(inputSize);
    const float bodyLine = hudFont.getLineSpacing(bodySize);

    float boxHeight = padding * 2.f;
    boxHeight += titleLine + 4.f;
    boxHeight += inputLine * static_cast<float>(inputLines.size());
    boxHeight += 6.f;
    boxHeight += 1.f + 8.f;
    boxHeight += bodyLine * static_cast<float>(lastLines.size());
    boxHeight += 4.f;
    boxHeight += bodyLine * static_cast<float>(statusLines.size());
    boxHeight = std::max(120.f, boxHeight);

    const float x = layout.hudX;
    const float y = static_cast<float>(window.getSize().y) - boxHeight - 12.f;

    sf::RectangleShape box;
    box.setSize({boxWidth, boxHeight});
    box.setPosition({x, y});
    box.setFillColor(sf::Color(16, 18, 24, 210));
    box.setOutlineColor(sf::Color(90, 140, 230, 200));
    box.setOutlineThickness(1.5f);
    window.draw(box);

    const float cursorX = x + padding;
    float cursorY = y + padding;
    const float labelAnchorX = cursorX + labelWidth;
    const float valueX = labelAnchorX + labelGap;

    sf::Text title(hudFont, "COMMAND");
    title.setCharacterSize(titleSize);
    title.setFillColor(sf::Color(150, 180, 220));
    title.setPosition({cursorX, cursorY});
    window.draw(title);
    cursorY += titleLine + 4.f;

    sf::Text label(hudFont);
    label.setFillColor(sf::Color(150, 180, 220));

    sf::Text value(hudFont);
    value.setFillColor(sf::Color::White);

    for (size_t i = 0; i < inputLines.size(); ++i) {
        if (i == 0) {
            label.setString("CMD>");
            label.setCharacterSize(inputSize);
            auto bounds = label.getLocalBounds();
            label.setOrigin({bounds.position.x + bounds.size.x, bounds.position.y});
            label.setPosition({labelAnchorX, cursorY});
            window.draw(label);
        }

        value.setCharacterSize(inputSize);
        value.setStyle(sf::Text::Bold);
        value.setString(inputLines[i]);
        value.setPosition({valueX, cursorY});
        window.draw(value);
        cursorY += inputLine;
    }

    sf::RectangleShape divider;
    divider.setSize({boxWidth - padding * 2.f, 1.f});
    divider.setPosition({cursorX, cursorY});
    divider.setFillColor(sf::Color(80, 110, 150, 180));
    window.draw(divider);
    cursorY += 8.f;

    value.setCharacterSize(bodySize);
    value.setStyle(sf::Text::Regular);
    value.setFillColor(sf::Color(200, 200, 210));
    for (size_t i = 0; i < lastLines.size(); ++i) {
        if (i == 0) {
            label.setString("LAST");
            label.setCharacterSize(bodySize);
            auto bounds = label.getLocalBounds();
            label.setOrigin({bounds.position.x + bounds.size.x, bounds.position.y});
            label.setPosition({labelAnchorX, cursorY});
            window.draw(label);
        }
        value.setString(lastLines[i]);
        value.setPosition({valueX, cursorY});
        window.draw(value);
        cursorY += bodyLine;
    }
    cursorY += 4.f;

    value.setFillColor(sf::Color(170, 220, 170));
    for (size_t i = 0; i < statusLines.size(); ++i) {
        if (i == 0) {
            label.setString("STATUS");
            label.setCharacterSize(bodySize);
            auto bounds = label.getLocalBounds();
            label.setOrigin({bounds.position.x + bounds.size.x, bounds.position.y});
            label.setPosition({labelAnchorX, cursorY});
            window.draw(label);
        }
        value.setString(statusLines[i]);
        value.setPosition({valueX, cursorY});
        window.draw(value);
        cursorY += bodyLine;
    }
}

void RenderSystem::drawIntroOverlay(sf::RenderWindow& window) {
    float t = clock.getElapsedTime().asSeconds();
    if (t > 1.6f) return;

    float alpha = static_cast<float>(std::max(0.0f, 1.0f - t / 1.6f));
    sf::RectangleShape fade;
    fade.setSize(sf::Vector2f{static_cast<float>(window.getSize().x),
                              static_cast<float>(window.getSize().y)});
    fade.setFillColor(sf::Color(0, 0, 0, static_cast<std::uint8_t>(alpha * 180.f)));
    window.draw(fade);
}

void RenderSystem::drawWinOverlay(const GameWorld& world,
                                  sf::RenderWindow& window,
                                  const Layout& /*layout*/)
{
    bool aDead = world.baseA && world.baseA->isDestroyed();
    bool bDead = world.baseB && world.baseB->isDestroyed();
    if (!aDead && !bDead) return;

    std::string textStr = aDead ? "Faction B Wins" : "Faction A Wins";
    ensureFontLoaded();
    if (!fontLoaded) return;

    sf::RectangleShape mask;
    mask.setSize(sf::Vector2f{static_cast<float>(window.getSize().x),
                              static_cast<float>(window.getSize().y)});
    mask.setFillColor(sf::Color(0, 0, 0, 120));
    window.draw(mask);

    float t = std::fmod(clock.getElapsedTime().asSeconds(), 1.5f);
    float scale = 1.0f + 0.05f * std::sin(t * 3.14f * 2.f);

    sf::Text txt(hudFont, textStr);
    txt.setCharacterSize(34);
    txt.setFillColor(sf::Color(255, 220, 120));
    auto bounds = txt.getLocalBounds();
    txt.setOrigin({bounds.position.x + bounds.size.x * 0.5f,
                   bounds.position.y + bounds.size.y * 0.5f});
    txt.setPosition({window.getSize().x * 0.5f, window.getSize().y * 0.5f});
    txt.setScale({scale, scale});
    window.draw(txt);
}

std::optional<Coord> RenderSystem::pixelToTile(const GameWorld& world,
                                               const sf::RenderWindow& window,
                                               const sf::Vector2i& pixel) const
{
    Layout layout = computeLayout(world, window);
    float localX = static_cast<float>(pixel.x) - layout.offsetX;
    float localY = static_cast<float>(pixel.y) - layout.offsetY;
    if (localX < 0.f || localY < 0.f) return std::nullopt;
    int gx = static_cast<int>(localX / layout.tileSize);
    int gy = static_cast<int>(localY / layout.tileSize);

    if (gx < 0 || gy < 0 ||
        gx >= world.map.getWidth() ||
        gy >= world.map.getHeight()) {
        return std::nullopt;
    }
    return Coord{gx, gy};
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
    drawCommandPanel(world, window, layout);
    drawWinOverlay(world, window, layout);
    drawIntroOverlay(window);
}
