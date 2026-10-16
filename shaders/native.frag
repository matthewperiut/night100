#version 330 core
noperspective in vec4 vertexColor;
in vec2 textureCoordinate;
uniform sampler2D diffuseTexture;
uniform int textured;
uniform float alphaReference;
uniform int fogEnabled;
uniform vec4 fogColor;
noperspective in float vertexFog;
layout(location=0) out vec4 result;
void main() {
    result=vertexColor;
    if(textured!=0) result*=texture(diffuseTexture,textureCoordinate);
    if(result.a<alphaReference) discard;
    if(fogEnabled!=0) result.rgb=mix(fogColor.rgb,result.rgb,vertexFog);
}
