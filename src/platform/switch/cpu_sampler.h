#pragma once

// Bounded, opt-in native PC observations. Host methods run only on the host
// poll thread; producerThreadReady is the sole producer-thread operation.
// This diagnostic briefly pauses a peer and can perturb its scheduling. It is
// never enabled by ordinary CPU phase profiling or normal gameplay.
namespace SwitchCpuSampler {
void configure(bool enabled) noexcept;
void producerThreadReady() noexcept;
// The caller admits only live gameplay, executing EE and idle GPU stages.
// Sampling begins at the first admitted poll, at most 20Hz /1024 observations /
//60 seconds. A completed window dumps once after the peer has been resumed.
void poll(bool eligible) noexcept;
// Stops any remaining sampling and emits compact hexadecimal PC,LR,SP CSV to
// the existing buffered logger. No stack memory is dereferenced.
void dump() noexcept;
}

// A runtime address paired with the linked ELF symbol supplies the ASLR slide.
extern "C" void night100SwitchCpuSamplerAnchor() noexcept;
