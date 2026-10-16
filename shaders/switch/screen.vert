#version 460
layout(location=0) in vec4 inPosition;
layout(location=1) in vec4 inUv;
layout(location=2) in vec4 inColor;
layout(std140,binding=0) uniform Viewport {vec4 viewport;};
layout(location=0) noperspective out vec4 vertexColor;
layout(location=1) out vec2 textureCoordinate;
layout(location=2) noperspective out float vertexFog;
layout(location=3) noperspective out float gsVertexAlpha;
void main() {
    float w=1.0/max(inUv.z,0.000001);
    gl_Position=vec4(inPosition.x/viewport.x*2.0-1.0,1.0-inPosition.y/viewport.y*2.0,inPosition.z/65536.0,1.0)*w;
    vertexColor=inColor/255.0;
    textureCoordinate=inUv.xy;
    vertexFog=inPosition.w;
    gsVertexAlpha=inUv.w;
}
