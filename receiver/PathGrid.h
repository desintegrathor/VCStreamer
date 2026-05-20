#pragma once

// Clearance Grid + Theta* Pathfinding + B-Spline Smoothing
// Replaces RRT pathfinding for the drone camera.

// Max waypoints from Theta* search
static constexpr int PATHGRID_MAX_WAYPOINTS = 64;

// Max B-spline control points (may be more than waypoints after collision insertion)
static constexpr int PATHGRID_MAX_SPLINE_CP = 96;

// Build the clearance grid from the loaded OctCollision map.
// Call after OctCollision_LoadCurrentMap() succeeds.
// resolution: voxel size in world units (1.0 recommended)
void PathGrid_Build(float resolution);

// Free clearance grid memory
void PathGrid_Destroy();

// Returns true if grid is built and ready
bool PathGrid_IsReady();

// Query clearance (distance to nearest wall) at world position.
// Returns 0 if outside grid or grid not built.
float PathGrid_GetClearance(float x, float y, float z);

// Theta* pathfinding from world position 'from' to 'to'.
// Writes waypoints into outWaypoints, returns count (0 = no path).
// clearanceBias: higher = prefer corridor centers (3.0 recommended)
// minClearance: minimum clearance a voxel must have to be navigable
int PathGrid_FindPath(const float from[3], const float to[3],
                      float outWaypoints[][3], int maxWaypoints,
                      float clearanceBias, float minClearance);

// Fit a clamped cubic B-spline through waypoints with collision validation.
// waypoints: input control points
// waypointCount: number of input points
// minClearance: minimum wall distance for spline samples
// Returns true if spline is valid (stored internally for EvalSpline).
bool PathGrid_FitSpline(const float waypoints[][3], int waypointCount,
                        float minClearance);

// Evaluate the fitted B-spline at parameter u (0..1 over total length).
// Returns position in outPos.
void PathGrid_EvalSpline(float u, float outPos[3]);

// Get total arc length of fitted spline (for speed control)
float PathGrid_GetSplineLength();

// Get clearance at a spline parameter u (for adaptive speed)
float PathGrid_GetSplineClearance(float u);

// Sample average clearance in a sphere around a world position.
// Used for drone go/no-go decisions (open area vs tight corridor).
// radius: sampling sphere radius in world units
// Returns average clearance across sample points (0 if grid not ready).
float PathGrid_GetAreaClearance(float x, float y, float z, float radius);
