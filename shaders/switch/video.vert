#version 460
layout(location=0) out vec2 uv;
void main() {
    vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
    uv=vec2(p.x,1.0-p.y);
}
