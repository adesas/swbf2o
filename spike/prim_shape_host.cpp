// Host-side, not engine code.
//
// PblMatrix's constructor from four PblVector4 rows is declared in PblMatrix.h and has no body in
// this tree. CollisionPrimitive.cpp builds a file-scope object with it (_IdentityMatrix, line 467),
// so that initializer runs before main and the binary cannot start without some definition. The check
// never reads that object: the measured path is SetSphere/SetCylinder/SetBox, which do not touch it.
// So this exists only so the process can start, and the check reports it as a host-side dependency,
// not as engine behaviour.
//
// Plain C ABI form (one hidden this pointer), because PblVector4 is only forward declared in the
// header.
extern "C" void _ZN9PblMatrixC1ERK10PblVector4S2_S2_S2_(void* self) {
    float* m = (float*)self;
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
