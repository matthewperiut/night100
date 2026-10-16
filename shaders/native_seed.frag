#version 330 core
uniform usampler2D integerSurface;
uniform uint frameFormat;
layout(location=0) out vec4 result;
void main() {
    uvec2 p=texelFetch(integerSurface,ivec2(gl_FragCoord.xy),0).rg;
    if(frameFormat==2u || frameFormat==10u)
        result=vec4(float((p.r&31u)<<3),float(((p.r>>5)&31u)<<3),float(((p.r>>10)&31u)<<3),float((p.r>>15)&1u)*255.0)/255.0;
    else
        result=vec4(float(p.r&255u),float((p.r>>8)&255u),float((p.r>>16)&255u),min(float(p.r>>24)*2.0,255.0))/255.0;
    gl_FragDepth=clamp(float(p.g)/65536.0,0.0,1.0);
}
