#pragma once
/* d3dx9math shim for the WASM build.
 *
 * The decompiled tree uses D3DX math as its linear-algebra library throughout game logic
 * (aiming, cameras, vehicle and bone math), so this is a functional implementation, not a stub.
 * Signatures mirror the real Direct3D 9 D3DX API exactly: out-pointer first, in-pointers for
 * operands, returned out-pointer (the tree calls e.g.
 *   D3DXMatrixMultiply(&aim, &aim, D3DXMatrixRotationY(&yaw, azim))
 * which only type-checks against the real pointer-based prototype).
 * Convention: row-major matrices, row-vector transform (v' = v * M), as in D3DX.
 */
/* This header models C++ COM/D3D APIs: never include it from C. */
#ifdef __cplusplus  /* C++ only */
#include <math.h>
#include <string.h>

/* Plain D3D9 vector types, declared before the D3DX wrappers: game code assigns between the two
 * (MSVC x86 treated them as layout-identical but distinct types). */
struct D3DVECTOR  { float x, y, z; };
struct D3DVECTOR2 { float x, y; };

struct D3DXVECTOR2 { float x, y; };
struct D3DXVECTOR3 {
  float x, y, z;
  D3DXVECTOR3() : x(0), y(0), z(0) {}
  D3DXVECTOR3(float a, float b, float c) : x(a), y(b), z(c) {}
  explicit D3DXVECTOR3(const float* a) : x(a[0]), y(a[1]), z(a[2]) {}
  /* MSVC x86 recorded D3DVECTOR and D3DXVECTOR3 as distinct but layout-identical;
   * game code converts between them with reinterpret_cast and with plain assignment. */
  D3DXVECTOR3(const D3DVECTOR& v) : x(v.x), y(v.y), z(v.z) {}
  D3DXVECTOR3& operator=(const D3DVECTOR& v) { x = v.x; y = v.y; z = v.z; return *this; }
  operator D3DVECTOR() const { D3DVECTOR v = { x, y, z }; return v; }
  bool operator==(const D3DXVECTOR3& r) const { return x == r.x && y == r.y && z == r.z; }
  bool operator!=(const D3DXVECTOR3& r) const { return !(*this == r); }
  D3DXVECTOR3 operator+(const D3DXVECTOR3& r) const { return D3DXVECTOR3(x + r.x, y + r.y, z + r.z); }
  D3DXVECTOR3 operator-(const D3DXVECTOR3& r) const { return D3DXVECTOR3(x - r.x, y - r.y, z - r.z); }
  D3DXVECTOR3 operator-() const { return D3DXVECTOR3(-x, -y, -z); }
  D3DXVECTOR3 operator*(float f) const { return D3DXVECTOR3(x * f, y * f, z * f); }
  D3DXVECTOR3 operator/(float f) const { return D3DXVECTOR3(x / f, y / f, z / f); }
  D3DXVECTOR3& operator+=(const D3DXVECTOR3& r) { x += r.x; y += r.y; z += r.z; return *this; }
  D3DXVECTOR3& operator-=(const D3DXVECTOR3& r) { x -= r.x; y -= r.y; z -= r.z; return *this; }
  D3DXVECTOR3& operator*=(float f) { x *= f; y *= f; z *= f; return *this; }
  D3DXVECTOR3& operator/=(float f) { x /= f; y /= f; z /= f; return *this; }
  operator float*() { return &x; }
  operator const float*() const { return &x; }
};
struct D3DXVECTOR4 {
  float x, y, z, w;
  D3DXVECTOR4() : x(0), y(0), z(0), w(0) {}
  D3DXVECTOR4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
  explicit D3DXVECTOR4(const float* a) : x(a[0]), y(a[1]), z(a[2]), w(a[3]) {}
  bool operator==(const D3DXVECTOR4& r) const { return x == r.x && y == r.y && z == r.z && w == r.w; }
  bool operator!=(const D3DXVECTOR4& r) const { return !(*this == r); }
  D3DXVECTOR4 operator+(const D3DXVECTOR4& r) const { return D3DXVECTOR4(x + r.x, y + r.y, z + r.z, w + r.w); }
  D3DXVECTOR4 operator-(const D3DXVECTOR4& r) const { return D3DXVECTOR4(x - r.x, y - r.y, z - r.z, w - r.w); }
  D3DXVECTOR4 operator*(float f) const { return D3DXVECTOR4(x * f, y * f, z * f, w * f); }
  operator float*() { return &x; }
  operator const float*() const { return &x; }
};
#ifndef SWBF2_D3DMATRIX_DEFINED
#define SWBF2_D3DMATRIX_DEFINED
struct D3DMATRIX {
  union {
    float m[4][4];
    // The named fields must live in a nested struct. Declaring them directly in the union - as
    // this file used to - makes each of _11.._44 its own union member, so all sixteen are another
    // name for m[0][0]. Layout is row-major, matching Microsoft's D3DMATRIX (_11.._44 and m[4][4]).
    struct {
      float _11, _12, _13, _14,
            _21, _22, _23, _24,
            _31, _32, _33, _34,
            _41, _42, _43, _44;
    };
  };
};
#endif
struct D3DXMATRIX : public D3DMATRIX {
  D3DXMATRIX() { memset(m, 0, sizeof(m)); _11 = _22 = _33 = _44 = 1.0f; }
  D3DXMATRIX(const D3DMATRIX& rhs) { memcpy(m, rhs.m, sizeof(m)); }
  D3DXMATRIX(const float* pF) { memcpy(m, pF, sizeof(m)); }
  D3DXMATRIX(float a00, float a01, float a02, float a03,
             float a10, float a11, float a12, float a13,
             float a20, float a21, float a22, float a23,
             float a30, float a31, float a32, float a33) {
    _11 = a00; _12 = a01; _13 = a02; _14 = a03;
    _21 = a10; _22 = a11; _23 = a12; _24 = a13;
    _31 = a20; _32 = a21; _33 = a22; _34 = a23;
    _41 = a30; _42 = a31; _43 = a32; _44 = a33;
  }
  float& operator()(int r, int c) { return m[r][c]; }
  float  operator()(int r, int c) const { return m[r][c]; }
};
/* Microsoft declares D3DXMATRIXA16 as a *derived* matrix type (an alignment promise, not a
 * different layout): D3DXMATRIXA16 : public D3DXMATRIX, constructible from 16 floats, from a
 * D3DXMATRIX or from a float array, and implicitly convertible back. Making it a plain typedef
 * broke pcRedSegment.cpp / pcRedStencilShadow.cpp, which build a `static const D3DXMATRIXA16
 * _IdentityMatrix(16 floats)` and pass D3DXMATRIXA16 objects to the D3DX* functions that take
 * D3DXMATRIX pointers (that conversion is a base-pointer conversion here). */
