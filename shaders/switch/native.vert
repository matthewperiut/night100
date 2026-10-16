#version 460
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUv;
layout(location=3) in vec4 inColor;
layout(location=4) in vec4 inWeights;
layout(location=5) in uvec4 inBones;
layout(std140,binding=0) uniform Transform {
    mat4 worldMatrix;
    mat4 cameraMatrix;
    vec4 depthSurface;
    uvec4 config;
    vec4 materialColor;
    vec4 ambientLight;
    vec4 lightColor[8];
    vec4 lightDirection[8];
    vec4 fogRange;
};
layout(std140,binding=1) uniform BonePose {mat4 bonePose[64];};
layout(std140,binding=2) uniform InverseBind {mat4 inverseBind[64];};
layout(location=0) noperspective out vec4 vertexColor;
layout(location=1) out vec2 textureCoordinate;
layout(location=2) noperspective out float vertexFog;
layout(location=3) noperspective out float gsVertexAlpha;
void main() {
    gsVertexAlpha=0.0;
    vec4 local=vec4(inPosition,1.0);
    vec3 normal=inNormal;
    if(config.x>0u) {
        vec3 p=vec3(0.0),n=vec3(0.0);
        for(int i=0;i<4;++i) {
            if(inWeights[i]<=0.0)continue;
            uint index=min(inBones[i],config.x-1u);
            mat4 skin=bonePose[index]*inverseBind[index];
            p+=(skin*local).xyz*inWeights[i];
            n+=mat3(skin)*normal*inWeights[i];
        }
        local=vec4(p,1.0);normal=n;
    }
    vec4 world=worldMatrix*local;
    vec3 projected=(cameraMatrix*world).xyz;
    bool parallel=config.w==2u;
    float w=parallel?1.0:projected.z;
    float z=parallel?depthSurface.x*projected.z+depthSurface.y:depthSurface.x+depthSurface.y*w;
    // deko3d uses a top-left viewport and a zero-to-one clip depth.
    gl_Position=vec4(projected.x*2.0-w,w-projected.y*2.0,z/65536.0,w);
    vertexFog=fogRange.z!=0.0?clamp((fogRange.y-projected.z)/max(fogRange.y-fogRange.x,0.00001),0.0,1.0):1.0;
    vec4 c=(config.y&8u)!=0u?inColor:vec4(0.0,0.0,0.0,inColor.a);
    if((config.y&32u)!=0u) {
        vec3 n=normalize(mat3(worldMatrix)*normal);
        c.rgb+=ambientLight.rgb*depthSurface.z;
        for(uint i=0u;i<config.z;++i)
            c.rgb+=lightColor[i].rgb*max(0.0,-dot(n,lightDirection[i].xyz))*depthSurface.w;
    }
    c=clamp(c,0.0,1.0);
    if((config.y&64u)!=0u)c.rgb*=materialColor.rgb;
    c.a*=materialColor.a;
    vertexColor=c;textureCoordinate=inUv;
}
