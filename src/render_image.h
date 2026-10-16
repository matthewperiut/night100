#pragma once
// Decoupled-render groundwork.
// Stage 0: the player render matrix `rendermat` (zEntPlayer.cpp, 0x36e7c0),
// which PlayerBoundUpdate reads in the next update, is produced at the end of
// update by a side-effect-free guest dry run of zEntPlayer_Render. The real
// render rewrites the same value, so render no longer feeds the next update.
// Stage 1 (Image, single-threaded): the render-side phases (xCameraBegin
// through zSceneRender, xCameraEnd + iEnvEndRenderFX, and presentation) see
// a render image R; everything else sees the update image U. R is U with
// render's private bytes P applied; render's writes go to P, never to U,
// except render infrastructure shared with interrupts and presentation (SDK
// DMA packets/queue/sky state, vertex staging, interrupt-written globals).
// U changes reach R through 64-byte dirty lines (NIGHT100_DIRTY_LINES build)
// against a shadow S of U. Update therefore never observes render writes.
// Exact-ELF only; off unless configured. Desktop: NIGHT100_RENDER_IMAGE=rendermat|image.
#include <cstdint>

class PS2Runtime;

namespace RenderImage {
enum class Mode : uint8_t {Off,Rendermat,Image};
struct Statistics {
    uint64_t dryRuns=0,skipped=0;        // skipped: player render callback not zEntPlayer_Render
    uint64_t rendered=0,unrendered=0;    // frames whose render did / did not enter zEntPlayer_Render
    uint64_t renderMatches=0,renderMismatches=0;
    uint64_t checkFrames=0,checkViolations=0; // NIGHT100_RENDERMAT_CHECK full-RAM write-set check
    // Image mode: per-window work (cumulative).
    uint64_t windows=0,frames=0;
    uint64_t syncedLines=0;              // update-dirty lines copied into S at window begin
    uint64_t renderLines=0;              // render-dirty lines classified at window end
    uint64_t privateBytes=0;             // bytes of P at the last window end
    uint64_t sharedBytes=0;              // render-written shared bytes kept in U, by reason:
    uint64_t sharedInfrastructureBytes=0,sharedStackBytes=0,sharedUpdatePageBytes=0;
    uint64_t beginNs=0,endNs=0,scanNs=0; // host time in window begin/end; dirty-map scans within them
    uint64_t holeLines=0;                // NIGHT100_RENDER_IMAGE_CHECK: U changes outside dirty lines
    uint64_t promotedBytes=0;            // P bytes made shared when update started writing their page
};
// Before FramePhases::install's boundary hooks run (it calls install()).
void configure(Mode);
void install(PS2Runtime&);
Mode mode();
Statistics statistics();
// Snapshot coherence: a saved state holds the render image (today's RAM);
// a restored state resynchronizes S and empties P.
void beforeSnapshotSave(PS2Runtime&);
void afterSnapshotSave(PS2Runtime&);
void afterSnapshotLoad(PS2Runtime&);
}
