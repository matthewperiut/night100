#version 460
layout(location=0) in vec2 uv;
layout(binding=0) uniform sampler2D luma;
layout(binding=1) uniform sampler2D chroma;
layout(std140,binding=0) uniform Conversion {
    vec4 crop;
    vec4 range;
    vec4 coefficients;
};
layout(location=0) out vec4 result;
void main() {
    float y=texture(luma,uv*crop.xy).r*range.x+range.y;
    vec2 c=(texture(chroma,uv*crop.zw).rg-vec2(128.0/255.0))*range.z;
    result=vec4(y+coefficients.x*c.y,y+coefficients.y*c.x+coefficients.z*c.y,y+coefficients.w*c.x,1.0);
}
