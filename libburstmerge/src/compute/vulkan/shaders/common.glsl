// common.glsl - included after #version/#extension in each shader.
// Provides the shared push-constant block and small helpers.

layout(push_constant) uniform PC
{
    int   w;            // primary image width
    int   h;            // primary image height
    int   channels;     // primary image channel count
    int   w2;           // secondary image width  (or dst width)
    int   h2;           // secondary image height (or dst height)
    int   channels2;    // secondary image channel count
    int   i0;           // generic int slot 0
    int   i1;           // generic int slot 1
    int   i2;           // generic int slot 2
    int   i3;           // generic int slot 3
    int   i4;           // generic int slot 4
    int   i5;           // generic int slot 5
    int   i6;           // generic int slot 6
    int   i7;           // generic int slot 7
    int   i8;           // generic int slot 8
    int   i9;           // generic int slot 9
    float f0;           // generic float slot 0
    float f1;           // generic float slot 1
    float f2;           // generic float slot 2
    float f3;           // generic float slot 3
    float f4;           // generic float slot 4
    float f5;           // generic float slot 5
    float f6;           // generic float slot 6
    float f7;           // generic float slot 7
} pc;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
