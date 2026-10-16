#version 330 core
uniform sampler2D nativeColor;
uniform sampler2D nativeDepth;
uniform uint frameFormat;
uniform uint depthFormat;
layout(location=0) out uvec2 result;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    vec4 c=texelFetch(nativeColor,p,0);
    uvec4 bytes=uvec4(clamp(floor(c*vec4(255,255,255,128)+0.5),0.0,255.0));
    uint color=bytes.r|(bytes.g<<8)|(bytes.b<<16)|(bytes.a<<24);
    if(frameFormat==2u || frameFormat==10u)
        color=(bytes.r>>3)|((bytes.g>>3)<<5)|((bytes.b>>3)<<10)|((bytes.a&128u)<<8);
    uint depth=uint(floor(texelFetch(nativeDepth,p,0).r*65536.0+0.5));
    if(depthFormat==50u || depthFormat==58u)depth&=65535u;
    else if(depthFormat==49u)depth&=0xffffffu;
    result=uvec2(color,depth);
}
