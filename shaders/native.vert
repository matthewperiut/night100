#version 330 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUv;
layout(location=3) in vec4 inColor;
layout(location=4) in vec4 inWeights;
layout(location=5) in uvec4 inBones;
layout(std140) uniform InverseBind { mat4 inverseBind[64]; };
layout(std140) uniform BonePose { mat4 bonePose[64]; };
uniform mat4 worldMatrix;
uniform mat4 cameraMatrix;
uniform int boneCount;
uniform uint geometryFlags;
uniform vec4 materialColor;
uniform vec2 surfaceProperties;
uniform vec2 depthMapping;
uniform int projectionType;
uniform vec4 ambientLight;
uniform int lightCount;
uniform vec4 lightColor[8];
uniform vec4 lightDirection[8];
uniform int fogEnabled;
uniform vec2 fogRange;
noperspective out float vertexFog;
noperspective out vec4 vertexColor;
out vec2 textureCoordinate;
void main() {
    vec4 local=vec4(inPosition,1.0);
    vec3 normal=inNormal;
    if(boneCount>0) {
        vec3 p=vec3(0),n=vec3(0);
        for(int i=0;i<4;++i) {
            if(inWeights[i]<=0.0) continue;
            int index=min(int(inBones[i]),boneCount-1);
            mat4 skin=bonePose[index]*inverseBind[index];
            p+=(skin*local).xyz*inWeights[i];
            n+=mat3(skin)*normal*inWeights[i];
        }
        local=vec4(p,1);normal=n;
    }
    vec4 world=worldMatrix*local;
    vec3 projected=(cameraMatrix*world).xyz;
    bool parallel=projectionType==2;
    float w=parallel ? 1.0 : projected.z;
    vertexFog=fogEnabled!=0 ? clamp((fogRange.y-projected.z)/max(fogRange.y-fogRange.x,0.00001),0.0,1.0) : 1.0;
    float z=parallel ? depthMapping.x*projected.z+depthMapping.y : depthMapping.x+depthMapping.y*w;
    // RenderWare's PS2 view matrix already maps x/y to [0,w]. Raw GS depth
    // is zScale/w+zShift in perspective and zScale*z+zShift in parallel.
    // Map its 16-bit viewport range to reversed GL depth.
    gl_Position=vec4(projected.xy*2.0-vec2(w),z*(2.0/65536.0)-w,w);
    vec4 c=(geometryFlags&8u)!=0u ? inColor : vec4(0,0,0,inColor.a);
    if((geometryFlags&32u)!=0u) {
        vec3 n=mat3(worldMatrix)*normal;
        n=normalize(n);
        c.rgb+=ambientLight.rgb*surfaceProperties.x;
        for(int i=0;i<lightCount;++i)
            c.rgb+=lightColor[i].rgb*max(0.0,-dot(n,lightDirection[i].xyz))*surfaceProperties.y;
    }
    c=clamp(c,0.0,1.0);
    if((geometryFlags&64u)!=0u)c.rgb*=materialColor.rgb;
    c.a*=materialColor.a;
    vertexColor=c;textureCoordinate=inUv;
}
