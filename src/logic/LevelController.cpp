#include "LevelController.hpp"

#include <algorithm>
#include <vector>

#include "content/Content.hpp"
#include "debug/Logger.hpp"
#include "engine/Engine.hpp"
#include "engine/EnginePaths.hpp"
#include "world/files/WorldFiles.hpp"
#include "maths/voxmaths.hpp"
#include "objects/Entities.hpp"
#include "objects/Players.hpp"
#include "objects/Player.hpp"
#include "physics/Hitbox.hpp"
#include "voxels/Chunks.hpp"
#include "voxels/Pathfinding.hpp"
#include "scripting/scripting.hpp"
#include "lighting/Lighting.hpp"
#include "settings.hpp"
#include "world/LevelEvents.hpp"
#include "world/Level.hpp"
#include "world/World.hpp"

static debug::Logger logger("level-control");

LevelController::LevelController(
    Engine& engine, std::unique_ptr<Level> levelPtr, Player* clientPlayer
)
    : engine(engine),
      settings(engine.getSettings()),
      level(std::move(levelPtr)),
      chunks(std::make_unique<ChunksController>(*level)),
      playerTickClock(20, 3),
      clientPlayer(clientPlayer) {
    
    level->events->listen(LevelEventType::CHUNK_PRESENT, [](auto, Chunk* chunk) {
        scripting::on_chunk_present(*chunk, chunk->flags.loaded);
    });
    level->events->listen(LevelEventType::CHUNK_UNLOAD, [](auto, Chunk* chunk) {
        scripting::on_chunk_remove(*chunk);
    });

    if (clientPlayer) {
        chunks->lighting = std::make_unique<Lighting>(
            *level->content.getIndices(), *clientPlayer->chunks
        );
    }
    blocks = std::make_unique<BlocksController>(
        *level, chunks ? chunks->lighting.get() : nullptr
    );
    scripting::on_world_load(this);

    // TODO: do something to players added later
    int confirmed;
    do {
        confirmed = 0;
        for (const auto& [_, player] : *level->players) {
            if (!player->isLoadingChunks()) {
                confirmed++;
                continue;
            }
            glm::vec3 position = player->getPosition();
            player->chunks->configure(
                std::floor(position.x), std::floor(position.z), 1
            );
            chunks->update(16'000, 1, 0, *player, player.get() == clientPlayer);
            if (player->chunks->get(
                    std::floor(position.x), 0, std::floor(position.z)
                )) {
                confirmed++;
            }
        }
    } while (confirmed < level->players->size());
}

void LevelController::update(float delta, bool pause) {
    level->pathfinding->performAllAsync(
        settings.pathfinding.stepsPerAsyncAgent.get()
    );
    // the chunks loading budget is shared by all players: the client player
    // goes first, the others take turns starting from a rotating one
    std::vector<Player*> players;
    for (const auto& [_, player] : *level->players) {
        if (!player->isSuspended() && player.get() != clientPlayer) {
            players.push_back(player.get());
        }
    }
    if (!players.empty()) {
        std::rotate(
            players.begin(),
            players.begin() + chunksTurn++ % players.size(),
            players.end()
        );
    }
    if (clientPlayer && !clientPlayer->isSuspended()) {
        players.insert(players.begin(), clientPlayer);
    }
    int64_t budget = settings.chunks.loadSpeed.get() * 1000;
    for (auto player : players) {
        player->rotationInterpolation.updateTimer(delta);
        glm::vec3 position = player->getPosition();
        player->chunks->configure(
            glm::floor(position.x),
            glm::floor(position.z),
            settings.chunks.loadDistance.get() + settings.chunks.padding.get()
        );
        if (budget > 0) {
            budget -= chunks->update(
                budget,
                settings.chunks.loadDistance.get(),
                settings.chunks.padding.get(),
                *player,
                player == clientPlayer
            );
        }
        player->updateEntity();
    }
    if (!pause) {
        blocks->update(delta, settings.chunks.padding.get());
        level->entities->update(delta);
        for (const auto& [_, player] : *level->players) {
            if (player->isSuspended()) {
                continue;
            }
            if (int parts = playerTickClock.update(delta)) {
                for (int i = 0; i < parts; i++) {
                    if (player->getId() % playerTickClock.getParts() !=
                        playerTickClock.convertPart(i)) {
                        continue;
                    }
                    const auto& position = player->getPosition();
                    if (player->chunks->get(
                        std::floor(position.x),
                        std::floor(position.y),
                        std::floor(position.z)
                    )){
                        scripting::on_player_tick(
                            player.get(), playerTickClock.getTickRate()
                        );
                    }
                }

            }
        }
    }
    level->entities->clean();
}

void LevelController::processBeforeQuit() {
    preQuitCallbacks.notify();
    // todo: move somewhere else
    for (auto player : level->players->getAll()) {
        if (player->chunks) {
            player->chunks->saveAndClear();
        }
    }
    scripting::process_before_quit();
}

void LevelController::saveWorld() {
    auto& world = level->getWorld();
    if (world.isNameless()) {
        logger.info() << "nameless world will not be saved";
        return;
    }
    logger.info() << "writing world '" << world.getName() << "'";
    world.wfile->createDirectories();
    scripting::on_world_save();
    level->onSave();
    level->getWorld().write(*level);
}

void LevelController::onWorldQuit() {
    scripting::on_world_quit();
    engine.getPaths().setCurrentWorldFolder("");
}

Level* LevelController::getLevel() {
    return level.get();
}

BlocksController* LevelController::getBlocksController() {
    return blocks.get();
}

ChunksController* LevelController::getChunksController() {
    return chunks.get();
}
