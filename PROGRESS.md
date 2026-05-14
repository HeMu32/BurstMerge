# BurstMerge — Port Progress & Complete Architecture

## Goal
Port the core RAW photo merge/HDR algorithm of hdr-plus-swift to GNU toolchain (gcc/MinGW), outputting corrected DNG files with proper embedded distortion correction data recognized by Capture One.

## Constraints & Preferences
- Focus on algorithm-level abstraction, not literal Metal→GLSL translation
- Exclude GUI (SwiftUI/AppKit) and Metal runtime dependencies
- Core pipeline: RAW decode → lens correction → align → merge → exposure correct → output DNG
- Target platforms: Windows (MinGW) and Linux (gcc) with Vulkan compute
- No Python environment available; all tooling via PowerShell/CMD/exiftool
- Capture One does **not** support Adobe LCP format — this is a hard constraint
- Pixel-level distortion re-rendering into DNG is rejected — must use DNG metadata/opcode approach

## Progress — Distortion Correction Injection (Completed ✅)

### Key Discovery
Capture One reads DNG-standard `WarpRectilinear` OpCode from `OpcodeList3` for distortion correction — NOT Sony MakerNote fields or Adobe `DistortionCorrParams`.

### Final Working Algorithm: Sony Spline → WarpRectilinear

**Source**: Sony MakerNote tag `0x9416` → `DistortionCorrParams` = 16 signed int16 values

