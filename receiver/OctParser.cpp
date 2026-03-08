#define _CRT_SECURE_NO_WARNINGS
#include "OctParser.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <stack>

#pragma pack(push, 1)
struct OctTriangleDisk {
    float v0[3], v1[3], v2[3];
    float normal[3];
    float planeD;
    uint32_t material;
    uint32_t flags[2];
    uint16_t grid[6];
};
#pragma pack(pop)

static_assert(sizeof(OctTriangleDisk) == 76, "OctTriangleDisk must be 76 bytes");

// Helper: read from a memory cursor
struct MemReader {
    const uint8_t* data;
    size_t size;
    size_t pos;

    bool read(void* dst, size_t bytes) {
        if (pos + bytes > size) return false;
        memcpy(dst, data + pos, bytes);
        pos += bytes;
        return true;
    }
    bool skip(size_t bytes) {
        if (pos + bytes > size) return false;
        pos += bytes;
        return true;
    }
    bool seek(size_t offset) {
        if (offset > size) return false;
        pos = offset;
        return true;
    }
};

// Shared parsing logic that works from a MemReader
static bool OctParseInternal(MemReader& r, OctTree& out)
{
    // --- Header (80 bytes) ---
    uint32_t magic;
    if (!r.read(&magic, 4) || magic != 0xFF000004u) return false;

    uint32_t nodeCount, triCount;
    r.read(&nodeCount, 4);
    r.read(&triCount, 4);

    // Skip validation bounds (32 bytes: 2x(4 floats))
    r.seek(12);
    r.skip(32); // skip to offset 44

    // Stored bounds (3x OctVec3 = 36 bytes, offsets 44-80)
    r.read(&out.boundsMin, sizeof(OctVec3));
    r.read(&out.boundsMax, sizeof(OctVec3));
    r.read(&out.center, sizeof(OctVec3));

    // --- Triangles (76 bytes each) ---
    out.triangles.resize(triCount);
    std::vector<OctTriangleDisk> diskTris(triCount);
    if (!r.read(diskTris.data(), sizeof(OctTriangleDisk) * triCount))
        return false;

    for (uint32_t i = 0; i < triCount; i++) {
        const auto& d = diskTris[i];
        auto& t = out.triangles[i];
        t.v[0] = { d.v0[0], d.v0[1], d.v0[2] };
        t.v[1] = { d.v1[0], d.v1[1], d.v1[2] };
        t.v[2] = { d.v2[0], d.v2[1], d.v2[2] };
        t.normal = { d.normal[0], d.normal[1], d.normal[2] };
        t.planeD = d.planeD;
        t.material = d.material;
    }

    // --- Octree Nodes (DFS pre-order) ---
    out.nodes.resize(nodeCount);
    out.childIndices.clear();
    out.triIndices.clear();

    struct StackEntry { uint32_t remaining; uint32_t parentIdx; };
    std::vector<StackEntry> stk;
    stk.reserve(64);

    for (uint32_t i = 0; i < nodeCount; i++) {
        auto& node = out.nodes[i];

        r.read(&node.center, sizeof(OctVec3));
        r.read(&node.radius, sizeof(float));
        r.read(&node.childCount, sizeof(uint32_t));

        uint32_t triRefCount;
        r.read(&triRefCount, sizeof(uint32_t));

        node.triCount = triRefCount;
        node.firstChildIdx = 0;

        while (!stk.empty() && stk.back().remaining == 0)
            stk.pop_back();

        if (!stk.empty()) {
            auto& top = stk.back();
            auto& parent = out.nodes[top.parentIdx];
            uint32_t childSlot = parent.childCount - top.remaining;
            out.childIndices[parent.firstChildIdx + childSlot] = i;
            top.remaining--;
        }

        node.firstTriIdx = (uint32_t)out.triIndices.size();
        if (triRefCount > 0) {
            size_t base = out.triIndices.size();
            out.triIndices.resize(base + triRefCount);
            r.read(&out.triIndices[base], sizeof(uint32_t) * triRefCount);
        }

        if (node.childCount > 0) {
            node.firstChildIdx = (uint32_t)out.childIndices.size();
            out.childIndices.resize(out.childIndices.size() + node.childCount, 0);
            stk.push_back({ node.childCount, i });
        }
    }

    return true;
}

