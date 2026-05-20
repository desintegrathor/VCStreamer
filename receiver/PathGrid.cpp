#define _CRT_SECURE_NO_WARNINGS
#include "PathGrid.h"
#include "OctCollision.h"
#include <cmath>
#include <cstring>
#include <cfloat>
#include <cstdlib>
#include <iostream>
#include <windows.h>

extern void LogDebug(const char* fmt, ...);

// ============================================================================
// Clearance Grid
// ============================================================================

struct ClearanceGrid {
    float* data;        // clearance values (FLT_MAX = open sky)
    int    dimX, dimY, dimZ;
    float  minX, minY, minZ;
    float  resolution;
    bool   ready;
};

static ClearanceGrid g_grid = {};

static inline int GridIndex(int x, int y, int z) {
    return z * g_grid.dimX * g_grid.dimY + y * g_grid.dimX + x;
}

static inline bool GridInBounds(int x, int y, int z) {
    return x >= 0 && x < g_grid.dimX &&
           y >= 0 && y < g_grid.dimY &&
           z >= 0 && z < g_grid.dimZ;
}

static inline void WorldToGrid(float wx, float wy, float wz, int& gx, int& gy, int& gz) {
    gx = (int)((wx - g_grid.minX) / g_grid.resolution);
    gy = (int)((wy - g_grid.minY) / g_grid.resolution);
    gz = (int)((wz - g_grid.minZ) / g_grid.resolution);
}

static inline void GridToWorld(int gx, int gy, int gz, float& wx, float& wy, float& wz) {
    wx = g_grid.minX + ((float)gx + 0.5f) * g_grid.resolution;
    wy = g_grid.minY + ((float)gy + 0.5f) * g_grid.resolution;
    wz = g_grid.minZ + ((float)gz + 0.5f) * g_grid.resolution;
}

// 14 ray directions: 6 axis-aligned + 8 diagonal
static const float g_rayDirs[14][3] = {
    { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1},
    { 0.5774f, 0.5774f, 0.5774f}, {-0.5774f, 0.5774f, 0.5774f},
    { 0.5774f,-0.5774f, 0.5774f}, {-0.5774f,-0.5774f, 0.5774f},
    { 0.5774f, 0.5774f,-0.5774f}, {-0.5774f, 0.5774f,-0.5774f},
    { 0.5774f,-0.5774f,-0.5774f}, {-0.5774f,-0.5774f,-0.5774f}
};

void PathGrid_Build(float resolution) {
    PathGrid_Destroy();

    if (!OctCollision_IsLoaded()) {
        LogDebug("[PathGrid] OctCollision not loaded, cannot build grid\n");
        return;
    }

    float boundsMin[3], boundsMax[3];
    if (!OctCollision_GetBounds(boundsMin, boundsMax)) {
        LogDebug("[PathGrid] Cannot get map bounds\n");
        return;
    }

    DWORD startTime = GetTickCount();

    g_grid.resolution = resolution;
    g_grid.minX = boundsMin[0];
    g_grid.minY = boundsMin[1];
    g_grid.minZ = boundsMin[2];

    g_grid.dimX = (int)ceilf((boundsMax[0] - boundsMin[0]) / resolution) + 1;
    g_grid.dimY = (int)ceilf((boundsMax[1] - boundsMin[1]) / resolution) + 1;
    g_grid.dimZ = (int)ceilf((boundsMax[2] - boundsMin[2]) / resolution) + 1;

    // Cap dimensions to avoid excessive memory
    if (g_grid.dimX > 512) g_grid.dimX = 512;
    if (g_grid.dimY > 512) g_grid.dimY = 512;
    if (g_grid.dimZ > 128) g_grid.dimZ = 128;

    int totalVoxels = g_grid.dimX * g_grid.dimY * g_grid.dimZ;
    size_t memSize = (size_t)totalVoxels * sizeof(float);

    g_grid.data = (float*)malloc(memSize);
    if (!g_grid.data) {
        LogDebug("[PathGrid] Failed to allocate %zu bytes for %d voxels\n", memSize, totalVoxels);
        return;
    }

    // Initialize all voxels
    int solidCount = 0;
    for (int z = 0; z < g_grid.dimZ; z++) {
        for (int y = 0; y < g_grid.dimY; y++) {
            for (int x = 0; x < g_grid.dimX; x++) {
                float wx, wy, wz;
                GridToWorld(x, y, z, wx, wy, wz);

                float origin[3] = { wx, wy, wz };
                float minHit = FLT_MAX;

                for (int r = 0; r < 14; r++) {
                    float dist = OctCollision_Raycast(origin, g_rayDirs[r], nullptr);
                    if (dist < minHit) minHit = dist;
                }

                g_grid.data[GridIndex(x, y, z)] = minHit;
                if (minHit < 0.3f) solidCount++;
            }
        }
    }

    DWORD elapsed = GetTickCount() - startTime;
    g_grid.ready = true;

    LogDebug("[PathGrid] Built: %dx%dx%d = %d voxels, %d solid, %.1f MB, %lu ms\n",
             g_grid.dimX, g_grid.dimY, g_grid.dimZ, totalVoxels, solidCount,
             (float)memSize / (1024.0f * 1024.0f), elapsed);
    std::cout << "[PathGrid] Built: " << g_grid.dimX << "x" << g_grid.dimY << "x" << g_grid.dimZ
              << " = " << totalVoxels << " voxels, " << solidCount << " solid, "
              << (float)memSize / (1024.0f * 1024.0f) << " MB, " << elapsed << " ms\n";
}