**Reference**: [Stannum blog: Sony ARW distortion correction](https://stannum.io/blog/0PwljB) — Sony coefficients are **spline knots**, not polynomial coefficients.

**Conversion steps**:

```
1. Read 16 int16 knot values sv[0..15] from Sony MakerNote (DistortionCorrParams)
   Positions: r_i = i / 15  for i = 0..15

2. Compute scale factor s = 1 + sv[15] / 16384

3. For each knot i, compute WarpRectilinear g(r) at knot position:
   g(r_i) = (1 + sv[i] / 16384) / s

4. Fit 3rd-order polynomial in r² to the 16 data points:
   g(r) = kr0 + kr1·r² + kr2·r⁴ + kr3·r⁶
   using least squares → yields [kr0, kr1, kr2, kr3]

5. Build 88-byte OpcodeList3 binary (big-endian):
   [numOps=1][opID=1(WarpRectilinear)][ver=1.3.0.0][flags=1(optional)][paramBytes=68]
   [N=1][kr0][kr1][kr2][kr3][kt0=0][kt1=0][cx=0.5][cy=0.5]

6. Inject via TIFF direct patch:
   - Parse DNG → IFD0 → tag 0x014A (SubIFDs) → sub-IFD at 0x31990
   - SubIFD entry[26] = tag 0xC74E (OpcodeList3) → data at 0x33906, oldSize=184
   - Overwrite data with new 88-byte binary, update count field, write back
```

**Result (for FE 200-600mm F5.6-6.3 @ 600mm)**:
```
kr0 = 0.97345385
kr1 = +0.02867752    ← kr1 > 0 → C1 interprets as barrel (corrects pincushion)
kr2 = -0.00214369
kr3 = +0.00003387
Max correction: ~57 pixels inward at r ≈ 0.6 (mustache profile)
```

**Key to C1 compatibility**: C1 determines correction direction from the **sign of kr1** (the r² coefficient):
- `kr1 > 0` → barrel distortion (corrects pincushion) ✅
- `kr1 < 0` → pincushion distortion (makes it worse) ❌

### Attempts Log (Unreliable / Superseded)

| Attempt | Method | kr1 | C1 Result | Status |
|---------|--------|-----|-----------|--------|
| **test2** | LCP inverted (g(r) > 1) | -0.0257 | pincushion (wrong) | ❌ Unreliable |
| **test3** | LCP direct (g(r) < 1) | +0.0240 | barrel (correct) | ⚠️ Weak correction |
| **sony_raw** | Sony values as raw +/- offsets | -0.0097 / +0.0097 | wrong polarity / weak | ❌ Unreliable |
| **sony_spline** | Sony spline formula (16384 scaling + s normalization) | **+0.0287** | **barrel, strong** | ✅ **Final** |

---

# Appendix: C/C++ Implementation Reference

This section provides the complete mathematical derivation and data structures needed for a C/C++ reimplementation of the lens correction injection algorithm.

## 1. Sony Parameter Formats

### 1.1 DistortionCorrParams (tag 0x9416)

Sony stores lens distortion correction as **16 signed 16-bit integer knot values** defining a 15-segment linear spline. The knots are uniformly distributed along the normalized radius `r ∈ [0, 1]`.

**ARW raw format** (17 int16 values):
```
[count=16, knot[0], knot[1], ..., knot[15]]
                ↑ count prefix (skip this)
```

**DNG (Adobe-converted) format** (16 int16 values, no prefix):
```
[knot[0], knot[1], ..., knot[15]]
```

Both represent the same 16 knots. For ARW, the count prefix must be removed.

### 1.2 VignettingCorrParams (same tag, different sub-field)

32 int16 values, but only the first 16 are meaningful:
```
ARW [count=16, 0, 272, 640, ..., 11488]
DNG [272, 640, ..., 11488, 0, 0, 0, ...]
```

The verified algorithm uses **15 of 16 values** (skipping the trailing zero in DNG format, or equivalently using the first 15 of 16 ARW values after removing the count prefix and the first `0` knot).

---

## 2. Distortion: Sony Spline → WarpRectilinear

### 2.1 Spline → Data Points

Given 16 knot values `sv[0..15]`:

```
For i = 0 to 15:
    r_i     = i / 15.0                       // normalized radius
    x_i     = r_i²                           // polynomial variable
    s       = 1 + sv[15] / 16384             // normalization scale
    g(r_i)  = (1 + sv[i] / 16384) / s        // target at knot position
```

The factor `16384 = 2¹⁴` is from Sony's encoding.

### 2.2 Polynomial Model

Fit a 3rd-order polynomial in `x = r²`:

```
g(r) = kr0 + kr1·x + kr2·x² + kr3·x³    where x = r²
or equivalently:
g(r) = kr0 + kr1·r² + kr2·r⁴ + kr3·r⁶
```

### 2.3 Least-Squares Fit (Normal Equations)

For data points `(x_i, y_i)` with `y_i = g(r_i)`, solve:

```
X'X · k = X'y

where:
X[i] = [1, x_i, x_i², x_i³]
k    = [kr0, kr1, kr2, kr3]ᵀ
y_i  = g(r_i)
```

#### Design Matrix X (16×4):

```
     ┌                        ┐
     │ 1   x₀   x₀²   x₀³   │
     │ 1   x₁   x₁²   x₁³   │
X =  │ 1   x₂   x₂²   x₂³   │
     │ ...                   │
     │ 1   x₁₅  x₁₅²  x₁₅³  │
     └                        ┘
```

#### Normal Equations Matrix X'X (4×4 symmetric):

```
X'X[i][j] = Σₙ  xₙⁱ · xₙʲ    for i,j ∈ {0,1,2,3}

where xₙ⁰ = 1, xₙ¹ = xₙ, xₙ² = xₙ², xₙ³ = xₙ³
```

**Explicit element values** (for 16 points at r = i/15):
```
X'X[0][0] = Σ 1      = 16
X'X[0][1] = Σ r²     = Σ x
X'X[0][2] = Σ r⁴     = Σ x²
X'X[0][3] = Σ r⁶     = Σ x³
X'X[1][1] = Σ r⁴     = Σ x²
X'X[1][2] = Σ r⁶     = Σ x³
X'X[1][3] = Σ r⁸     = Σ x⁴
X'X[2][2] = Σ r⁸     = Σ x⁴
X'X[2][3] = Σ r¹⁰    = Σ x⁵
X'X[3][3] = Σ r¹²    = Σ x⁶
```

#### RHS Vector X'y (4×1):
```
X'y[j] = Σₙ  xₙʲ · yₙ
```

### 2.4 Gaussian Elimination for 4×4 System

Solve `A · k = b` (where `A = X'X`, `b = X'y`) using partial pivot:

```
Augmented matrix M (4×5):
┌                                              ┐
│ A[0][0] A[0][1] A[0][2] A[0][3] │ b[0]    │
│ A[1][0] A[1][1] A[1][2] A[1][3] │ b[1]    │
│ A[2][0] A[2][1] A[2][2] A[2][3] │ b[2]    │
│ A[3][0] A[3][1] A[3][2] A[3][3] │ b[3]    │
└                                              ┘
```

**C/Pseudocode**:
```c
double M[4][5];
// Fill M[i][j] = X'X[i][j], M[i][4] = X'y[i]

for (int c = 0; c < 4; c++) {
    // Partial pivot
    int mr = c;
    for (int r = c+1; r < 4; r++)
        if (fabs(M[r][c]) > fabs(M[mr][c])) mr = r;
    // Swap rows
    if (mr != c) for (int j = c; j <= 4; j++) swap(M[c][j], M[mr][j]);
    // Eliminate
    double pv = M[c][c];
    for (int r = c+1; r < 4; r++) {
        double f = M[r][c] / pv;
        for (int j = c; j <= 4; j++) M[r][j] -= f * M[c][j];
    }
}
// Back-substitute
double k[4];
for (int i = 3; i >= 0; i--) {
    double s = M[i][4];
    for (int j = i+1; j < 4; j++) s -= M[i][j] * k[j];
    k[i] = s / M[i][i];
}
```

### 2.5 DNG Spec: WarpRectilinear Application

The `WarpRectilinear` opcode (ID=1) is applied by the DNG reader to map corrected pixel positions back to source positions:

```
For each output pixel (x, y) of the corrected image:
    dx = (x - cx) / m_x
    dy = (y - cy) / m_y
    r  = sqrt(dx² + dy²)             // normalized radius [0,1]
    f  = kr0 + kr1·r² + kr2·r⁴ + kr3·r⁶
    
    // Radial distortion correction
    x' = cx + m_x · (dx · f + tangential_x)
    y' = cy + m_y · (dy · f + tangential_y)
    
    // Sample source image at (x', y')
```

Where:
- `(cx, cy)` = normalized optical center (=0.5 for centered lens)
- `m_x = max(|x₀-cx|, |x₁-cx|)`, `m_y = max(|y₀-cy|, |y₁-cy|)` = max pixel distances from center
- `x₀,y₀` = top-left pixel, `x₁,y₁` = bottom-right pixel
- `kt0, kt1` = tangential coefficients (=0 for current use)

---

## 3. Vignette: Sony Spline → FixVignetteRadial

### 3.1 Spline → Data Points

Given 16 knot values from VignettingCorrParams:

1. **ARW format**: [count=16, 0, 272, 640, ..., 11488] → skip count, take values[1..15] = [272, 640, ..., 11488, 0]
2. **DNG format**: [272, 640, ..., 11488, 0, 0, 0...] → take first 16

Actually, the verified algorithm uses **15 of 16 values**, skipping the 16th (which is 0 padding):

```
n = 15
base = knot[0]               // e.g. 272
For i = 0 to 14:
    r_i     = i / 14.0                     // r ranges 0 to 1
    diff    = knot[i] - base               // baseline-subtracted
    gain(r) = 1.0 + diff / 16384           // correction gain
    y_i     = gain(r) - 1.0                // gain offset (no constant term in fit)
```

### 3.2 Polynomial Model

Fit without constant term: `gain(r) - 1 = k₀·r² + k₁·r⁴ + k₂·r⁶ + k₃·r⁸ + k₄·r¹⁰`

Note: The DNG spec's `FixVignetteRadial` model is:
```
gain(r) = 1 + k₀·r² + k₁·r⁴ + k₂·r⁶ + k₃·r⁸ + k₄·r¹⁰
```

### 3.3 Least-Squares Fit

Design columns: `v(r) = [r², r⁴, r⁶, r⁸, r¹⁰]`

Normal equations `X'X · k = X'y` where:
- `X[i][j] = v_j(r_i)` for knot i, predictor j
- `X'X[p][q] = Σᵢ v_p(r_i) · v_q(r_i)`
- `X'y[p] = Σᵢ v_p(r_i) · y_i`

Solve 5×5 system with Gaussian elimination (same algorithm as 4×4, extended to 5×6 augmented matrix).

### 3.4 DNG Spec: FixVignetteRadial Application

```
For each pixel (x, y) at normalized distance r from optical center:
    gain = 1 + k₀·r² + k₁·r⁴ + k₂·r⁶ + k₃·r⁸ + k₄·r¹⁰
    I_corrected(x,y) = gain · I_original(x,y)
```

---

## 4. OpcodeList3 Binary Format

All multi-byte values are **big-endian** regardless of DNG byte order.

### 4.1 Opcode List Header

```
Offset  Bytes  Field
0       4     numOpcodes (uint32)
```

### 4.2 Per-Opcode Header (16 bytes each)

```
Offset  Bytes  Field
0       4     opcodeID (uint32)
4       4     dngVersion (uint32, e.g. 0x01030000 = 1.3.0.0)
8       4     flags (uint32, bit0=1 → optional)
12      4     paramBytes (uint32)
```

### 4.3 WarpRectilinear (OpcodeID=1) Parameters

```
Offset  Bytes  Field
0       4     N (uint32) — number of coefficient sets
4       8×6   Set[0]: kr0, kr1, kr2, kr3, kt0, kt1 (IEEE 754 doubles)
...     8×6   Set[N-1]...
...     8×2   cx, cy (doubles) — shared optical center (typically 0.5, 0.5)
```

Total params: `4 + N×48 + 16` bytes. For N=1: 68 bytes.
Total opcode with header: `16 + 68 = 84` bytes.

**Double encoding**: IEEE 754 64-bit big-endian:
```c
void write_double_be(uint8_t* buf, double val) {
    uint64_t bits; memcpy(&bits, &val, 8);
    for (int i = 0; i < 8; i++)
        buf[i] = (bits >> (56 - i*8)) & 0xFF;
}
```

### 4.4 FixVignetteRadial (OpcodeID=3) Parameters

```
Offset  Bytes  Field
0       8     k0 (double)
8       8     k1 (double)
16      8     k2 (double)
24      8     k3 (double)
32      8     k4 (double)
40      8     cx (double) — optical center x
48      8     cy (double) — optical center y
```

Total params: 56 bytes. Total opcode with header: 72 bytes.

### 4.5 Combined OpcodeList3 Example (160 bytes)

With both WarpRectilinear and FixVignetteRadial:

```
[4B]  numOps = 2

--- WarpRectilinear (84 bytes) ---
[4B]  opcodeID = 1
[4B]  version = 0x01030000
[4B]  flags = 1
[4B]  paramBytes = 68
[4B]  N = 1
[8B]  kr0
[8B]  kr1
[8B]  kr2
[8B]  kr3
[8B]  kt0 = 0.0
[8B]  kt1 = 0.0
[8B]  cx = 0.5
[8B]  cy = 0.5

--- FixVignetteRadial (72 bytes) ---
[4B]  opcodeID = 3
[4B]  version = 0x01030000
[4B]  flags = 1
[4B]  paramBytes = 56
[8B]  k0
[8B]  k1
[8B]  k2
[8B]  k3
[8B]  k4
[8B]  cx = 0.5
[8B]  cy = 0.5
```

---

## 5. TIFF/DNG Injection

### 5.1 IFD Structure (TIFF)

```
Header (8 bytes):
  [2B] byte order: 0x4949 = LE, 0x4D4D = BE
  [2B] magic: 0x002A = 42
  [4B] offset to first IFD

IFD Entry (12 bytes each):
  [2B] tag (uint16)
  [2B] type (uint16) — 4=LONG, 7=UNDEFINED, 16=IFD
  [4B] count (uint32)
  [4B] value/offset (uint32) — inline if fits 4B, else pointer
```

### 5.2 Finding OpcodeList3 (tag 0xC74E)

```
1. Read IFD0 at offset from header
2. Find tag 0x014A (SubIFDs) in IFD0:
   - type=LONG or IFD, count=N
   - If count==1: value is SubIFD offset directly
   - If count>1: value is pointer to array of N offsets
3. Parse SubIFD at that offset
4. Find tag 0xC74E (OpcodeList3) in SubIFD:
   - type=UNDEFINED (7)
   - count = data size in bytes (must be ≥ new opcode size)
   - value = offset to opcode binary data in file
```

### 5.3 Replacing OpcodeList3 Data

```c
// Read entire DNG into memory
uint8_t* dng = read_file(path, &filesize);
uint8_t* new_op = build_opcode_list3(kr, kv, has_vignette, &op_size);

// Find IFD entry for tag 0xC74E, read its value/offset as data_ptr
uint32_t data_ptr = ...;  // extracted from IFD entry
uint32_t old_size = ...;  // extracted from IFD entry count

// Verify new size fits
assert(op_size <= old_size);

// Overwrite
memcpy(dng + data_ptr, new_op, op_size);
if (op_size < old_size)
    memset(dng + data_ptr + op_size, 0, old_size - op_size);

// Update count field in IFD entry (at entry_offset + 4)
*(uint32_le*)(dng + entry_offset + 4) = op_size;

// Write back
write_file(path, dng, filesize);
```

---

## Pipeline Implementation

### Completed Infrastructure
- Full structural analysis: 7 modules, 52 unique compute kernels
- Metal shader → Vulkan compute mapping with binding tables
- Vulkan descriptor layout (3-set), push constants, SSBOs
- Metal→GLSL compatibility macro layer specified
- Validated Adobe DNG Converter 16.4 CLI on Windows
- DNG Opcode binary format fully documented

## Relevant Files
### Working (active) scripts
- **`SyncARWLensCorr.ps1`** (项目根目录) — **最终交付**：从 Sony ARW/DNG 提取畸变+暗角参数，注入目标 DNG 的 OpcodeList3
  - 支持 ARW（17值含计数前缀）和 DNG（16值无前缀）两种参考文件格式
  - 同时注入 WarpRectilinear（畸变）+ FixVignetteRadial（暗角）两个 OpCode
  - 写入方式：TIFF IFD 动态扫描 + 直接补丁
- `R:\build_vignette.ps1` — 验证过的注入流程参考
- `R:\sony_spline_to_wr.ps1` — Sony 16-knot spline → WarpRectilinear 系数转换验证
- `R:\tiff_patch3.ps1` — TIFF OFD解析 + OpcodeList3 写入原型

### Reference material
- `Reference\DNG_Spec_1_7_1_0\DNG_Spec_1_7_1_0.md` — WarpRectilinear spec
- `Reference\sony-distortion-corrector\` — Sony ARW distortion corrector (open source reference, empty)

### Test files
- `R:\DSC09300_sony.dng` — Sony spline → WarpRectilinear injection ✅
- `R:\DSC09300_test2.dng` — (LCP inverted, unreliable)
- `R:\DSC09300_test3.dng` — (LCP direct, correct direction but weak)
- `R:\DSC09300_test.dng` — clean copy of original Adobe DNG

## PS1 Script Caveats (PowerShell 5.1 Compatibility)
- **大小写敏感 BUG**: PowerShell 变量名不区分大小写，`$X`/`$x`、`$Y`/`$y`、`$M`/`$m` 视为同一变量导致数组被覆盖
- **修复**: 全部变量使用纯小写；畸变法方程用标量累加器代替数组 `+=` 操作
- **throw 空格**: `throw"msg"` 在某些环境中解析为命令 `throw"msg"`，需写为 `throw "msg"`
- **数组创建**: `New-Object double[] N` 可能失效，改用 `@(0.0)*N`

## Key Decisions
- **DNG I/O**: Adobe DNG SDK for write path. exiftool for field-level patches.
- **No LCP embedding**: C1 incompatible with Adobe LCP format.
- **No pixel-level distortion**: Bayer-domain re-rendering rejected (degrades resolution).
- **Bayer-only pipeline**: No demosaic; output remains Bayer CFA.
- **WhiteLevel**: keep DNG default 16380.
- **OpcodeList3 injection**: TIFF direct patch (exiftool cannot write SubIFD tags).
- **C1 direction detection**: C1 uses kr1 sign to determine barrel vs pincushion.

## Next Steps
1. Integrate distortion correction into merge pipeline:
   - At output: extract Sony MakerNote DistortionCorrParams (16 values)
   - Convert to WarpRectilinear kr0-kr3 via spline formula
   - Inject into OpcodeList3 of output DNG
2. Begin Vulkan compute + libraw + libtiff implementation
3. Validate final output in Capture One

## Known Limitations
Current vignette parameters translation result (from ARW to DNG) mismatch with original ARW files in Capture One, but another known fact was that C1's explanation of ARW burnt-in vignette correction parameters perform awfully. 
Perhaps still minor difference for the effect of the translated distortion parameters vs the original file in C1. However not visible for the 200-600 G lens sample picture.
