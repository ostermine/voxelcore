#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "typedefs.hpp"

class Level;
class Chunk;
class Chunks;
class Player;
class Lighting;
class WorldGenerator;

/// @brief ChunksController manages chunks dynamic loading/unloading
class ChunksController {
private:
    Level& level;
    std::unique_ptr<WorldGenerator> generator;
    /// @brief Data of chunks to load, for a world without generation (logic
    /// flag "generation" off): its chunks come only from here; key - chunk
    /// position
    mutable std::unordered_map<int64_t, std::vector<ubyte>> chunksData;

    /// @brief Process one chunk: load it or calculate lights for it
    bool loadVisible(const Player& player, uint padding, bool isLocalPlayer) const;
    bool buildLights(const Player& player, const std::shared_ptr<Chunk>& chunk) const;
    void createChunk(const Player& player, int x, int y) const;
public:
    std::unique_ptr<Lighting> lighting;

    ChunksController(Level& level);
    ~ChunksController();

    /// @param maxDuration milliseconds reserved for chunks loading
    void update(
        int64_t maxDuration,
        int loadDistance,
        uint padding,
        Player& player,
        bool isLocalPlayer
    ) const;

    bool isInLoadingZone(const Player& player, uint padding, int x, int z) const;

    /// @brief Logic flag "generation": false - chunks are never generated,
    /// a missing chunk loads when its data is put (putChunkData)
    bool isGenerationEnabled() const;

    /// @brief Keep the data of a chunk that is not loaded until it loads
    /// (worlds without generation, e.g. a network game client)
    void putChunkData(int x, int z, std::vector<ubyte> data);

    const WorldGenerator* getGenerator() const {
        return generator.get();
    }
};
