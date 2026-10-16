#pragma once
#include "native_model.h"
#include <array>
#include <map>
#include <unordered_map>

// Guest heap addresses are identities only within a scene resource lifetime.
// IDs remain monotonic across retire(), including reloads of the same scene.
// Queued commands own immutable snapshots, so retiring lookup entries cannot
// replace assets which an earlier GIF marker has yet to consume.
struct NativeAssetCache {
    using TextureKey=std::array<uint32_t,11>;
    std::unordered_map<uint64_t,std::shared_ptr<NativeGeometry>> geometries;
    std::map<TextureKey,std::shared_ptr<NativeTexture>> textures;

    std::shared_ptr<NativeGeometry> createGeometry(const std::shared_ptr<NativeGeometry>& previous={}) {
        auto result=std::make_shared<NativeGeometry>();result->lifetime=lifetime;
        result->id=previous?previous->id:++nextGeometryId;
        result->revision=previous?previous->revision+1:1;
        return result;
    }
    std::shared_ptr<NativeTexture> createTexture() {
        auto result=std::make_shared<NativeTexture>();result->lifetime=lifetime;result->id=++nextTextureId;
        return result;
    }
    void retire() {
        geometries.clear();textures.clear();lifetime=std::make_shared<NativeAssetLifetime>();
    }
private:
    std::shared_ptr<const NativeAssetLifetime> lifetime=std::make_shared<NativeAssetLifetime>();
    uint64_t nextGeometryId=0,nextTextureId=0;
};