void PathGrid_Destroy() {
    if (g_grid.data) {
        free(g_grid.data);
        g_grid.data = nullptr;
    }
    g_grid.ready = false;
}

bool PathGrid_IsReady() {
    return g_grid.ready;
}

float PathGrid_GetClearance(float x, float y, float z) {
    if (!g_grid.ready) return 0.0f;
    int gx, gy, gz;
    WorldToGrid(x, y, z, gx, gy, gz);
    if (!GridInBounds(gx, gy, gz)) return 0.0f;
    return g_grid.data[GridIndex(gx, gy, gz)];
}

// ============================================================================
// Theta* Pathfinding
// ============================================================================

// Open-set node for A*
struct ThetaNode {
    int x, y, z;
    int parentIdx;   // index into closed set (-1 = start uses itself as LOS parent)
    float g, f;
};

// Binary min-heap for open set
static constexpr int THETA_MAX_OPEN = 4096;
static constexpr int THETA_MAX_CLOSED = 8192;

struct ThetaHeap {
    ThetaNode nodes[THETA_MAX_OPEN];
    int count;
};

static inline void HeapSwap(ThetaHeap& h, int a, int b) {
    ThetaNode tmp = h.nodes[a];
    h.nodes[a] = h.nodes[b];
    h.nodes[b] = tmp;
}

static void HeapPush(ThetaHeap& h, const ThetaNode& n) {
    if (h.count >= THETA_MAX_OPEN) return;
    h.nodes[h.count] = n;
    int i = h.count++;
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (h.nodes[i].f < h.nodes[parent].f) {
            HeapSwap(h, i, parent);
            i = parent;
        } else break;
    }
}

static ThetaNode HeapPop(ThetaHeap& h) {
    ThetaNode result = h.nodes[0];
    h.count--;
    if (h.count > 0) {
        h.nodes[0] = h.nodes[h.count];
        int i = 0;
        for (;;) {
            int left = 2 * i + 1, right = 2 * i + 2, smallest = i;
            if (left < h.count && h.nodes[left].f < h.nodes[smallest].f) smallest = left;
            if (right < h.count && h.nodes[right].f < h.nodes[smallest].f) smallest = right;
            if (smallest != i) { HeapSwap(h, i, smallest); i = smallest; }
            else break;
        }
    }
    return result;
}

// Hash map for visited cells (closed set)
static constexpr int VISIT_HASH_SIZE = 16384; // must be power of 2