struct _D3DXMATRIXA16 : D3DXMATRIX {
  _D3DXMATRIXA16() : D3DXMATRIX() {}
  _D3DXMATRIXA16(float a00, float a01, float a02, float a03,
                 float a10, float a11, float a12, float a13,
                 float a20, float a21, float a22, float a23,
                 float a30, float a31, float a32, float a33) {
    _11 = a00; _12 = a01; _13 = a02; _14 = a03;
    _21 = a10; _22 = a11; _23 = a12; _24 = a13;
    _31 = a20; _32 = a21; _33 = a22; _34 = a23;
    _41 = a30; _42 = a31; _43 = a32; _44 = a33;
  }
  _D3DXMATRIXA16(const D3DXMATRIX& rhs) { memcpy(m, rhs.m, sizeof(m)); }
  _D3DXMATRIXA16(const float* pF) { memcpy(m, pF, sizeof(m)); }
  _D3DXMATRIXA16(float f) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) m[r][c] = f; }
  _D3DXMATRIXA16& operator=(const D3DXMATRIX& rhs) {
    memcpy(m, rhs.m, sizeof(m));
    return *this;
  }
};
typedef _D3DXMATRIXA16 D3DXMATRIXA16;

struct D3DXQUATERNION {
  float x, y, z, w;
  D3DXQUATERNION() : x(0), y(0), z(0), w(0) {}
  D3DXQUATERNION(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

struct D3DXCOLOR {
  float r, g, b, a;
  D3DXCOLOR(const D3DXVECTOR3& v) : r(v.x), g(v.y), b(v.z), a(1.0f) {}
  D3DXCOLOR(const D3DXVECTOR4& v) : r(v.x), g(v.y), b(v.z), a(v.w) {}
  D3DXCOLOR& operator=(const D3DXVECTOR3& v) { r = v.x; g = v.y; b = v.z; a = 1.0f; return *this; }
  D3DXCOLOR& operator=(const D3DXVECTOR4& v) { r = v.x; g = v.y; b = v.z; a = v.w; return *this; }
  D3DXCOLOR() : r(0), g(0), b(0), a(0) {}
  D3DXCOLOR(float rr, float gg, float bb, float aa) : r(rr), g(gg), b(bb), a(aa) {}
};
typedef D3DXCOLOR D3DCOLORVALUE;
typedef unsigned int D3DCOLOR;

/* ---------------- matrices ---------------- */
inline D3DXMATRIX* D3DXMatrixIdentity(D3DXMATRIX* pOut) {
  memset(pOut->m, 0, sizeof(pOut->m));
  pOut->_11 = pOut->_22 = pOut->_33 = pOut->_44 = 1.0f;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixMultiply(D3DXMATRIX* pOut, const D3DXMATRIX* pA, const D3DXMATRIX* pB) {
  D3DXMATRIX r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += pA->m[i][k] * pB->m[k][j];
      r.m[i][j] = s;
    }
  *pOut = r;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixScaling(D3DXMATRIX* pOut, float sx, float sy, float sz) {
  D3DXMatrixIdentity(pOut);
  pOut->_11 = sx; pOut->_22 = sy; pOut->_33 = sz;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixTranslation(D3DXMATRIX* pOut, float x, float y, float z) {
  D3DXMatrixIdentity(pOut);
  pOut->_41 = x; pOut->_42 = y; pOut->_43 = z;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixRotationX(D3DXMATRIX* pOut, float a) {
  D3DXMatrixIdentity(pOut);
  float c = cosf(a), s = sinf(a);
  pOut->_22 = c;  pOut->_23 = s;
  pOut->_32 = -s; pOut->_33 = c;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixRotationY(D3DXMATRIX* pOut, float a) {
  D3DXMatrixIdentity(pOut);
  float c = cosf(a), s = sinf(a);
  pOut->_11 = c;  pOut->_13 = -s;
  pOut->_31 = s;  pOut->_33 = c;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixRotationZ(D3DXMATRIX* pOut, float a) {
  D3DXMatrixIdentity(pOut);
  float c = cosf(a), s = sinf(a);
  pOut->_11 = c;  pOut->_12 = s;
  pOut->_21 = -s; pOut->_22 = c;
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixRotationAxis(D3DXMATRIX* pOut, const D3DXVECTOR3* pAxis, float a) {
  float c = cosf(a), s = sinf(a);
  D3DXVECTOR3 u = *pAxis;
  float len = sqrtf(u.x * u.x + u.y * u.y + u.z * u.z);
  if (len > 0) { u.x /= len; u.y /= len; u.z /= len; }
  D3DXMatrixIdentity(pOut);
  pOut->_11 = c + u.x * u.x * (1 - c);
  pOut->_12 = u.x * u.y * (1 - c) + u.z * s;
  pOut->_13 = u.x * u.z * (1 - c) - u.y * s;
  pOut->_21 = u.y * u.x * (1 - c) - u.z * s;
  pOut->_22 = c + u.y * u.y * (1 - c);
  pOut->_23 = u.y * u.z * (1 - c) + u.x * s;
  pOut->_31 = u.z * u.x * (1 - c) + u.y * s;
  pOut->_32 = u.z * u.y * (1 - c) - u.x * s;
  pOut->_33 = c + u.z * u.z * (1 - c);
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixRotationQuaternion(D3DXMATRIX* pOut, const D3DXQUATERNION* pQ) {
  D3DXMatrixIdentity(pOut);
  const float x2 = pQ->x * 2, y2 = pQ->y * 2, z2 = pQ->z * 2;
  const float xs = pQ->x * x2, ys = pQ->y * y2, zs = pQ->z * z2;
  const float xy = pQ->x * y2, xz = pQ->x * z2, yz = pQ->y * z2;
  const float wx = pQ->w * x2, wy = pQ->w * y2, wz = pQ->w * z2;
  pOut->_11 = 1 - (ys + zs); pOut->_12 = xy + wz;       pOut->_13 = xz - wy;
  pOut->_21 = xy - wz;       pOut->_22 = 1 - (xs + zs); pOut->_23 = yz + wx;
  pOut->_31 = xz + wy;       pOut->_32 = yz - wx;       pOut->_33 = 1 - (xs + ys);
  return pOut;
}

inline D3DXMATRIX* D3DXMatrixInverse(D3DXMATRIX* pOut, float* pDeterminant, const D3DXMATRIX* pM) {
  const float a00 = pM->_11, a01 = pM->_12, a02 = pM->_13;
  const float a10 = pM->_21, a11 = pM->_22, a12 = pM->_23;
  const float a20 = pM->_31, a21 = pM->_32, a22 = pM->_33;
  const float b01 = a22 * a11 - a12 * a21;
  const float b11 = -a22 * a10 + a12 * a20;
  const float b21 = a21 * a10 - a11 * a20;
  float d = a00 * b01 + a01 * b11 + a02 * b21;
  if (d == 0.0f) return 0;
  const float inv = 1.0f / d;
  if (pDeterminant) *pDeterminant = d;
  D3DXMatrixIdentity(pOut);
  pOut->_11 = b01 * inv;
  pOut->_12 = (-a22 * a01 + a02 * a21) * inv;
  pOut->_13 = (a12 * a01 - a02 * a11) * inv;
  pOut->_21 = b11 * inv;
  pOut->_22 = (a22 * a00 - a02 * a20) * inv;
  pOut->_23 = (-a12 * a00 + a02 * a10) * inv;
  pOut->_31 = b21 * inv;
  pOut->_32 = (-a21 * a00 + a01 * a20) * inv;
  pOut->_33 = (a11 * a00 - a01 * a10) * inv;
  /* affine translation row of the inverse: -(t * R^-1) */
  const float tx = pM->_41, ty = pM->_42, tz = pM->_43;
  pOut->_41 = -(tx * pOut->_11 + ty * pOut->_21 + tz * pOut->_31);
  pOut->_42 = -(tx * pOut->_12 + ty * pOut->_22 + tz * pOut->_32);
  pOut->_43 = -(tx * pOut->_13 + ty * pOut->_23 + tz * pOut->_33);
  return pOut;
}

/* ---------------- vectors ---------------- */
inline float D3DXVec3Dot(const D3DXVECTOR3* pA, const D3DXVECTOR3* pB) {
  return pA->x * pB->x + pA->y * pB->y + pA->z * pB->z;
}

inline float D3DXVec3Length(const D3DXVECTOR3* pV) {
  return sqrtf(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z);
}

inline float D3DXVec3LengthSq(const D3DXVECTOR3* pV) {
  return pV->x * pV->x + pV->y * pV->y + pV->z * pV->z;
}

inline D3DXVECTOR3* D3DXVec3Cross(D3DXVECTOR3* pOut, const D3DXVECTOR3* pA, const D3DXVECTOR3* pB) {
  pOut->x = pA->y * pB->z - pA->z * pB->y;
  pOut->y = pA->z * pB->x - pA->x * pB->z;
  pOut->z = pA->x * pB->y - pA->y * pB->x;
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3Add(D3DXVECTOR3* pOut, const D3DXVECTOR3* pA, const D3DXVECTOR3* pB) {
  pOut->x = pA->x + pB->x; pOut->y = pA->y + pB->y; pOut->z = pA->z + pB->z;
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3Subtract(D3DXVECTOR3* pOut, const D3DXVECTOR3* pA, const D3DXVECTOR3* pB) {
  pOut->x = pA->x - pB->x; pOut->y = pA->y - pB->y; pOut->z = pA->z - pB->z;
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3Scale(D3DXVECTOR3* pOut, const D3DXVECTOR3* pV, float s) {
  pOut->x = pV->x * s; pOut->y = pV->y * s; pOut->z = pV->z * s;
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3Lerp(D3DXVECTOR3* pOut, const D3DXVECTOR3* pV1, const D3DXVECTOR3* pV2, float s) {
  pOut->x = pV1->x + s * (pV2->x - pV1->x);
  pOut->y = pV1->y + s * (pV2->y - pV1->y);
  pOut->z = pV1->z + s * (pV2->z - pV1->z);
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3Normalize(D3DXVECTOR3* pOut, const D3DXVECTOR3* pV) {
  float len = sqrtf(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z);
  if (len > 0) { pOut->x = pV->x / len; pOut->y = pV->y / len; pOut->z = pV->z / len; }
  else { pOut->x = 0; pOut->y = 0; pOut->z = 1; }
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3TransformCoord(D3DXVECTOR3* pOut, const D3DXVECTOR3* pV, const D3DXMATRIX* pM) {
  float x = pV->x * pM->_11 + pV->y * pM->_21 + pV->z * pM->_31 + pM->_41;
  float y = pV->x * pM->_12 + pV->y * pM->_22 + pV->z * pM->_32 + pM->_42;
  float z = pV->x * pM->_13 + pV->y * pM->_23 + pV->z * pM->_33 + pM->_43;
  float w = pV->x * pM->_14 + pV->y * pM->_24 + pV->z * pM->_34 + pM->_44;
  if (w != 0.0f) { x /= w; y /= w; z /= w; }
  pOut->x = x; pOut->y = y; pOut->z = z;
  return pOut;
}

inline D3DXVECTOR3* D3DXVec3TransformNormal(D3DXVECTOR3* pOut, const D3DXVECTOR3* pV, const D3DXMATRIX* pM) {
  float x = pV->x * pM->_11 + pV->y * pM->_21 + pV->z * pM->_31;
  float y = pV->x * pM->_12 + pV->y * pM->_22 + pV->z * pM->_32;
  float z = pV->x * pM->_13 + pV->y * pM->_23 + pV->z * pM->_33;
  pOut->x = x; pOut->y = y; pOut->z = z;
  return pOut;
}

inline float D3DXVec3Distance(const D3DXVECTOR3* pA, const D3DXVECTOR3* pB) {
  const float dx = pA->x - pB->x, dy = pA->y - pB->y, dz = pA->z - pB->z;
  return sqrtf(dx * dx + dy * dy + dz * dz);
}

inline D3DXVECTOR4* D3DXVec4Transform(D3DXVECTOR4* pOut, const D3DXVECTOR4* pV, const D3DXMATRIX* pM) {
  float x = pV->x * pM->_11 + pV->y * pM->_21 + pV->z * pM->_31 + pV->w * pM->_41;
  float y = pV->x * pM->_12 + pV->y * pM->_22 + pV->z * pM->_32 + pV->w * pM->_42;
  float z = pV->x * pM->_13 + pV->y * pM->_23 + pV->z * pM->_33 + pV->w * pM->_43;
  float w = pV->x * pM->_14 + pV->y * pM->_24 + pV->z * pM->_34 + pV->w * pM->_44;
  pOut->x = x; pOut->y = y; pOut->z = z; pOut->w = w;
  return pOut;
}

inline D3DXVECTOR4* D3DXVec4TransformArray(D3DXVECTOR4* pOut, unsigned outStride,
                                           const D3DXVECTOR4* pV, unsigned vStride,
                                           const D3DXMATRIX* pM, unsigned nCount) {
  for (unsigned i = 0; i < nCount; ++i) {
    D3DXVECTOR4 v;
    memcpy(&v, (const char*)pV + i * vStride, sizeof(v));
    D3DXVec4Transform((D3DXVECTOR4*)((char*)pOut + i * outStride), &v, pM);
  }
  return pOut;
}

/* ---------------- quaternions ---------------- */
inline D3DXQUATERNION* D3DXQuaternionNormalize(D3DXQUATERNION* pOut, const D3DXQUATERNION* pQ) {
  float len = sqrtf(pQ->x * pQ->x + pQ->y * pQ->y + pQ->z * pQ->z + pQ->w * pQ->w);
  if (len > 0) { pOut->x = pQ->x / len; pOut->y = pQ->y / len; pOut->z = pQ->z / len; pOut->w = pQ->w / len; }
  else { pOut->x = 0; pOut->y = 0; pOut->z = 0; pOut->w = 1; }
  return pOut;
}

inline D3DXQUATERNION* D3DXQuaternionIdentity(D3DXQUATERNION* pOut) {
  pOut->x = pOut->y = pOut->z = 0; pOut->w = 1;
  return pOut;
}

inline D3DXQUATERNION* D3DXQuaternionRotationMatrix(D3DXQUATERNION* pOut, const D3DXMATRIX* pM) {
  const float trace = pM->_11 + pM->_22 + pM->_33;
  if (trace > 0.0f) {
    float s = 0.5f / sqrtf(trace + 1.0f);
    pOut->w = 0.25f / s;
    pOut->x = (pM->_23 - pM->_32) * s;
    pOut->y = (pM->_31 - pM->_13) * s;
    pOut->z = (pM->_12 - pM->_21) * s;
  } else {
    int i = (pM->_11 > pM->_22) ? ((pM->_11 > pM->_33) ? 0 : 2) : ((pM->_22 > pM->_33) ? 1 : 2);
    static const int nxt[3] = { 1, 2, 0 };
    static const int nxt2[3] = { 2, 0, 1 };
    const float base = pM->m[i][i], a = pM->m[nxt[i]][nxt[i]], b = pM->m[nxt2[i]][nxt2[i]];
    float s = 2.0f * sqrtf(1.0f + base - a - b);
    pOut->w = (pM->m[nxt[i]][nxt2[i]] - pM->m[nxt2[i]][nxt[i]]) / s;
    pOut->x = (pM->m[nxt[i]][i] + pM->m[i][nxt[i]]) / s;
    pOut->y = (pM->m[nxt2[i]][i] + pM->m[i][nxt2[i]]) / s;
    pOut->z = (pM->m[nxt[i]][nxt2[i]] + pM->m[nxt2[i]][nxt[i]]) / s;
  }
  return pOut;
}

inline D3DXQUATERNION* D3DXQuaternionMultiply(D3DXQUATERNION* pOut, const D3DXQUATERNION* pA, const D3DXQUATERNION* pB) {
  D3DXQUATERNION r;
  r.x = pA->w * pB->x + pA->x * pB->w + pA->y * pB->z - pA->z * pB->y;
  r.y = pA->w * pB->y - pA->x * pB->z + pA->y * pB->w + pA->z * pB->x;
  r.z = pA->w * pB->z + pA->x * pB->y - pA->y * pB->x + pA->z * pB->w;
  r.w = pA->w * pB->w - pA->x * pB->x - pA->y * pB->y - pA->z * pB->z;
  *pOut = r;
  return pOut;
}

#endif  /* C++ only */
