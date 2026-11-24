// prim shape oracle: drive the engine's own CollisionPrimitive through the records in prim/DATA.
//
// The engine reads each record as Read32 + 3 x ReadFloat (CollisionBodyPrimitive::Read,
// CollisionBody.cpp:236) and then calls SetSphere / SetCylinder / SetBox. This harness does the
// same thing, on the same header and the same translation unit - it does not re-implement them.
//
// Input : argv[1] - N records of 16 bytes, <u32 type, f32 f0, f32 f1, f32 f2>
// Output: argv[2] - N records of 28 bytes, <i32 mType, u32 assigned_mask,
//         u32 bits for mRadius, mHeight, mWidth, mDepth, mMeshCollisionRadius>
//
// Before each record the object is filled with SENTINEL. Whatever survives is a field the engine
// never writes. That is an observable fact, not an assumption: the constructor
// (CollisionPrimitive.cpp:42) initialises only mName, so fields that Set does not assign keep
// whatever the object already had. The check compares only the fields the source says are assigned,
// and reports the rest as "untouched".
//
// Build: tools/build_prim_shape_oracle.sh (same flags as tools/compile_native.sh, -m32)

#include "CollisionPrimitive.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static const uint32_t SENTINEL = 0xAAAAAAAAu;

#pragma pack(push, 1)
struct Record {
    uint32_t type;
    float f0;
    float f1;
    float f2;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct Result {
    int32_t mType;
    uint32_t assigned_mask;   // bit 0 mRadius, 1 mHeight, 2 mWidth, 3 mDepth, 4 mMeshCollisionRadius
    uint32_t radius_bits;
    uint32_t height_bits;
    uint32_t width_bits;
    uint32_t depth_bits;
    uint32_t mesh_bits;
};
#pragma pack(pop)

static const int F_RADIUS = 0, F_HEIGHT = 1, F_WIDTH = 2, F_DEPTH = 3, F_MESH = 4;

static inline uint32_t bits_of(float v) {
    uint32_t b = 0;
    memcpy(&b, &v, sizeof(b));
    return b;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <records.bin> <out.bin>\n", argv[0]);
        return 2;
    }

    FILE* fin = fopen(argv[1], "rb");
    if (!fin) { perror("open input"); return 2; }
    fseek(fin, 0, SEEK_END);
    long size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (size <= 0 || size % sizeof(Record) != 0) {
        fprintf(stderr, "input is not a whole number of 16-byte records: %ld\n", size);
        fclose(fin);
        return 2;
    }
    size_t count = (size_t)(size / (long)sizeof(Record));
    Record* records = (Record*)malloc(count * sizeof(Record));
    if (fread(records, sizeof(Record), count, fin) != count) { fclose(fin); return 2; }
    fclose(fin);

    FILE* fout = fopen(argv[2], "wb");
    if (!fout) { perror("open output"); return 2; }

    for (size_t i = 0; i < count; ++i) {
        CollisionPrimitive p;                       // the engine's constructor, not a value-init
        // Fill the fields we can name, not the whole object: the class is polymorphic, so byte
        // filling would overwrite the vtable pointer as well.
        float sentinel_f;
        uint32_t s = SENTINEL;
        memcpy(&sentinel_f, &s, sizeof(sentinel_f));
        p.mRadius = sentinel_f;
        p.mHeight = sentinel_f;
        p.mWidth = sentinel_f;
        p.mDepth = sentinel_f;
        p.mMeshCollisionRadius = sentinel_f;
        p.mType = (CollisionPrimitive::PrimitiveType)(int)SENTINEL;

        uint32_t mask = 0;
        switch (records[i].type) {
            case 0: case 1: case 3:                 // exactly the switch in CollisionBody.cpp
                p.SetSphere(records[i].f0);
                mask = (1u << F_RADIUS) | (1u << F_HEIGHT) | (1u << F_MESH);
                break;
            case 2:
                p.SetCylinder(records[i].f0, records[i].f1);
                mask = (1u << F_RADIUS) | (1u << F_HEIGHT) | (1u << F_MESH);
                break;
            case 4:
                p.SetBox(records[i].f0, records[i].f1, records[i].f2, -1.0f);
                mask = (1u << F_WIDTH) | (1u << F_HEIGHT) | (1u << F_DEPTH)
                     | (1u << F_RADIUS) | (1u << F_MESH);
                break;
            default:
                fprintf(stderr, "type %u not in the switch at record %zu\n", records[i].type, i);
                fclose(fout);
                return 2;
        }

        Result r;
        r.mType = (int32_t)p.mType;
        r.assigned_mask = mask;
        r.radius_bits = bits_of(p.mRadius);
        r.height_bits = bits_of(p.mHeight);
        r.width_bits = bits_of(p.mWidth);
        r.depth_bits = bits_of(p.mDepth);
        r.mesh_bits = bits_of(p.mMeshCollisionRadius);
        fwrite(&r, sizeof(r), 1, fout);
    }

    fclose(fout);
    printf("ran %zu records through the engine's own Set* functions\n", count);
    return 0;
}