struct VisitEntry {
    int key;       // packed grid coord, 0 = empty
    int parentIdx; // index into closed array
    float g;
};

struct VisitMap {
    VisitEntry entries[VISIT_HASH_SIZE];
};

static inline int PackCoord(int x, int y, int z) {
    // Pack into single int: x (10 bits) | y (10 bits) | z (10 bits) + 1 to avoid 0
    return ((x & 0x3FF) | ((y & 0x3FF) << 10) | ((z & 0x3FF) << 20)) + 1;
}

static inline int VisitHash(int key) {
    // Simple hash
    unsigned int h = (unsigned int)key;
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return (int)(h & (VISIT_HASH_SIZE - 1));
}

static VisitEntry* VisitFind(VisitMap& vm, int key) {
    int idx = VisitHash(key);
    for (int i = 0; i < 32; i++) {
        int slot = (idx + i) & (VISIT_HASH_SIZE - 1);
        if (vm.entries[slot].key == key) return &vm.entries[slot];
        if (vm.entries[slot].key == 0) return nullptr;
    }
    return nullptr;
}

static VisitEntry* VisitInsert(VisitMap& vm, int key) {
    int idx = VisitHash(key);
    for (int i = 0; i < 32; i++) {
        int slot = (idx + i) & (VISIT_HASH_SIZE - 1);
        if (vm.entries[slot].key == 0 || vm.entries[slot].key == key) {
            vm.entries[slot].key = key;
            return &vm.entries[slot];
        }
    }
    return nullptr; // hash table full in this chain
}

// 26-connected neighbors (all combinations of -1,0,1 except 0,0,0)
static const int g_neighbors[26][3] = {
    {-1,-1,-1},{0,-1,-1},{1,-1,-1},{-1,0,-1},{0,0,-1},{1,0,-1},{-1,1,-1},{0,1,-1},{1,1,-1},
    {-1,-1, 0},{0,-1, 0},{1,-1, 0},{-1,0, 0},        {1,0, 0},{-1,1, 0},{0,1, 0},{1,1, 0},
    {-1,-1, 1},{0,-1, 1},{1,-1, 1},{-1,0, 1},{0,0, 1},{1,0, 1},{-1,1, 1},{0,1, 1},{1,1, 1}
};

// Check line-of-sight on the grid using 3D Bresenham
static bool GridLineOfSight(int x0, int y0, int z0, int x1, int y1, int z1, float minClearance) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), dz = abs(z1 - z0);
    int sx = (x1 > x0) ? 1 : -1;
    int sy = (y1 > y0) ? 1 : -1;
    int sz = (z1 > z0) ? 1 : -1;

    int maxD = dx;
    if (dy > maxD) maxD = dy;
    if (dz > maxD) maxD = dz;
    if (maxD == 0) return true;

    // Parametric stepping
    float stepX = (float)dx / (float)maxD;
    float stepY = (float)dy / (float)maxD;
    float stepZ = (float)dz / (float)maxD;

    float fx = (float)x0, fy = (float)y0, fz = (float)z0;
    for (int i = 0; i <= maxD; i++) {
        int cx = (int)(fx + 0.5f), cy = (int)(fy + 0.5f), cz = (int)(fz + 0.5f);
        if (!GridInBounds(cx, cy, cz)) return false;
        if (g_grid.data[GridIndex(cx, cy, cz)] < minClearance) return false;
        fx += stepX * sx;
        fy += stepY * sy;
        fz += stepZ * sz;
    }
    return true;
}

// Closed set for parent tracking during path extraction
struct ClosedNode {
    int x, y, z;
    int parentIdx; // -1 = root
};

static ClosedNode g_closedSet[THETA_MAX_CLOSED];
static int g_closedCount = 0;

