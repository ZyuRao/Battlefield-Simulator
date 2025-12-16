#pragma once
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include "gameworld.hpp"

class Game {
private:
    bool running = false;
    GameWorld world;
    std::thread inputThread;
    std::atomic<bool> inputRunning{false};

public:
    Game() : running(false) {}
    ~Game() {
        world.stopRenderThread();
        if(inputRunning.load()) {
            inputRunning.store(false);
        }
        if (inputThread.joinable()) {
            inputThread.join();
        }
    }

    void run() {
        running = true;
        std::cout << "[Main] GameProject started.\n";
        std::cout << "[Game] world created\n";

        world.startRenderThread();
        inputRunning.store(true);
        inputThread = std::thread([this]() {
            std::string line;
            while (inputRunning.load()) {
                if (!std::cin.rdbuf()->in_avail()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                if (!std::getline(std::cin, line)) break;
                if (line.empty()) continue;
                world.enqueueCommand(line);
            }
        });
        
        while(running) {
            world.update();

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if(world.shouldQuit()) {
                running = false;
            }
            if(world.baseA->isDestroyed() || world.baseB->isDestroyed()) {
                running = false;
                std::string winner = !world.baseA->isDestroyed() ? "A" : "B";
                std::cout << "Winner:" << winner << std::endl;
            }
        }

        world.markGameOver();
        inputRunning.store(false);
        if (inputThread.joinable()) {
            inputThread.join();
        }
        if (world.isRenderRunning()) {
            world.waitRenderThread();
        }
        world.stopRenderThread();
    }
};
