#pragma once
#include <cstdint>
#include <vector>

struct OctVec3 { float x, y, z; };

struct OctTriangle {
    OctVec3 v[3];       // 3 vertices
    OctVec3 normal;      // face normal
    float planeD;        // plane distance
    uint32_t material;   // material tag
};

struct OctNode {
    OctVec3 center;          // bounding sphere center
    float radius;            // bounding sphere radius
    uint32_t childCount;     // 0-8 child octree nodes
    uint32_t firstChildIdx;  // index into childIndices array
    uint32_t triCount;       // triangle references
    uint32_t firstTriIdx;    // index into triIndices array
};

struct OctTree {
    OctVec3 boundsMin, boundsMax, center;
    std::vector<OctTriangle> triangles;
    std::vector<OctNode> nodes;
    std::vector<uint32_t> childIndices; // node -> child node index mapping
    std::vector<uint32_t> triIndices;   // node -> triangle mapping
};

// Parse a .oct collision file into an OctTree structure.
bool OctParse(const char* filePath, OctTree& out);

// Parse a .oct collision file from an in-memory buffer.
bool OctParseFromMemory(const uint8_t* data, size_t dataSize, OctTree& out);

// Cast a ray and return distance to nearest hit (FLT_MAX if no hit).
// Optionally returns the hit surface normal.
float OctRaycast(const OctTree& tree, const OctVec3& origin, const OctVec3& dir, OctVec3* hitNormal = nullptr);

// Returns true if there is an unobstructed line of sight between two points.
bool OctLineOfSight(const OctTree& tree, const OctVec3& from, const OctVec3& to);
