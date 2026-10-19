// Based on doom-eternal-collision-viewer by IlyaTsyganov (commit 43ac57d), used with the author's permission.
// After editing, regenerate ../shaders_spirv.h: python scripts/embed_shaders.py (see docs/BUILDING.md).
#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec4 color;
layout(location=0) out vec4 vertexColor;
layout(location=1) out float viewDistance;
layout(push_constant) uniform Camera { vec4 row0; vec4 row1; vec4 row2; vec4 row3; vec4 options; vec4 depthParams; vec4 uvParams; vec4 debug; } camera;
void main() {
    if(camera.debug.x>0.5) { gl_Position=vec4(position.xy,0.0,1.0); vertexColor=vec4(0); viewDistance=1; return; }
    vec4 p=vec4(position,1.0);
    float x=dot(camera.row0,p);
    float y=dot(camera.row1,p);
    float w=dot(camera.row3,p);
    // Infinite far plane: distance selects whole objects on the CPU, not their fragments.
    float z=w-0.05;
    gl_Position=vec4(x,-y,z,w);
    vertexColor=color;
    viewDistance=w;
}