int PathGrid_FindPath(const float from[3], const float to[3],
                      float outWaypoints[][3], int maxWaypoints,
                      float clearanceBias, float minClearance) {
    if (!g_grid.ready) return 0;

    int sx, sy, sz, gx, gy, gz;
    WorldToGrid(from[0], from[1], from[2], sx, sy, sz);
    WorldToGrid(to[0], to[1], to[2], gx, gy, gz);

    // Clamp to grid
    if (sx < 0) sx = 0; if (sx >= g_grid.dimX) sx = g_grid.dimX - 1;
    if (sy < 0) sy = 0; if (sy >= g_grid.dimY) sy = g_grid.dimY - 1;
    if (sz < 0) sz = 0; if (sz >= g_grid.dimZ) sz = g_grid.dimZ - 1;
    if (gx < 0) gx = 0; if (gx >= g_grid.dimX) gx = g_grid.dimX - 1;
    if (gy < 0) gy = 0; if (gy >= g_grid.dimY) gy = g_grid.dimY - 1;
    if (gz < 0) gz = 0; if (gz >= g_grid.dimZ) gz = g_grid.dimZ - 1;

    // If start or goal is in a wall, find nearest navigable cell
    auto findNavigable = [&](int& cx, int& cy, int& cz) {
        if (GridInBounds(cx, cy, cz) && g_grid.data[GridIndex(cx, cy, cz)] >= minClearance)
            return true;
        // Spiral search
        for (int r = 1; r <= 5; r++) {
            for (int dz2 = -r; dz2 <= r; dz2++) {
                for (int dy2 = -r; dy2 <= r; dy2++) {
                    for (int dx2 = -r; dx2 <= r; dx2++) {
                        int nx = cx + dx2, ny = cy + dy2, nz = cz + dz2;
                        if (GridInBounds(nx, ny, nz) && g_grid.data[GridIndex(nx, ny, nz)] >= minClearance) {
                            cx = nx; cy = ny; cz = nz;
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    };

    if (!findNavigable(sx, sy, sz)) return 0;
    if (!findNavigable(gx, gy, gz)) return 0;

    // Same cell?
    if (sx == gx && sy == gy && sz == gz) {
        GridToWorld(gx, gy, gz, outWaypoints[0][0], outWaypoints[0][1], outWaypoints[0][2]);
        return 1;
    }

    // Allocate visit map on heap to avoid stack overflow (~260KB)
    VisitMap* visited = (VisitMap*)calloc(1, sizeof(VisitMap));
    if (!visited) return 0;

    ThetaHeap open;
    open.count = 0;
    g_closedCount = 0;

    // Heuristic: octile distance
    auto heuristic = [&](int x, int y, int z) -> float {
        float dx2 = (float)abs(x - gx);
        float dy2 = (float)abs(y - gy);
        float dz2 = (float)abs(z - gz);
        // Chebyshev-ish
        float maxD = dx2; if (dy2 > maxD) maxD = dy2; if (dz2 > maxD) maxD = dz2;
        float minD = dx2; if (dy2 < minD) minD = dy2; if (dz2 < minD) minD = dz2;
        float midD = dx2 + dy2 + dz2 - maxD - minD;
        return (maxD + (1.4142f - 1.0f) * midD + (1.7321f - 1.4142f) * minD) * g_grid.resolution;
    };

    // Start node
    g_closedSet[0] = { sx, sy, sz, -1 };
    g_closedCount = 1;

    ThetaNode startNode;
    startNode.x = sx; startNode.y = sy; startNode.z = sz;
    startNode.parentIdx = 0; // self
    startNode.g = 0.0f;
    startNode.f = heuristic(sx, sy, sz);
    HeapPush(open, startNode);

    VisitEntry* startVisit = VisitInsert(*visited, PackCoord(sx, sy, sz));
    if (startVisit) { startVisit->g = 0.0f; startVisit->parentIdx = 0; }

    int goalClosedIdx = -1;
    int expansions = 0;

    while (open.count > 0 && expansions < THETA_MAX_OPEN) {
        ThetaNode cur = HeapPop(open);
        expansions++;

        // Goal check
        if (cur.x == gx && cur.y == gy && cur.z == gz) {
            goalClosedIdx = cur.parentIdx;
            // Add goal to closed set
            if (g_closedCount < THETA_MAX_CLOSED) {
                g_closedSet[g_closedCount] = { gx, gy, gz, cur.parentIdx };
                goalClosedIdx = g_closedCount;
                g_closedCount++;
            }
            break;
        }

        // Already have a better path?
        int curKey = PackCoord(cur.x, cur.y, cur.z);
        VisitEntry* curVisit = VisitFind(*visited, curKey);
        if (curVisit && cur.g > curVisit->g + 0.01f) continue;

        // Get parent for Theta* LOS check
        int curClosedIdx = curVisit ? curVisit->parentIdx : cur.parentIdx;
        int parentClosedIdx = g_closedSet[curClosedIdx].parentIdx;
        if (parentClosedIdx < 0) parentClosedIdx = curClosedIdx; // root

        int px = g_closedSet[parentClosedIdx].x;
        int py = g_closedSet[parentClosedIdx].y;
        int pz = g_closedSet[parentClosedIdx].z;

        // Expand 26 neighbors
        for (int n = 0; n < 26; n++) {
            int nx = cur.x + g_neighbors[n][0];
            int ny = cur.y + g_neighbors[n][1];
            int nz = cur.z + g_neighbors[n][2];

            if (!GridInBounds(nx, ny, nz)) continue;

            float nClearance = g_grid.data[GridIndex(nx, ny, nz)];
            if (nClearance < minClearance) continue;

            // Clearance-weighted cost: distance * (1 + bias / clearance)
            float clearanceCost = 1.0f + clearanceBias / (nClearance + 0.1f);

            // Theta*: try direct path from parent
            float newG;
            int newParent;
            float stepDist = sqrtf((float)(g_neighbors[n][0] * g_neighbors[n][0] +
                                           g_neighbors[n][1] * g_neighbors[n][1] +
                                           g_neighbors[n][2] * g_neighbors[n][2])) * g_grid.resolution;

            if (parentClosedIdx != curClosedIdx &&
                GridLineOfSight(px, py, pz, nx, ny, nz, minClearance)) {
                // Path 2 (Theta*): parent -> neighbor directly
                float dx2 = (float)(nx - px) * g_grid.resolution;
                float dy2 = (float)(ny - py) * g_grid.resolution;
                float dz2 = (float)(nz - pz) * g_grid.resolution;
                float directDist = sqrtf(dx2*dx2 + dy2*dy2 + dz2*dz2);

                // Find parent's g cost
                VisitEntry* parentVisit = VisitFind(*visited, PackCoord(px, py, pz));
                float parentG = parentVisit ? parentVisit->g : 0.0f;
                newG = parentG + directDist * clearanceCost;
                newParent = parentClosedIdx;
            } else {
                // Path 1 (A*): current -> neighbor
                newG = cur.g + stepDist * clearanceCost;
                newParent = curClosedIdx;
            }

            int nKey = PackCoord(nx, ny, nz);
            VisitEntry* nVisit = VisitFind(*visited, nKey);

            if (nVisit && newG >= nVisit->g) continue;

            // Add to closed set
            int nClosedIdx = -1;
            if (g_closedCount < THETA_MAX_CLOSED) {
                nClosedIdx = g_closedCount;
                g_closedSet[g_closedCount] = { nx, ny, nz, newParent };
                g_closedCount++;
            } else {
                // Closed set full — stop search
                break;
            }

            VisitEntry* nEntry = VisitInsert(*visited, nKey);
            if (nEntry) {
                nEntry->g = newG;
                nEntry->parentIdx = nClosedIdx;
            }

            ThetaNode next;
            next.x = nx; next.y = ny; next.z = nz;
            next.parentIdx = nClosedIdx;
            next.g = newG;
            next.f = newG + heuristic(nx, ny, nz);
            HeapPush(open, next);
        }
    }

    free(visited);

    if (goalClosedIdx < 0) {
        // No complete path — find best partial (closest to goal in closed set)
        float bestDist = FLT_MAX;
        int bestIdx = -1;
        for (int i = 0; i < g_closedCount; i++) {
            float dx2 = (float)(g_closedSet[i].x - gx);
            float dy2 = (float)(g_closedSet[i].y - gy);
            float dz2 = (float)(g_closedSet[i].z - gz);
            float d = dx2*dx2 + dy2*dy2 + dz2*dz2;
            if (d < bestDist) {
                bestDist = d;
                bestIdx = i;
            }
        }
        if (bestIdx < 0) return 0;
        goalClosedIdx = bestIdx;
        std::cout << "[PathGrid] Partial path (best effort, " << expansions << " expansions)\n";
    } else {
        std::cout << "[PathGrid] Path found (" << expansions << " expansions)\n";
    }

    // Extract path by walking parent pointers
    int pathIndices[PATHGRID_MAX_WAYPOINTS];
    int pathCount = 0;
    int idx = goalClosedIdx;
    while (idx >= 0 && pathCount < PATHGRID_MAX_WAYPOINTS) {
        pathIndices[pathCount++] = idx;
        int nextIdx = g_closedSet[idx].parentIdx;
        if (nextIdx == idx) break; // root
        idx = nextIdx;
    }

    // Reverse into forward order, convert to world coords
    int outCount = 0;
    for (int i = pathCount - 1; i >= 0 && outCount < maxWaypoints; i--) {
        int ci = pathIndices[i];
        GridToWorld(g_closedSet[ci].x, g_closedSet[ci].y, g_closedSet[ci].z,
                    outWaypoints[outCount][0], outWaypoints[outCount][1], outWaypoints[outCount][2]);
        outCount++;
    }

    return outCount;
}

// ============================================================================
// Clamped Cubic B-Spline
// ============================================================================

static float g_splineCP[PATHGRID_MAX_SPLINE_CP][3];
static int   g_splineCPCount = 0;
static float g_splineLength = 0.0f;

// Basis function for clamped cubic B-spline (de Boor simplified for cubic)
static void BSplineBasis(float t, int span, int n, float* N) {
    // Clamped knot vector: 0,0,0,0, 1,2,...,n-3, n-3,n-3,n-3,n-3
    // We use the Cox-de Boor simplified evaluation via de Boor's algorithm
    // For simplicity, use direct matrix evaluation per segment
    (void)t; (void)span; (void)n; (void)N;
}

// Evaluate cubic B-spline with clamped knot vector at parameter u (0..1)
static void EvalBSpline(const float cp[][3], int cpCount, float u, float out[3]) {
    if (cpCount < 4) {
        // Not enough control points — linear interpolation
        if (cpCount == 1) {
            out[0] = cp[0][0]; out[1] = cp[0][1]; out[2] = cp[0][2];
            return;
        }
        float t = u * (float)(cpCount - 1);
        int seg = (int)t;
        if (seg >= cpCount - 1) seg = cpCount - 2;
        float frac = t - (float)seg;
        out[0] = cp[seg][0] + frac * (cp[seg+1][0] - cp[seg][0]);
        out[1] = cp[seg][1] + frac * (cp[seg+1][1] - cp[seg][1]);
        out[2] = cp[seg][2] + frac * (cp[seg+1][2] - cp[seg][2]);
        return;
    }

    // Number of spans = cpCount - 3
    int spans = cpCount - 3;
    float scaled = u * (float)spans;
    int seg = (int)scaled;
    if (seg >= spans) seg = spans - 1;
    if (seg < 0) seg = 0;
    float t = scaled - (float)seg;

    // Cubic B-spline basis matrix
    float t2 = t * t;
    float t3 = t2 * t;

    float b0 = (-t3 + 3.0f*t2 - 3.0f*t + 1.0f) / 6.0f;
    float b1 = (3.0f*t3 - 6.0f*t2 + 4.0f) / 6.0f;
    float b2 = (-3.0f*t3 + 3.0f*t2 + 3.0f*t + 1.0f) / 6.0f;
    float b3 = t3 / 6.0f;

    int i0 = seg, i1 = seg + 1, i2 = seg + 2, i3 = seg + 3;
    out[0] = b0*cp[i0][0] + b1*cp[i1][0] + b2*cp[i2][0] + b3*cp[i3][0];
    out[1] = b0*cp[i0][1] + b1*cp[i1][1] + b2*cp[i2][1] + b3*cp[i3][1];
    out[2] = b0*cp[i0][2] + b1*cp[i1][2] + b2*cp[i2][2] + b3*cp[i3][2];
}

bool PathGrid_FitSpline(const float waypoints[][3], int waypointCount,
                        float minClearance) {
    if (waypointCount < 2) return false;

    // Build initial control points — duplicate first and last for clamping
    g_splineCPCount = 0;

    // Clamp start: duplicate first point
    if (g_splineCPCount < PATHGRID_MAX_SPLINE_CP) {
        memcpy(g_splineCP[g_splineCPCount], waypoints[0], sizeof(float) * 3);
        g_splineCPCount++;
    }

    // Copy all waypoints
    for (int i = 0; i < waypointCount && g_splineCPCount < PATHGRID_MAX_SPLINE_CP; i++) {
        memcpy(g_splineCP[g_splineCPCount], waypoints[i], sizeof(float) * 3);
        g_splineCPCount++;
    }

    // Clamp end: duplicate last point
    if (g_splineCPCount < PATHGRID_MAX_SPLINE_CP) {
        memcpy(g_splineCP[g_splineCPCount], waypoints[waypointCount - 1], sizeof(float) * 3);
        g_splineCPCount++;
    }

    // Collision validation: sample at 0.5 unit intervals, fix violations
    for (int iteration = 0; iteration < 3; iteration++) {
        bool anyViolation = false;

        // Compute arc length for sampling
        float arcLen = 0.0f;
        float prev[3];
        EvalBSpline(g_splineCP, g_splineCPCount, 0.0f, prev);

        for (int s = 1; s <= 100; s++) {
            float u = (float)s / 100.0f;
            float cur[3];
            EvalBSpline(g_splineCP, g_splineCPCount, u, cur);
            float dx = cur[0] - prev[0], dy = cur[1] - prev[1], dz = cur[2] - prev[2];
            arcLen += sqrtf(dx*dx + dy*dy + dz*dz);
            prev[0] = cur[0]; prev[1] = cur[1]; prev[2] = cur[2];
        }

        int sampleCount = (int)(arcLen / 0.5f) + 1;
        if (sampleCount > 500) sampleCount = 500;

        // Check each sample
        float worstClearance = FLT_MAX;
        float worstU = 0.0f;
        float worstPos[3] = {};

        for (int s = 0; s <= sampleCount; s++) {
            float u = (float)s / (float)sampleCount;
            float pos[3];
            EvalBSpline(g_splineCP, g_splineCPCount, u, pos);

            float clr = PathGrid_GetClearance(pos[0], pos[1], pos[2]);
            if (clr < minClearance && clr < worstClearance) {
                worstClearance = clr;
                worstU = u;
                worstPos[0] = pos[0]; worstPos[1] = pos[1]; worstPos[2] = pos[2];
                anyViolation = true;
            }
        }

        if (!anyViolation) break;

        // Find clearance gradient direction at worst point and push control point
        // Use finite differences on clearance grid
        float step = g_grid.resolution;
        float gradX = PathGrid_GetClearance(worstPos[0] + step, worstPos[1], worstPos[2])
                     - PathGrid_GetClearance(worstPos[0] - step, worstPos[1], worstPos[2]);
        float gradY = PathGrid_GetClearance(worstPos[0], worstPos[1] + step, worstPos[2])
                     - PathGrid_GetClearance(worstPos[0], worstPos[1] - step, worstPos[2]);
        float gradZ = PathGrid_GetClearance(worstPos[0], worstPos[1], worstPos[2] + step)
                     - PathGrid_GetClearance(worstPos[0], worstPos[1], worstPos[2] - step);

        float gradLen = sqrtf(gradX*gradX + gradY*gradY + gradZ*gradZ);
        if (gradLen < 0.001f) break; // no gradient info

        gradX /= gradLen; gradY /= gradLen; gradZ /= gradLen;

        // Push amount: enough to reach minClearance
        float pushDist = (minClearance - worstClearance) + 0.5f;

        // Find nearest control point to worstU and push it
        int nearestCP = 1; // skip first clamped duplicate
        float bestDist = FLT_MAX;
        for (int i = 1; i < g_splineCPCount - 1; i++) {
            float dx = g_splineCP[i][0] - worstPos[0];
            float dy = g_splineCP[i][1] - worstPos[1];
            float dz = g_splineCP[i][2] - worstPos[2];
            float d = sqrtf(dx*dx + dy*dy + dz*dz);
            if (d < bestDist) { bestDist = d; nearestCP = i; }
        }

        g_splineCP[nearestCP][0] += gradX * pushDist;
        g_splineCP[nearestCP][1] += gradY * pushDist;
        g_splineCP[nearestCP][2] += gradZ * pushDist;
    }

    // Compute final arc length
    g_splineLength = 0.0f;
    float prev2[3];
    EvalBSpline(g_splineCP, g_splineCPCount, 0.0f, prev2);
    for (int s = 1; s <= 200; s++) {
        float u = (float)s / 200.0f;
        float cur[3];
        EvalBSpline(g_splineCP, g_splineCPCount, u, cur);
        float dx = cur[0] - prev2[0], dy = cur[1] - prev2[1], dz = cur[2] - prev2[2];
        g_splineLength += sqrtf(dx*dx + dy*dy + dz*dz);
        prev2[0] = cur[0]; prev2[1] = cur[1]; prev2[2] = cur[2];
    }

    return true;
}

void PathGrid_EvalSpline(float u, float outPos[3]) {
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    EvalBSpline(g_splineCP, g_splineCPCount, u, outPos);
}

float PathGrid_GetSplineLength() {
    return g_splineLength;
}

float PathGrid_GetSplineClearance(float u) {
    float pos[3];
    PathGrid_EvalSpline(u, pos);
    return PathGrid_GetClearance(pos[0], pos[1], pos[2]);
}

float PathGrid_GetAreaClearance(float x, float y, float z, float radius) {
    if (!g_grid.ready) return 0.0f;

    // 27 sample points: center + 6 axis + 8 corners + 12 edge midpoints
    static const float offsets[27][3] = {
        // Center
        { 0, 0, 0 },
        // 6 axis-aligned
        { 1, 0, 0 }, {-1, 0, 0 }, { 0, 1, 0 }, { 0,-1, 0 }, { 0, 0, 1 }, { 0, 0,-1 },
        // 8 corners (normalized)
        { 0.5774f, 0.5774f, 0.5774f }, {-0.5774f, 0.5774f, 0.5774f },
        { 0.5774f,-0.5774f, 0.5774f }, {-0.5774f,-0.5774f, 0.5774f },
        { 0.5774f, 0.5774f,-0.5774f }, {-0.5774f, 0.5774f,-0.5774f },
        { 0.5774f,-0.5774f,-0.5774f }, {-0.5774f,-0.5774f,-0.5774f },
        // 12 edge midpoints
        { 0.7071f, 0.7071f, 0 }, {-0.7071f, 0.7071f, 0 },
        { 0.7071f,-0.7071f, 0 }, {-0.7071f,-0.7071f, 0 },
        { 0.7071f, 0, 0.7071f }, {-0.7071f, 0, 0.7071f },
        { 0.7071f, 0,-0.7071f }, {-0.7071f, 0,-0.7071f },
        { 0, 0.7071f, 0.7071f }, { 0,-0.7071f, 0.7071f },
        { 0, 0.7071f,-0.7071f }, { 0,-0.7071f,-0.7071f }
    };

    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < 27; i++) {
        float sx = x + offsets[i][0] * radius;
        float sy = y + offsets[i][1] * radius;
        float sz = z + offsets[i][2] * radius;
        float c = PathGrid_GetClearance(sx, sy, sz);
        sum += c;
        count++;
    }
    return (count > 0) ? (sum / (float)count) : 0.0f;
}
