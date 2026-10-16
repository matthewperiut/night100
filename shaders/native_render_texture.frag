#version 330 core
layout(location=0) out vec4 result;
uniform usampler2D sourceSurface;
uniform int sourceFormat;
uniform uvec3 textureAlpha; // TA0, TA1, AEM
void main() {
    uint raw=texelFetch(sourceSurface,ivec2(gl_FragCoord.xy),0).r;
    uvec3 rgb;uint alpha;
    if(sourceFormat==2 || sourceFormat==10) {
        rgb=uvec3(raw&31u,(raw>>5)&31u,(raw>>10)&31u)<<3;
        alpha=(raw&32768u)!=0u?textureAlpha.y:textureAlpha.x;
        if((raw&32768u)==0u && textureAlpha.z!=0u && all(equal(rgb,uvec3(0))))alpha=0u;
    } else {
        rgb=uvec3(raw&255u,(raw>>8)&255u,(raw>>16)&255u);
        alpha=sourceFormat==1?textureAlpha.x:(raw>>24)&255u;
        if(sourceFormat==1 && textureAlpha.z!=0u && all(equal(rgb,uvec3(0))))alpha=0u;
    }
    // Native material textures use normalized 0..255 alpha, while GS targets
    // encode the RenderWare opaque value as 128. Texture RGB keeps GS's exact
    // five-bit expansion (no display-only low-bit replication).
    result=vec4(uvec4(rgb,min(255u,alpha*2u)))/255.0;
}
