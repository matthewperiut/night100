#version 460
layout(location=0) in vec2 uv;
layout(binding=0) uniform sampler2D sourceSurface;
layout(std140,binding=0) uniform Conversion {uvec4 formatAlpha;};
layout(location=0) out vec4 result;
void main() {
    // Reproduce native_resolve.frag followed by native_render_texture.frag:
    // normalized camera output first becomes GS storage, then a TEXA texture.
    vec4 source=texelFetch(sourceSurface,ivec2(uv*vec2(textureSize(sourceSurface,0))),0);
    uvec4 raw=uvec4(clamp(floor(source*vec4(255.0,255.0,255.0,128.0)+0.5),0.0,255.0));
    uvec3 rgb=raw.rgb;
    uint alpha=raw.a;
    if(formatAlpha.x==2u || formatAlpha.x==10u) {
        rgb=(rgb>>3u)<<3u;
        bool alphaBit=(raw.a&128u)!=0u;
        alpha=alphaBit?formatAlpha.z:formatAlpha.y;
        if(!alphaBit && formatAlpha.w!=0u && all(equal(rgb,uvec3(0))))alpha=0u;
    } else if(formatAlpha.x==1u) {
        alpha=formatAlpha.y;
        if(formatAlpha.w!=0u && all(equal(rgb,uvec3(0))))alpha=0u;
    }
    result=vec4(uvec4(rgb,min(255u,alpha*2u)))/255.0;
}
