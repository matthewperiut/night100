#version 460
layout(location=0) noperspective in vec4 vertexColor;
layout(location=1) in vec2 textureCoordinate;
layout(location=2) noperspective in float vertexFog;
layout(location=3) noperspective in float gsVertexAlpha;
layout(binding=0) uniform sampler2D diffuseTexture;
layout(std140,binding=0) uniform Fragment {vec4 controls;vec4 fogColor;vec4 gsAlphaTest;};
layout(location=0) out vec4 result;
void main() {
    result=vertexColor;
    int gsAlpha=clamp(int(gsVertexAlpha),0,255);
    if(controls.x!=0.0) {
        vec4 t=texture(diffuseTexture,textureCoordinate);
        if(gsAlphaTest.x!=0.0) {
            int textureAlpha=clamp(int(floor(t.a*255.0+0.5)),0,255);
            if(controls.w==0.0)gsAlpha=min((gsAlpha*textureAlpha)>>7,255);
            else if(controls.w==1.0)gsAlpha=textureAlpha;
            // TCC=RGB (combine modes 2/3) retains the vertex alpha byte.
        }
        if(controls.w==1.0)result=t;
        else if(controls.w==2.0)result*=vec4(t.rgb,1.0);
        else if(controls.w==3.0)result=vec4(t.rgb,result.a);
        else result*=t;
    }
    if(gsAlphaTest.x!=0.0) {
        int op=int(gsAlphaTest.y),reference=int(gsAlphaTest.z);
        bool pass=op==1 || (op==2 && gsAlpha<reference) || (op==3 && gsAlpha<=reference) ||
            (op==4 && gsAlpha==reference) || (op==5 && gsAlpha>=reference) ||
            (op==6 && gsAlpha>reference) || (op==7 && gsAlpha!=reference);
        if(pass==(gsAlphaTest.w!=0.0))discard;
        result.a=float(min(gsAlpha*2,255))/255.0;
    }
    if(result.a<controls.y)discard;
    if(controls.z!=0.0)result.rgb=mix(fogColor.rgb,result.rgb,vertexFog);
}
