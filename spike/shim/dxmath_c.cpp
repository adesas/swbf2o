/* D3DX-named math that the decompiled engine calls with C linkage on its OWN types.
 *
 * Several modules declare, and call, routines like
 *
 *     extern "C" PblMatrix* __stdcall D3DXMatrixMultiply(PblMatrix* pOut,
 *                                                        const PblMatrix* pM1, const PblMatrix* pM2);
 *
 * i.e. the Direct3D 9 math API spelled over PblMatrix / PblVector3 / PblQuaternion. The binary
 * resolved them against the D3DX import library, so the decompilation contains the calls but no
 * body. They are ordinary gameplay maths (aiming, camera, cloth, lens flare, sky dome), so this is
 * a real implementation rather than a stub - exact D3DX9 semantics, matching the engine's layout.
 *
 * Layout facts taken from the decompiled headers, which pin the convention down:
 *   PblMatrix::m[4][4], GetTranslation() == m[3][0..2], GetXAxis() == m[0][0..2]
 *     -> identical memory layout to D3DXMATRIX: row-vector maths, v' = v * M.
 *   PblVector3 { float x, y, z; }
 *   PblQuaternion { float _fX, _fY, _fZ, _fS; }  (vector part first, scalar last)
 */
#include "swbf2_prefix.h"

#include "PebbleFL/Common/PblMatrix.h"
#include "PebbleFL/Common/PblVector.h"
#include "PebbleFL/Common/PblQuaternion.h"

#include <math.h>
#include <string.h>

