#pragma once
class PS2Runtime;
// Desktop diagnostic (off by default): NIGHT100_PARTICLE_WATCH=<directory>
// records every guest function that reads or writes the particle pool
// gParPool [0x379200,0x3a0700) and the dead-list head gParDead, with the node
// field offsets it touched. Install after every other dispatch replacement.
namespace ParticleWatch {
void install(PS2Runtime& runtime);
}
