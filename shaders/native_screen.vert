#version 330 core
layout(location=0) in vec4 inPosition;
layout(location=1) in vec4 inUv;
layout(location=2) in vec4 inColor;
uniform vec2 viewportSize;
noperspective out vec4 vertexColor;
out vec2 textureCoordinate;
noperspective out float vertexFog;
void main() {
    vertexFog=1.0;
    float w=1.0/max(inUv.z,0.000001);
    gl_Position=vec4(inPosition.xy/viewportSize*2.0-1.0,inPosition.z/32768.0-1.0,1.0)*w;
    vertexColor=inColor/255.0;
    textureCoordinate=inUv.xy;
}
