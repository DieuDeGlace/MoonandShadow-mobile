#pragma once
// Inline terrain shadows: HLSL appended to SteamShadows' `hlsl` source.
//
// Steam's world vertex shader (exe offset 0x436fb0) and its two world pixel
// shaders (0x438810 "shine", 0x43b500 "plain") are re-expressed instruction
// by instruction, so a terrain chunk drawn with these shaders produces the same
// colour, depth and interpolants as the native draw. The pixel shader then
// applies the receiver pass's shadow term in place: native * transmission +
// sun warmth, exactly what the receiver blend did to the stored native colour.
// Terrain therefore no longer needs a second (receiver) geometry pass.
//
// Compiled with INLINE_TERRAIN and MODEL_DIRECT_LIGHT, plus INLINE_VS for the
// vertex shader or INLINE_PS with INLINE_SHINE=0/1 for the pixel shaders.
// Native cbuffers stay in their native slots; SteamShadows' Params moves to
// b12 and the floating-origin shift to b13. Shadow maps use t0/t1 and s0/s1,
// which the native world pixel shaders do not use.
// NativeEquivalenceTest compares every native output against these copies.
namespace inlineTerrain {
inline const char *hlsl = R"HLSL(
#ifdef INLINE_TERRAIN
cbuffer InlineShift:register(b13){float4 originShift;}; // frame N world -> shadow-map frame
struct NativeOut{
    float4 p:SV_POSITION;
    float4 color:COLOR0;
    float4 clip:TEXCOORD0;
    float3 viewNormal:TEXCOORD1;
    float3 viewLight:TEXCOORD2;
    float3 viewPos:TEXCOORD3;
};
struct InlineV{
    float4 p:SV_POSITION;
    float4 color:COLOR0;
    float4 clip:TEXCOORD0;
    float3 viewNormal:TEXCOORD1;
    float3 viewLight:TEXCOORD2;
    float3 viewPos:TEXCOORD3;
    float3 w:TEXCOORD4;
    float3 n:TEXCOORD5;
    float local:TEXCOORD6;
    float3 directFraction:TEXCOORD7;
};

#ifdef INLINE_VS
cbuffer NativeProj:register(b0){float4 nProj[4];};
cbuffer NativeView:register(b1){float4 nView[4];};
cbuffer NativeWorld:register(b2){float4 nWorld[4];};
cbuffer NativeMaterial:register(b3){float4 nMaterial;};
cbuffer NativePoints:register(b4){float4 nPointPos[10];float4 nPointColor[10];};
cbuffer NativeLight:register(b5){float4 nFront,nBack,nDirection;};
cbuffer NativeAmbient:register(b6){float4 nAmbient;};
cbuffer NativeWhite:register(b7){float nWhite;};
cbuffer NativeDepthOffset:register(b8){float nDepthOffset;};
cbuffer NativeGlow:register(b9){float nGlow;};

// Native: rows of (view row k) * world, multiply-then-MAD in the same order.
float4 nativeRow(float4 viewRow){
    precise float4 r=viewRow.y*nWorld[1];
    r=mad(nWorld[0],viewRow.x,r);
    r=mad(nWorld[2],viewRow.z,r);
    return mad(nWorld[3],viewRow.w,r);
}
float4 nativeClipRow(float4 projRow,float4 r1,float4 r2,float4 r3,float4 r4){
    precise float4 r=r2*projRow.y;
    r=mad(r1,projRow.x,r);
    r=mad(r3,projRow.z,r);
    return mad(r4,projRow.w,r);
}

// The native world vertex shader. `wn` returns its world normal.
NativeOut nativeVertex(float4 v0,float4 v1,out float3 wn,out float3 worldPos){
    NativeOut o;
    float a=v0.w;
    float3 n=a==0?float3(1,0,0):a==1.0/255?float3(-1,0,0):a==2.0/255?float3(0,1,0)
            :a==3.0/255?float3(0,-1,0):a==4.0/255?float3(0,0,1):float3(0,0,-1);
    float4 r1=nativeRow(nView[0]),r2=nativeRow(nView[1]),r3=nativeRow(nView[2]),r4=nativeRow(nView[3]);
    float4 r5=nativeClipRow(nProj[0],r1,r2,r3,r4),r6=nativeClipRow(nProj[1],r1,r2,r3,r4),
           r7=nativeClipRow(nProj[2],r1,r2,r3,r4),r8=nativeClipRow(nProj[3],r1,r2,r3,r4);
    r7.w=r7.w-nDepthOffset;
    float3 vn=float3(dot(n,r1.xyz),dot(n,r2.xyz),dot(n,r3.xyz));
    o.viewNormal=rsqrt(dot(vn,vn))*vn;
    float3 wraw=float3(dot(n,nWorld[0].xyz),dot(n,nWorld[1].xyz),dot(n,nWorld[2].xyz));
    wn=rsqrt(dot(wraw,wraw))*wraw;
    float facing=max(mad(dot(wn,nDirection.xyz),0.7,0.3),0);
    float orient=mad(abs(dot(wn,float3(0.597022,0.398015,0.696526))),0.5,0.5);
    float3 direct=saturate(mad(facing.xxx,nFront.xyz,orient*nBack.xyz));
    float4 r9=float4(v0.xyz*float3(255,255,255),1);
    float4 clip=float4(dot(r9,r5),dot(r9,r6),dot(r9,r7),dot(r9,r8));
    worldPos=float3(dot(r9,nWorld[0]),dot(r9,nWorld[1]),dot(r9,nWorld[2]));
    o.viewPos=float3(dot(r9,r1),dot(r9,r2),dot(r9,r3));
    float materialAlpha=v1.w*nMaterial.w;
    float3 lit=mad(direct,materialAlpha.xxx,orient*nAmbient.xyz);
    float3 points=0;
    [loop]for(int j=0;j<10;j++){
        if(0<nPointPos[j].w){
            float3 d=worldPos-nPointPos[j].xyz;
            float d2=dot(d,d);
            float f=1-min(d2/(nPointPos[j].w*nPointPos[j].w),1);
            float3 dn=rsqrt(d2)*d;
            float face=min(max(dot(wn,dn)*-4,0.3),1);
            f=f*f;
            points=mad((face*f).xxx,nPointColor[j].xyz,points);
        }
    }
    float3 c=saturate(lit+points);
    float3 vc=v1.xyz+v1.xyz;
    float k=min(max(mad(nAmbient.w-c.y,0.5,1),1),2);
    float lum=dot(float3(0.299,0.587,0.114),vc);
    float t=lum*(1-k);
    float3 tone=min(max(mad(k.xxx,vc,t.xxx),0),2);
    tone=nGlow<0?lum.xxx:tone;
    c=c*nMaterial.xyz;
    c=c*tone;
    float3 glowMix=mad(vc,nMaterial.xyz,-c);
    glowMix=mad(nGlow.xxx,glowMix,c);
    c=0<nGlow?glowMix:c;
    o.color.xyz=saturate(mad(nWhite.xxx,1-c,c));
    o.color.w=v1.w;
    o.viewLight=float3(dot(nDirection.xyz,r1.xyz),dot(nDirection.xyz,r2.xyz),dot(nDirection.xyz,r3.xyz));
    o.p=clip;
    o.clip=clip;
    return o;
}
NativeOut nativeWorldVertex(float4 v0:POSITION,float4 v1:COLOR){
    float3 wn,worldPos;
    return nativeVertex(v0,v1,wn,worldPos);
}

// Native outputs plus the receiver's shadow inputs (same formulas as
// SteamShadows' receiver with MODEL_DIRECT_LIGHT), read from native cbuffers.
InlineV inlineTerrainVertex(float4 v0:POSITION,float4 v1:COLOR){
    float3 wn,worldPos;
    NativeOut o=nativeVertex(v0,v1,wn,worldPos);
    InlineV r;
    r.p=o.p;r.color=o.color;r.clip=o.clip;r.viewNormal=o.viewNormal;r.viewLight=o.viewLight;r.viewPos=o.viewPos;
    float3 a=float3(nWorld[0].x,nWorld[1].x,nWorld[2].x);
    float3 b=float3(nWorld[0].y,nWorld[1].y,nWorld[2].y);
    float3 c=float3(nWorld[0].z,nWorld[1].z,nWorld[2].z);
    float3 n=normal(v0.w);
    float handedness=dot(a,cross(b,c))<0?-1:1;
    r.n=normalize((cross(b,c)*n.x+cross(c,a)*n.y+cross(a,b)*n.z)*handedness);
    float local=0;
    float3 localColor=0;
    [loop]for(int j=0;j<10;j++)if(nPointPos[j].w>0){
        float3 delta=worldPos-nPointPos[j].xyz;
        float d2=dot(delta,delta);
        float falloff=saturate(1-d2/(nPointPos[j].w*nPointPos[j].w));
        float face=clamp(-4*dot(r.n,delta*rsqrt(max(d2,.0001))),.3,1);
        local+=falloff*falloff*face*max(nPointColor[j].r,max(nPointColor[j].g,nPointColor[j].b));
        localColor+=falloff*falloff*face*nPointColor[j].rgb;
    }
    r.local=saturate(local);
    float orientation=.5+.5*abs(dot(r.n,float3(.597022,.398015,.696526)));
    float facing=max(0,.3+.7*dot(r.n,nDirection.xyz));
    float3 back=nBack.rgb*orientation;
    float3 ambient=nAmbient.rgb*orientation+localColor;
    float3 full=saturate(ambient+saturate(back+nFront.rgb*facing)*nMaterial.w);
    float3 indirect=saturate(ambient+saturate(back)*nMaterial.w);
    float protection=saturate(max(nWhite,nGlow));
    r.directFraction=saturate((full-indirect)/max(full,.00001))*(1-protection);
    // Shadow lookups use the frame the shadow maps were rendered in.
    r.w=worldPos+originShift.xyz;
    return r;
}
#endif

#ifdef INLINE_PS
cbuffer Sky:register(b0){float4 skyColor1,skyColor2,fogColor,sunColor;};
cbuffer FogDistance:register(b1){float fogScale;};
cbuffer FogAngle:register(b2){float fogGradientTranslation;};
cbuffer SunPos:register(b3){float4 sunPos;};
cbuffer Alpha:register(b4){float alpha;};
#if INLINE_SHINE
cbuffer Shininess:register(b5){float shininess;};
#endif

float nativeSmooth(float x){float s=x*x;return s*3-dot(float2(s,s),float2(x,x));}

float4 nativeColor(NativeOut i){
    float3 r0=skyColor2.xyz-skyColor1.xyz;
    float2 screen=i.clip.xy/i.clip.ww;
    float height=screen.y+fogGradientTranslation;
#if INLINE_SHINE
    // Native 0x438810.
    r0=mad(nativeSmooth(saturate(mad(height,0.4,0.2))).xxx,r0,skyColor1.xyz);
    float3 horizon=mad(skyColor1.xyz,float3(0.4,0.4,0.4),fogColor.xyz*float3(0.6,0.6,0.6));
    r0=r0-horizon;
    r0=mad(nativeSmooth(saturate(mad(height,2,1.5))).xxx,r0,horizon);
    r0=r0-sunColor.xyz;
    float2 sunDelta=float2((sunPos.x-screen.x)*sunPos.z,sunPos.y-height);
    float wave=mad(screen.x,5,height*10);
    float sun=min(dot(sunDelta,sunDelta)*4,1);
    sun=(1-sun)*sunPos.w;
    sun=sun*sun;
    sun=mad(-sun,sun,1);
    r0=mad(sun.xxx,r0,sunColor.xyz);
    wave=mad(i.viewNormal.x+i.viewNormal.y,2,wave);
    float shimmer=mad(cos(wave),0.3,1);
    float3 base=0<shininess?shimmer*i.color.xyz:i.color.xyz;
    float lum=dot(float3(0.299,0.587,0.114),base);
    float boost=mad(1-lum,0.25,1);
    lum=lum*(1-boost);
    base=mad(boost.xxx,base,lum.xxx);
    float3 toFog=fogColor.xyz-base;
    float d2=dot(i.viewPos,i.viewPos);
    float fog=log2(d2*(fogScale*fogScale));
    fog=fog*mad(-exp2(fogScale*-403.954620),0.75,1);
    fog=min(exp2(fog),1);
    fog=nativeSmooth(fog);
    base=mad(fog.xxx,toFog,base);
    fog=fog*fog;
    fog=fog*fog;
    r0=r0-base;
    r0=saturate(mad(fog.xxx,r0,base));
#else
    // Native 0x43b500.
    float sunX=(sunPos.x-screen.x)*sunPos.z;
    r0=mad(nativeSmooth(saturate(mad(height,0.4,0.2))).xxx,r0,skyColor1.xyz);
    float3 horizon=mad(skyColor1.xyz,float3(0.4,0.4,0.4),fogColor.xyz*float3(0.6,0.6,0.6));
    r0=r0-horizon;
    float lower=saturate(mad(height,2,1.5));
    float2 sunDelta=float2(sunX,sunPos.y-height);
    float sun=min(dot(sunDelta,sunDelta)*4,1);
    sun=(1-sun)*sunPos.w;
    sun=sun*sun;
    sun=mad(-sun,sun,1);
    r0=mad(nativeSmooth(lower).xxx,r0,horizon);
    r0=r0-sunColor.xyz;
    r0=mad(sun.xxx,r0,sunColor.xyz);
    precise float3 base=1/rsqrt(i.color.xyz);
    float lum=dot(float3(0.299,0.587,0.114),base);
    float boost=mad(1-lum,0.25,1);
    lum=lum*(1-boost);
    base=mad(boost.xxx,base,lum.xxx);
    r0=r0-base;
    float fog=saturate(dot(i.viewPos,i.viewPos)*(fogScale*fogScale));
    fog=mad(-(fog*fog),fog,1);
    fog=mad(-fog,fog,1);
    r0=saturate(mad(fog.xxx,r0,base));
#endif
    float grey=dot(float3(0.299,0.587,0.114),r0)*0.35;
    r0.yz=mad(r0.yz,float2(0.65,0.65),grey.xx);
    float3 sq=r0*r0;
    float3 cube=r0*sq;
    cube=cube+cube;
    return float4(mad(sq,float3(3,3,3),-cube),alpha);
}
float4 nativeWorldPixel(NativeOut i):SV_TARGET{return nativeColor(i);}

// Native colour with the receiver's shadow term applied in place.
float4 inlineTerrainPixel(InlineV i):SV_TARGET{
    NativeOut n;
    n.p=i.p;n.color=i.color;n.clip=i.clip;n.viewNormal=i.viewNormal;n.viewLight=i.viewLight;n.viewPos=i.viewPos;
    float4 native=nativeColor(n);
    V v;
    v.p=i.p;v.w=i.w;v.n=i.n;v.local=i.local;v.directFraction=i.directFraction;
    ModelOutput s=visibility(v);
    return float4(native.rgb*s.transmission.rgb+s.color.rgb,native.a);
}
#endif
#endif
)HLSL";
} // namespace inlineTerrain