bool OctParse(const char* filePath, OctTree& out)
{
    FILE* f = fopen(filePath, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    size_t fileSize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<uint8_t> buf(fileSize);
    if (fread(buf.data(), 1, fileSize, f) != fileSize) {
        fclose(f);
        return false;
    }
    fclose(f);

    return OctParseFromMemory(buf.data(), fileSize, out);
}

bool OctParseFromMemory(const uint8_t* data, size_t dataSize, OctTree& out)
{
    MemReader r = { data, dataSize, 0 };
    return OctParseInternal(r, out);
}

// --- Ray-triangle intersection (Moller-Trumbore) ---
static bool RayTriangle(const OctVec3& origin, const OctVec3& dir,
                        const OctVec3& v0, const OctVec3& v1, const OctVec3& v2,
                        float& outT)
{
    const float EPSILON = 1e-7f;

    float e1x = v1.x - v0.x, e1y = v1.y - v0.y, e1z = v1.z - v0.z;
    float e2x = v2.x - v0.x, e2y = v2.y - v0.y, e2z = v2.z - v0.z;

    float hx = dir.y * e2z - dir.z * e2y;
    float hy = dir.z * e2x - dir.x * e2z;
    float hz = dir.x * e2y - dir.y * e2x;

    float a = e1x * hx + e1y * hy + e1z * hz;
    if (a > -EPSILON && a < EPSILON) return false;

    float f = 1.0f / a;
    float sx = origin.x - v0.x, sy = origin.y - v0.y, sz = origin.z - v0.z;
    float u = f * (sx * hx + sy * hy + sz * hz);
    if (u < 0.0f || u > 1.0f) return false;

    float qx = sy * e1z - sz * e1y;
    float qy = sz * e1x - sx * e1z;
    float qz = sx * e1y - sy * e1x;

    float v = f * (dir.x * qx + dir.y * qy + dir.z * qz);
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = f * (e2x * qx + e2y * qy + e2z * qz);
    if (t > EPSILON) {
        outT = t;
        return true;
    }
    return false;
}

// --- Ray-sphere intersection test ---
static bool RaySphere(const OctVec3& origin, const OctVec3& dir,
                      const OctVec3& center, float radius, float maxT)
{
    float ocx = origin.x - center.x;
    float ocy = origin.y - center.y;
    float ocz = origin.z - center.z;

    float b = ocx * dir.x + ocy * dir.y + ocz * dir.z;
    float c = ocx * ocx + ocy * ocy + ocz * ocz - radius * radius;

    if (c < 0.0f) return true;
    if (b > 0.0f) return false;

    float disc = b * b - c;
    if (disc < 0.0f) return false;

    float t = -b - sqrtf(disc);
    return t < maxT;
}

float OctRaycast(const OctTree& tree, const OctVec3& origin, const OctVec3& dir, OctVec3* hitNormal)
{
    if (tree.nodes.empty()) return FLT_MAX;

    float bestT = FLT_MAX;
    const OctTriangle* bestTri = nullptr;

    std::stack<uint32_t> stk;
    stk.push(0);

    while (!stk.empty()) {
        uint32_t idx = stk.top();
        stk.pop();

        const auto& node = tree.nodes[idx];

        if (!RaySphere(origin, dir, node.center, node.radius, bestT))
            continue;

        // Test triangles in this node
        for (uint32_t i = 0; i < node.triCount; i++) {
            uint32_t triIdx = tree.triIndices[node.firstTriIdx + i];
            const auto& tri = tree.triangles[triIdx];
            float t;
            if (RayTriangle(origin, dir, tri.v[0], tri.v[1], tri.v[2], t)) {
                if (t < bestT) {
                    bestT = t;
                    bestTri = &tri;
                }
            }
        }

        // Push children via childIndices
        for (uint32_t i = 0; i < node.childCount; i++) {
            stk.push(tree.childIndices[node.firstChildIdx + i]);
        }
    }

    if (hitNormal && bestTri) {
        *hitNormal = bestTri->normal;
    }
    return bestT;
}

bool OctLineOfSight(const OctTree& tree, const OctVec3& from, const OctVec3& to)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float dz = to.z - from.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    if (dist < 1e-6f) return true;

    float inv = 1.0f / dist;
    OctVec3 dir = { dx * inv, dy * inv, dz * inv };

    float hitDist = OctRaycast(tree, from, dir);
    return hitDist >= dist;
}
