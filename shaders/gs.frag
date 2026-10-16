#version 330 core
layout(location=0) out uvec2 result;
uniform usampler2D previousSurface;
uniform usampler2D texturePixels;
// Core 3.3 binds this block from C++; no 420pack binding qualifier.
struct PrimitiveParameters {
    vec4 position[3]; // xy relative to XYOFFSET, relative Z, fog
    vec4 color[3];
    vec4 stq[3];
    vec4 uv[3]; // std140 array stride is explicitly 16 bytes
    ivec4 primitive; // type, gouraud, textured, fixed UV
    ivec4 flags; // fog, blend, PABE, linear
    uvec4 tests; // TEST, ALPHA low, ALPHA FIX, FBA
    uvec4 format; // frame PSM, depth PSM, depth mask, FBMSK
    ivec4 textureInfo; // original width, height, TFX, TCC
    ivec4 wrapping; // U/V modes, unused
    ivec4 region; // MINU, MAXU, MINV, MAXV
    ivec4 sprite; // unclipped origin, span
    ivec4 fogColor;
    uvec4 misc; // base depth / raw clear word, reserved
    ivec4 bounds; // exclusive pixel rectangle after scissor
};
layout(std140) uniform DrawParameters {
    ivec4 batchInfo; // primitive count, reserved
    PrimitiveParameters items[32];
};
int drawIndex;
#define position items[drawIndex].position
#define color items[drawIndex].color
#define stq items[drawIndex].stq
#define uv items[drawIndex].uv
#define primitive items[drawIndex].primitive
#define flags items[drawIndex].flags
#define tests items[drawIndex].tests
#define format items[drawIndex].format
#define textureInfo items[drawIndex].textureInfo
#define wrapping items[drawIndex].wrapping
#define region items[drawIndex].region
#define sprite items[drawIndex].sprite
#define fogColor items[drawIndex].fogColor
#define misc items[drawIndex].misc
#define depthBase misc.x

