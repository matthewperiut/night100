#version 330 core
layout(location=0) out vec4 result;
uniform usampler2D circuit1;
uniform usampler2D circuit2;
uniform ivec4 source1; // width, height, origin X, origin Y
uniform ivec4 source2;
uniform ivec4 formats; // circuit 1 PSM, circuit 2 PSM, dual circuits, unused
uniform ivec4 pmode; // MMOD, AMOD, SLBG, ALP
uniform uvec3 background;
uniform ivec2 outputSize;

uvec4 displayColor(usampler2D image,ivec2 pixel,ivec4 source,int psm) {
    uint raw=texelFetch(image,pixel+source.zw,0).r;
    if(psm==2 || psm==10) {
        uvec3 rgb=uvec3(raw&31u,(raw>>5)&31u,(raw>>10)&31u);
        return uvec4((rgb<<3)|(rgb>>2),(raw&32768u)!=0u?128u:0u);
    }
    return uvec4(raw&255u,(raw>>8)&255u,(raw>>16)&255u,psm==1?255u:(raw>>24)&255u);
}

void main() {
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    if(any(greaterThanEqual(pixel,outputSize))){result=vec4(0);return;}
    uvec4 color;
    if(formats.z==0)color=displayColor(circuit1,pixel,source1,formats.x);
    else {
        color=uvec4(background,uint(pmode.w));
        if(pmode.z==0 && all(lessThan(pixel,source2.xy)))color=displayColor(circuit2,pixel,source2,formats.y);
        if(all(lessThan(pixel,source1.xy))) {
            uvec4 src=displayColor(circuit1,pixel,source1,formats.x);
            int factor=pmode.x!=0?pmode.w:int(min(255u,src.a*2u));
            // GLSL signed division truncates toward zero, matching the CPU
            // compositor for negative as well as positive channel deltas.
            color.rgb=uvec3(clamp(ivec3(color.rgb)+(ivec3(src.rgb)-ivec3(color.rgb))*factor/255,ivec3(0),ivec3(255)));
        }
    }
    result=vec4(uvec4(color.rgb,255u))/255.0;
}
