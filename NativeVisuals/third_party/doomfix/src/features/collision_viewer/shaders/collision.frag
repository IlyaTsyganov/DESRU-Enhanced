// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
// After editing, regenerate ../shaders_spirv.h: python scripts/embed_shaders.py (see docs/BUILDING.md).
#version 450
layout(location=0) in vec4 vertexColor;
layout(location=1) in float viewDistance;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D screenDepth;
layout(push_constant) uniform Camera { vec4 row0; vec4 row1; vec4 row2; vec4 row3; vec4 options; vec4 depthParams; vec4 uvParams; vec4 debug; } camera;
vec4 surfaceDistance(vec4 sampled) {
    vec4 ndc=(sampled-camera.depthParams.x)/(camera.depthParams.y-camera.depthParams.x);
    if(camera.depthParams.z==1.0) ndc=ndc*2.0-1.0;
    if(camera.depthParams.z==2.0) ndc=1.0-ndc;
    return camera.options.z/(ndc-camera.options.y);
}
void main() {
    if(camera.debug.x>0.5) {
        uint bits=floatBitsToUint(texture(screenDepth,gl_FragCoord.xy*camera.uvParams.xy+camera.uvParams.zw).r);
        outColor=vec4(uvec4(bits,bits>>8,bits>>16,bits>>24)&255u)/255.0;
        return;
    }
    if(camera.options.w>0.5) {
        vec2 uv=gl_FragCoord.xy*camera.uvParams.xy+camera.uvParams.zw;
        float sampled=texture(screenDepth,uv).r;
        float distanceToSurface=surfaceDistance(vec4(sampled)).x;
        // Depth is sampled on the game's (possibly scaled and jittered) pixel grid. On a
        // coplanar slope one nearest texel alternately hides the overlay. Use the 2x2 footprint,
        // but reject neighbours across a depth discontinuity so foreground walls still occlude.
        vec4 neighbours=surfaceDistance(textureGather(screenDepth,uv,0));
        // A percentage alone rejects legitimate neighbours on grazing surfaces. Bound the
        // footprint by the overlay plane's screen-space depth gradient, not the depth image's
        // discontinuity (which could otherwise admit the background across a foreground wall).
        vec2 depthTexelPixels=1.0/max(abs(camera.uvParams.xy)*vec2(textureSize(screenDepth,0)),vec2(1e-8));
        float footprint=2.0*dot(abs(vec2(dFdx(viewDistance),dFdy(viewDistance))),depthTexelPixels);
        float limit=distanceToSurface+camera.depthParams.w+max(max(distanceToSurface,0.0)*0.03,footprint);
        for(int i=0;i<4;++i)
            if(neighbours[i]>distanceToSurface && neighbours[i]<=limit) distanceToSurface=neighbours[i];
        float tolerance=camera.depthParams.w+max(distanceToSurface,0.0)*0.001;
        if(distanceToSurface>0.0&&viewDistance>distanceToSurface+tolerance) discard;
    }
    outColor=vertexColor;
}
