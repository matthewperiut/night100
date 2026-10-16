#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using NativeMatrix=std::array<float,16>;
// CPU caches and queued snapshots own this token. GPU caches keep only a weak
// reference, so a completed scene's resources can retire without a GPU readback.
struct NativeAssetLifetime {};
struct NativeVertex {
    std::array<float,3> position{},normal{};
    std::array<float,2> uv{};
    uint32_t color=0xffffffffu;
    std::array<float,4> weights{1,0,0,0};
    std::array<uint8_t,4> bones{};
};
static_assert(sizeof(NativeVertex)==56);
struct NativeMesh {
    uint32_t firstIndex=0,indexCount=0,material=0;
};
struct NativeGeometry {
    std::shared_ptr<const NativeAssetLifetime> lifetime;
    uint64_t id=0,revision=0;
    uint32_t flags=0,boneCount=0;
    bool dynamic=false;
    std::vector<NativeVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<NativeMesh> meshes;
    std::array<NativeMatrix,64> inverseBind{};
};
// Imported camera surfaces have a host identity. Their original memory layout
// stays in the backend import adapter; ordered draws resolve the current image.
struct NativeRenderTexture {
    uint64_t sourceId=0;
};
struct NativeTexture {
    std::shared_ptr<const NativeAssetLifetime> lifetime;
    uint64_t id=0;
    uint32_t width=0,height=0;
    bool linear=false;
    uint32_t wrapU=0,wrapV=0;
    std::optional<NativeRenderTexture> renderTarget;
    std::vector<uint8_t> rgba;
};
struct NativeMaterial {
    std::array<float,4> color{1,1,1,1};
    float ambient=1,diffuse=1;
    std::shared_ptr<const NativeTexture> texture;
};
struct NativeLights {
    std::array<float,4> ambient{};
    std::array<std::array<float,4>,8> color{},direction{};
    uint32_t count=0;
};
struct NativeModelDraw {
    std::shared_ptr<const NativeGeometry> geometry;
    std::vector<NativeMaterial> materials;
    NativeMatrix world{},camera{};
    std::array<NativeMatrix,64> pose{};
    NativeLights lights{};
    float zScale=1,zShift=0;
    uint32_t sourceBlend=5,destinationBlend=6,cullMode=1,projectionType=1;
    bool depthTest=true,depthWrite=true,vertexAlpha=true,indexed=true;
    float alphaReference=0.5f;
    bool fogEnabled=false;
    // Drawn through the 2D model path (xModelRender2D: HUD icons, menu models).
    // Widescreen keeps these in the 4:3 area instead of the world camera's wide view.
    bool hud=false;
    // The captured clip transform was built from the widescreen-widened view window.
    bool cameraWidened=false;
    std::array<float,4> fogColor{};
    float fogStart=0,fogEnd=1;

};
struct NativeScreenVertex {
    std::array<float,4> position{};
    std::array<float,4> uv{};
    std::array<float,4> color{};
    std::array<float,4> normal{};
};
static_assert(sizeof(NativeScreenVertex)==64);
struct NativeScreenDraw {
    std::vector<NativeScreenVertex> vertices;
    std::vector<uint16_t> indices;
    std::shared_ptr<const NativeTexture> texture;
    uint32_t primitive=3,sourceBlend=5,destinationBlend=6;
    bool depthTest=false,depthWrite=false,vertexAlpha=true;
};