extern "C" {

PblMatrix* __stdcall D3DXMatrixIdentity(PblMatrix* pOut) {
    memset(pOut->m, 0, sizeof(pOut->m));
    pOut->m[0][0] = pOut->m[1][1] = pOut->m[2][2] = pOut->m[3][3] = 1.0f;
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixMultiply(PblMatrix* pOut, const PblMatrix* pM1, const PblMatrix* pM2) {
    float t[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r][c] = pM1->m[r][0] * pM2->m[0][c] + pM1->m[r][1] * pM2->m[1][c] +
                      pM1->m[r][2] * pM2->m[2][c] + pM1->m[r][3] * pM2->m[3][c];
    memcpy(pOut->m, t, sizeof(t));
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixScaling(PblMatrix* pOut, float sx, float sy, float sz) {
    memset(pOut->m, 0, sizeof(pOut->m));
    pOut->m[0][0] = sx; pOut->m[1][1] = sy; pOut->m[2][2] = sz; pOut->m[3][3] = 1.0f;
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixTranslation(PblMatrix* pOut, float x, float y, float z) {
    D3DXMatrixIdentity(pOut);
    pOut->m[3][0] = x; pOut->m[3][1] = y; pOut->m[3][2] = z;
    return pOut;
}

/* D3DX row-vector forms: rotating +a about +x maps +y towards +z, which is what these matrices do
 * when a row vector is multiplied on the left. */
PblMatrix* __stdcall D3DXMatrixRotationX(PblMatrix* pOut, float a) {
    float c = cosf(a), s = sinf(a);
    D3DXMatrixIdentity(pOut);
    pOut->m[1][1] = c;  pOut->m[1][2] = s;
    pOut->m[2][1] = -s; pOut->m[2][2] = c;
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixRotationY(PblMatrix* pOut, float a) {
    float c = cosf(a), s = sinf(a);
    D3DXMatrixIdentity(pOut);
    pOut->m[0][0] = c;  pOut->m[0][2] = -s;
    pOut->m[2][0] = s;  pOut->m[2][2] = c;
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixRotationZ(PblMatrix* pOut, float a) {
    float c = cosf(a), s = sinf(a);
    D3DXMatrixIdentity(pOut);
    pOut->m[0][0] = c;  pOut->m[0][1] = s;
    pOut->m[1][0] = -s; pOut->m[1][1] = c;
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixRotationAxis(PblMatrix* pOut, const PblVector3* pV, float a) {
    float x = pV->x, y = pV->y, z = pV->z;
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) { x /= len; y /= len; z /= len; }
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    float tx = t * x, ty = t * y, tz = t * z;
    memset(pOut->m, 0, sizeof(pOut->m));
    pOut->m[0][0] = tx * x + c;    pOut->m[0][1] = ty * x + s * z; pOut->m[0][2] = tz * x - s * y;
    pOut->m[1][0] = tx * y - s * z; pOut->m[1][1] = ty * y + c;    pOut->m[1][2] = tz * y + s * x;
    pOut->m[2][0] = tx * z + s * y; pOut->m[2][1] = ty * z - s * x; pOut->m[2][2] = tz * z + c;
    pOut->m[3][3] = 1.0f;
    return pOut;
}

PblMatrix* __stdcall D3DXMatrixRotationQuaternion(PblMatrix* pOut, const PblQuaternion* pQ) {
    float x = pQ->_fX, y = pQ->_fY, z = pQ->_fZ, w = pQ->_fS;
    float x2 = 2.0f * x * x, y2 = 2.0f * y * y, z2 = 2.0f * z * z;
    float xy = 2.0f * x * y, xz = 2.0f * x * z, yz = 2.0f * y * z;
    float sx = 2.0f * w * x, sy = 2.0f * w * y, sz = 2.0f * w * z;
    memset(pOut->m, 0, sizeof(pOut->m));
    pOut->m[0][0] = 1.0f - y2 - z2; pOut->m[0][1] = xy + sz;       pOut->m[0][2] = xz - sy;
    pOut->m[1][0] = xy - sz;        pOut->m[1][1] = 1.0f - x2 - z2; pOut->m[1][2] = yz + sx;
    pOut->m[2][0] = xz + sy;        pOut->m[2][1] = yz - sx;       pOut->m[2][2] = 1.0f - x2 - y2;
    pOut->m[3][3] = 1.0f;
    return pOut;
}

/* Same algorithm Direct3DX uses: invert the upper-left 3x3 by cofactors (which for a rotation
 * matrix is the transpose scaled by 1/det), then transform the translation row. */
PblMatrix* __stdcall D3DXMatrixInverse(PblMatrix* pOut, float* pDeterminant, const PblMatrix* pM) {
    const float (*m)[4] = pM->m;
    float minor[9];
    minor[0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    minor[1] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    minor[2] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    float det = m[0][0] * minor[0] + m[0][1] * minor[1] + m[0][2] * minor[2];
    if (pDeterminant) *pDeterminant = det;
    if (det == 0.0f) return 0;
    float r = 1.0f / det;
    float inv[3][3];
    inv[0][0] = minor[0] * r;
    inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * r;
    inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * r;
    inv[1][0] = minor[1] * r;
    inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * r;
    inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * r;
    inv[2][0] = minor[2] * r;
    inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * r;
    inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * r;

    float out[4][4];
    for (int c = 0; c < 3; ++c)
        out[3][c] = m[3][0] * inv[0][c] + m[3][1] * inv[1][c] + m[3][2] * inv[2][c];
    for (int r0 = 0; r0 < 3; ++r0) {
        for (int c = 0; c < 3; ++c)
            out[r0][c] = inv[r0][c];
        out[r0][3] = 0.0f;
    }
    out[3][3] = 1.0f;
    memcpy(pOut->m, out, sizeof(out));
    return pOut;
}

PblVector3* __stdcall D3DXVec3Normalize(PblVector3* pOut, const PblVector3* pV) {
    float len = sqrtf(pV->x * pV->x + pV->y * pV->y + pV->z * pV->z);
    if (len > 0.0f) { pOut->x = pV->x / len; pOut->y = pV->y / len; pOut->z = pV->z / len; }
    else { pOut->x = pOut->y = pOut->z = 0.0f; }
    return pOut;
}

PblVector3* __stdcall D3DXVec3TransformCoord(PblVector3* pOut, const PblVector3* pV, const PblMatrix* pM) {
    const float (*m)[4] = pM->m;
    float x = pV->x, y = pV->y, z = pV->z;
    float w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
    pOut->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    pOut->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
    pOut->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
    if (w != 0.0f && w != 1.0f) { pOut->x /= w; pOut->y /= w; pOut->z /= w; }
    return pOut;
}

PblVector3* __stdcall D3DXVec3TransformNormal(PblVector3* pOut, const PblVector3* pV, const PblMatrix* pM) {
    const float (*m)[4] = pM->m;
    float x = pV->x, y = pV->y, z = pV->z;
    pOut->x = x * m[0][0] + y * m[1][0] + z * m[2][0];
    pOut->y = x * m[0][1] + y * m[1][1] + z * m[2][1];
    pOut->z = x * m[0][2] + y * m[1][2] + z * m[2][2];
    return pOut;
}

PblQuaternion* __stdcall D3DXQuaternionNormalize(PblQuaternion* pOut, const PblQuaternion* pQ) {
    float len = sqrtf(pQ->_fX * pQ->_fX + pQ->_fY * pQ->_fY + pQ->_fZ * pQ->_fZ + pQ->_fS * pQ->_fS);
    if (len == 0.0f) return 0;
    pOut->_fX = pQ->_fX / len; pOut->_fY = pQ->_fY / len;
    pOut->_fZ = pQ->_fZ / len; pOut->_fS = pQ->_fS / len;
    return pOut;
}

PblQuaternion* __stdcall D3DXQuaternionRotationMatrix(PblQuaternion* pOut, const PblMatrix* pM) {
    const float (*m)[4] = pM->m;
    float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        pOut->_fS = 0.25f * s;
        pOut->_fX = (m[1][2] - m[2][1]) / s;
        pOut->_fY = (m[2][0] - m[0][2]) / s;
        pOut->_fZ = (m[0][1] - m[1][0]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        pOut->_fS = (m[1][2] - m[2][1]) / s;
        pOut->_fX = 0.25f * s;
        pOut->_fY = (m[0][1] + m[1][0]) / s;
        pOut->_fZ = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        pOut->_fS = (m[2][0] - m[0][2]) / s;
        pOut->_fX = (m[0][1] + m[1][0]) / s;
        pOut->_fY = 0.25f * s;
        pOut->_fZ = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        pOut->_fS = (m[0][1] - m[1][0]) / s;
        pOut->_fX = (m[0][2] + m[2][0]) / s;
        pOut->_fY = (m[1][2] + m[2][1]) / s;
        pOut->_fZ = 0.25f * s;
    }
    return pOut;
}

}  /* extern "C" */