ivec4 unpackColor(uint p) {
    return ivec4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24);
}
uint packColor(ivec4 c) {
    return uint(c.r) | (uint(c.g)<<8) | (uint(c.b)<<16) | (uint(c.a)<<24);
}
int wrapCoord(int p, int size, int mode, int lo, int hi) {
    if (mode == 0) return int(uint(p) & uint(size-1));
    if (mode == 1) return clamp(p, 0, size-1);
    if (mode == 2) return min(max(p, lo), hi);
    return int((uint(p) & uint(lo)) | uint(hi));
}
ivec4 pointSample(ivec2 p) {
    p.x = wrapCoord(p.x, textureInfo.x, wrapping.x, region.x, region.y);
    p.y = wrapCoord(p.y, textureInfo.y, wrapping.y, region.z, region.w);
    return unpackColor(texelFetch(texturePixels, p, 0).r);
}
ivec4 sampleTexture(vec2 p) {
    if (flags.w == 0) return pointSample(ivec2(p));
    vec2 a = p - vec2(.5);
    ivec2 base = ivec2(floor(a));
    vec2 f = a - vec2(base);
    vec4 c00 = vec4(pointSample(base));
    vec4 c10 = vec4(pointSample(base+ivec2(1,0)));
    vec4 c01 = vec4(pointSample(base+ivec2(0,1)));
    vec4 c11 = vec4(pointSample(base+ivec2(1,1)));
    vec4 top = c00 + (c10-c00)*f.x;
    vec4 bottom = c01 + (c11-c01)*f.x;
    return clamp(ivec4(floor(top+(bottom-top)*f.y+vec4(.5))),0,255);
}
ivec4 combine(ivec4 v, ivec4 t) {
    bool alpha = textureInfo.w != 0;
    if (textureInfo.z == 0) return ivec4(clamp((t.rgb*v.rgb)>>7,0,255), alpha ? min((t.a*v.a)>>7,255) : v.a);
    if (textureInfo.z == 1) return ivec4(t.rgb, alpha ? t.a : v.a);
    return ivec4(clamp(((t.rgb*v.rgb)>>7)+ivec3(v.a),0,255), alpha ? (textureInfo.z==2 ? min(t.a+v.a,255) : t.a) : v.a);
}
bool alphaPass(int a) {
    if ((tests.x & 1u)==0u) return true;
    uint op=(tests.x>>1)&7u;
    int ref=int((tests.x>>4)&255u);
    if (op==0u) return false;
    if (op==1u) return true;
    if (op==2u) return a<ref;
    if (op==3u) return a<=ref;
    if (op==4u) return a==ref;
    if (op==5u) return a>=ref;
    if (op==6u) return a>ref;
    return a!=ref;
}
ivec3 selectRGB(uint sel, ivec3 s, ivec3 d) {
    if (sel==0u) return s;
    if (sel==1u) return d;
    return ivec3(0);
}
float safeQ(float q) { return abs(q)>1e-8 ? q : 1.0; }
uvec2 applyPrimitive(uvec2 old, ivec2 pixel) {
    uvec2 updated = old;
    if (primitive.x==7) { updated.r=(depthBase & ~format.w)|(old.r & format.w); return updated; }
    vec3 w = vec3(0,0,1);
    vec2 texcoord=vec2(0);
    int provoking=2;
    uint depth=depthBase;
    int fog=0;
    bool triangle=primitive.x>=3 && primitive.x<=5;
    if (triangle) {
        vec2 a=position[0].xy, b=position[1].xy, c=position[2].xy;
        float denom=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
        if (abs(denom)<.001) return old;
        float winding=denom<0 ? -1.0 : 1.0;
        float inverse=1.0/abs(denom);
        vec2 p=gl_FragCoord.xy;
        w.x=(((b.y-c.y)*(p.x-c.x)+(c.x-b.x)*(p.y-c.y))*winding)*inverse;
        w.y=(((c.y-a.y)*(p.x-c.x)+(a.x-c.x)*(p.y-c.y))*winding)*inverse;
        w.z=1.0-w.x-w.y;
        if (any(lessThan(w,vec3(-1e-4)))) return old;
        depth += uint(max(0.0,position[0].z*w.x+position[1].z*w.y+position[2].z*w.z+.5));
        fog=clamp(int(position[0].w*w.x+position[1].w*w.y+position[2].w*w.z),0,255);
        if (primitive.w!=0) texcoord=vec2(ivec2(uv[0].xy*w.x+uv[1].xy*w.y+uv[2].xy*w.z) & ivec2(65535))/16.0;
        else {
            vec3 v=stq[0].xyz*w.x+stq[1].xyz*w.y+stq[2].xyz*w.z;
            texcoord=(v.xy/safeQ(v.z))*vec2(textureInfo.xy);
        }
    } else if (primitive.x==6) {
        provoking=1;
        w=vec3(0,1,0);
        depth += uint(position[1].z);
        fog=int(position[1].w);
        vec2 f=(vec2(pixel-sprite.xy)+vec2(.5))/vec2(sprite.zw);
        vec2 a,b;
        if (primitive.w!=0) { a=floor(uv[0].xy/16.0); b=floor(uv[1].xy/16.0); }
        else { a=(stq[0].xy/safeQ(stq[0].z))*vec2(textureInfo.xy); b=(stq[1].xy/safeQ(stq[1].z))*vec2(textureInfo.xy); }
        texcoord=a+(b-a)*f;
        if (primitive.w!=0) texcoord=vec2(clamp(ivec2(texcoord*16.0+vec2(.5)),0,65535))/16.0;
    } else if (primitive.x==0) {
        provoking=0; w=vec3(1,0,0); depth+=uint(position[0].z); fog=int(position[0].w);
    } else {
        // Evaluate inclusive Bresenham coverage on GPU. The major-axis
        // formula preserves the reference's tie rule in all eight octants.
        ivec2 a=ivec2(position[0].xy),b=ivec2(position[1].xy);
        ivec2 delta=abs(b-a),direction=ivec2(b.x>a.x?1:-1,b.y>a.y?1:-1);
        int step=0,total=max(delta.x,delta.y);
        if (total==0) { if(any(notEqual(pixel,a))) return old; }
        else if(delta.x>=delta.y) {
            step=(pixel.x-a.x)*direction.x;
            int expected=a.y+direction.y*((2*delta.y*step+delta.x)/(2*delta.x));
            if(step<0 || step>total || pixel.y!=expected) return old;
        } else {
            step=(pixel.y-a.y)*direction.y;
            int expected=a.x+direction.x*((2*delta.x*step+delta.y)/(2*delta.y));
            if(step<0 || step>total || pixel.x!=expected) return old;
        }
        float t=float(step)/float(max(1,total));
        provoking=1;w=vec3(1.0-t,t,0);
        depth+=uint(max(0.0,position[0].z+(position[1].z-position[0].z)*t));
        fog=clamp(int(position[0].w+(position[1].w-position[0].w)*t),0,255);
    }
    ivec4 src=ivec4(color[provoking]);
    if (triangle && primitive.y!=0) src=clamp(ivec4(color[0]*w.x+color[1]*w.y+color[2]*w.z),0,255);
    if ((primitive.x==1 || primitive.x==2) && primitive.y!=0) src=clamp(ivec4(color[0]+(color[1]-color[0])*w.y),0,255);
    // The current software reference only textures triangles and sprites.
    if (primitive.z!=0 && (triangle || primitive.x==6)) src=combine(src,sampleTexture(texcoord));
    if (flags.x!=0) src.rgb=((ivec3(fog)*src.rgb)>>8)+((ivec3(255-fog)*fogColor.rgb)>>8);
    bool writeRgb=true,writeAlpha=true,writeDepth=true;
    if (!alphaPass(src.a)) {
        uint fail=(tests.x>>12)&3u;
        if (fail==0u) return old;
        if (fail==1u) writeDepth=false;
        if (fail==2u) { writeRgb=false; writeAlpha=false; }
        if (fail==3u) { writeDepth=false; if (format.x==0u) writeAlpha=false; }
    }
    bool sixteen=format.x==2u || format.x==10u;
    if ((tests.x & (1u<<14))!=0u && format.x!=1u) {
        uint bit=(old.r>>(sixteen ? 15u : 31u))&1u;
        if (bit!=((tests.x>>15)&1u)) return old;
    }
    uint ztest=(tests.x>>17)&3u;
    if (ztest==0u || (ztest==2u && depth<old.g) || (ztest==3u && depth<=old.g)) return old;
    if (writeRgb || writeAlpha) {
        ivec4 dst=unpackColor(old.r);
        if (sixteen) dst=ivec4(int(old.r&31u)<<3,int((old.r>>5)&31u)<<3,int((old.r>>10)&31u)<<3,int((old.r>>15)&1u)<<7);
        if (format.x==1u) dst.a=128;
        if (flags.y!=0 && !(flags.z!=0 && (src.a&128)==0)) {
            uint a=tests.y&3u,b=(tests.y>>2)&3u,c=(tests.y>>4)&3u,d=(tests.y>>6)&3u;
            int factor=c==0u ? src.a : c==1u ? dst.a : int(tests.z);
            src.rgb=clamp(((selectRGB(a,src.rgb,dst.rgb)-selectRGB(b,src.rgb,dst.rgb))*factor>>7)+selectRGB(d,src.rgb,dst.rgb),0,255);
        }
        if (writeAlpha && (tests.w&1u)!=0u && format.x!=1u) src.a|=128;
        uint value=packColor(src);
        uint destination=packColor(dst);
        value=(value & ~format.w)|(destination & format.w);
        if (!writeAlpha && format.x==0u) value=(value&0xffffffu)|(destination&0xff000000u);
        if (sixteen) value=((value>>3)&31u)|(((value>>11)&31u)<<5)|(((value>>19)&31u)<<10)|((value>>31)<<15);
        if (format.x==1u) value &= 0xffffffu;
        updated.r=value;
    }
    if (writeDepth && format.z==0u) {
        if (format.y==49u) depth &= 0xffffffu;
        if (format.y==50u || format.y==58u) depth &= 65535u;
        updated.g=depth;
    }
    return updated;
}
void main() {
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    uvec2 value=texelFetch(previousSurface,pixel,0).rg;
    // Each fragment owns its destination pixel for the entire ordered batch.
    // No cross-fragment communication or framebuffer feedback is involved.
    for(int i=0;i<batchInfo.x;++i) {
        drawIndex=i;
        ivec4 r=items[i].bounds;
        if(pixel.x>=r.x && pixel.y>=r.y && pixel.x<r.z && pixel.y<r.w)
            value=applyPrimitive(value,pixel);
    }
    result=value;
}
