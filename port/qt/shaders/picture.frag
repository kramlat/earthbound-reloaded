#version 440
// The picture's texel, opaque. The texture is twice the picture's height: a plain frame fills the upper half; a smooth-camera frame has the scenery there and the party's pixels (alpha 0 elsewhere) in the lower half.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
layout(std140, binding = 0) uniform Frame {
    mat4 correction;
    vec4 shift;
    vec4 mode;
};
layout(binding = 1) uniform sampler2D picture;
void main() {
    if (mode.x < 0.5) {
        colour = vec4(texture(picture, vec2(uv.x, uv.y * 0.5)).rgb, 1.0);
        return;
    }
    vec2 size = shift.zw;
    vec2 p = uv * size;
    // the scenery moves left and up by the leader's sub-pixel fraction; the edge it uncovers shows the neighbouring row and column
    vec2 sp = clamp(p + shift.xy, vec2(0.0), size - vec2(0.001));
    vec4 scenery = texture(picture, vec2(sp.x / size.x, sp.y / (2.0 * size.y)));
    vec4 party = texture(picture, vec2(p.x / size.x, (size.y + p.y) / (2.0 * size.y)));
    colour = vec4(party.a > 0.5 ? party.rgb : scenery.rgb, 1.0);
}
