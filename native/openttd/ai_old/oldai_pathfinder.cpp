/*
 * Modelled-terrain rail pathfinder for oldai.cpp.
 *
 * This file is intentionally an include fragment, not a separately compiled
 * translation unit.  Include it in oldai.cpp after TileLevelable(),
 * DiagDirBetween(), TrackForEdges(), OL(), and OLn() have been defined.  It
 * uses the OpenTTD headers already included by oldai.cpp.
 *
 * C++03, fixed arrays only, no allocation and no exceptions.
 */

enum RailStepKind {
	RAILSTEP_LEVEL  = 0,
	RAILSTEP_TRACK  = 1,
	RAILSTEP_BRIDGE = 2
};

/* LEVEL:  tile=tile, value=absolute target height.
 * TRACK:  tile=tile, value=Track (0..5).
 * BRIDGE: tile=near head, other=far head, value=bridge id. */
struct RailStep {
	byte kind;
	TileIndex tile;
	TileIndex other;
	int value;
};

enum RoadStepKind {
	ROADSTEP_LEVEL  = 0,
	ROADSTEP_ROAD   = 1,
	ROADSTEP_BRIDGE = 2
};

/* LEVEL:  tile=first corner, other=opposite corner, low byte=target height.
 * ROAD:   tile=tile, low nibble=bits added, bits 8..11=pre-attempt bits.
 * BRIDGE: tile=near head, other=far head, low byte=bridge id.
 * Bit 15 records that ExecuteRoadPlan really built this object.  It prevents
 * cleanup from removing a road/bridge which appeared between plan and execute. */
struct RoadStep {
	TileIndex tile;
	TileIndex other;
	uint16 data;
	byte kind;
	byte unused;
};
typedef char RoadStepMustStay12Bytes[(sizeof(RoadStep) == 12) ? 1 : -1];

enum {
	PF_NODE_BUDGET = 12288,
	PF_HASH_SIZE = 32768,       /* must be a power of two */
	PF_REVERSE_CAPACITY = 1024,
	PF_MAX_BRIDGE_WATER = 16,
	PF_STATION_CORNER_CAPACITY = 32, /* two 1x7 strips have 16 corners each */

	/* The road search borrows the first part of the rail scratch arrays.  Rail
	 * and road planning never overlap, so these limits add no second A* arena. */
	RP_NODE_BUDGET = 4096,
	RP_HASH_SIZE = 8192,        /* must be a power of two */
	RP_REVERSE_CAPACITY = 256,
	RP_MAX_BRIDGE_WATER = 16
};

enum PFTerrainMode {
	PF_TERRAIN_NATURAL = 0,
	PF_TERRAIN_LEVELLED = 1,
	PF_TERRAIN_BRIDGE_HEAD = 2
};

enum PFAction {
	PF_ACTION_NONE = 0,
	PF_ACTION_TRACK,
	PF_ACTION_BRIDGE,
	PF_ACTION_LEAVE_BRIDGE
};

struct PFNode {
	TileIndex tile;
	uint32 g;
	int16 parent;       /* -1 or a node index; PF_NODE_BUDGET fits in int16 */
	byte in_dir;       /* direction travelled from parent to this tile */
	byte height;       /* modelled TileHeight / absolute level target */
	byte terrain;
	byte action_grade; /* low 2 bits action; bit 2 means pending grade */
	byte track;        /* valid for PF_ACTION_TRACK */
	byte bridge_id;    /* valid for PF_ACTION_BRIDGE */
};
typedef char PFNodeMustStay16Bytes[(sizeof(PFNode) == 16) ? 1 : -1];

static PFNode _pf_nodes[PF_NODE_BUDGET];
static int _pf_node_count;
static int _pf_hash[PF_HASH_SIZE];       /* node index + 1; zero means empty */
static int _pf_heap[PF_NODE_BUDGET];
static int _pf_heap_count;
static int _pf_reverse[PF_REVERSE_CAPACITY];

static TileIndex _pf_p_start, _pf_a_start;
static byte _pf_p_dir, _pf_a_dir;
static int _pf_a_height;
static TileIndex _pf_station_corner[PF_STATION_CORNER_CAPACITY];
static byte _pf_station_corner_height[PF_STATION_CORNER_CAPACITY];
static int _pf_station_corner_count;

static byte PFReverseDir(byte d)
{
	return (byte)(d ^ 2);
}

static byte PFNodeAction(const PFNode &n)
{
	return (byte)(n.action_grade & 3);
}

static byte PFNodeNeedGrade(const PFNode &n)
{
	return (byte)((n.action_grade >> 2) & 1);
}

static bool PFNeighbour(TileIndex t, byte d, TileIndex *out)
{
	int x = (int)TileX(t);
	int y = (int)TileY(t);
	if (d == DIAGDIR_NE) x--;
	else if (d == DIAGDIR_SE) y++;
	else if (d == DIAGDIR_SW) x++;
	else if (d == DIAGDIR_NW) y--;
	else return false;
	if (x < 1 || y < 1 || x >= (int)MapMaxX() || y >= (int)MapMaxY()) return false;
	*out = TileXY(x, y);
	return IsValidTile(*out);
}

/* CMD_LEVEL_LAND is corner based.  LevelStationFootprint executes the 2x8
 * corner rectangle belonging to seven collinear tiles at each end: inner exit,
 * five platform tiles, outer exit.  Store exactly those 16 future corner
 * heights so every terrain read during the free plan sees the post-station map.
 *
 * OpenTTD stores TileHeight(tile) at the tile's north corner.  The other three
 * corner-height entries are at (x + 1,y)=W, (x,y + 1)=E, and
 * (x + 1,y + 1)=S; slope bits are W=1, S=2, E=4, N=8. */
static bool PFSetStationCorner(TileIndex corner, int height)
{
	for (int i = 0; i < _pf_station_corner_count; i++) {
		if (_pf_station_corner[i] != corner) continue;
		return _pf_station_corner_height[i] == (byte)height;
	}
	if (_pf_station_corner_count >= PF_STATION_CORNER_CAPACITY) return false;
	_pf_station_corner[_pf_station_corner_count] = corner;
	_pf_station_corner_height[_pf_station_corner_count] = (byte)height;
	_pf_station_corner_count++;
	return true;
}

static bool PFOverlayLevelTile(TileIndex tile, int height)
{
	int x = (int)TileX(tile), y = (int)TileY(tile);
	return PFSetStationCorner(TileXY(x,     y),     height) &&
		PFSetStationCorner(TileXY(x + 1, y),     height) &&
		PFSetStationCorner(TileXY(x,     y + 1), height) &&
		PFSetStationCorner(TileXY(x + 1, y + 1), height);
}

static bool PFOverlayStationStrip(TileIndex inner_exit, byte outward_dir, int height)
{
	TileIndex tile = inner_exit;
	byte inward_dir = PFReverseDir(outward_dir);
	for (int i = 0; i < 7; i++) {
		if (!PFOverlayLevelTile(tile, height)) return false;
		if (i != 6 && !PFNeighbour(tile, inward_dir, &tile)) return false;
	}
	return true;
}

static bool PFPrepareStationOverlay(TileIndex p_start, byte p_dir, int p_height,
		TileIndex a_start, byte a_dir, int a_height)
{
	_pf_station_corner_count = 0;
	/* Conflicting corner heights mean the two real levelled strips could not
	 * both have the post-state claimed by this model.  Reject the pair for free. */
	return PFOverlayStationStrip(p_start, p_dir, p_height) &&
		PFOverlayStationStrip(a_start, a_dir, a_height);
}

static bool PFStationCornersTouch(TileIndex tile)
{
	int x = (int)TileX(tile), y = (int)TileY(tile);
	TileIndex corners[4] = {
		TileXY(x, y), TileXY(x + 1, y), TileXY(x, y + 1), TileXY(x + 1, y + 1)
	};
	for (int c = 0; c < 4; c++) {
		for (int i = 0; i < _pf_station_corner_count; i++) {
			if (_pf_station_corner[i] == corners[c]) return true;
		}
	}
	return false;
}

static int PFStationCornerHeight(int x, int y)
{
	TileIndex corner = TileXY(x, y);
	for (int i = 0; i < _pf_station_corner_count; i++) {
		if (_pf_station_corner[i] == corner) return (int)_pf_station_corner_height[i];
	}
	return (int)TileHeight(corner);
}

static int PFPostStationTileHeight(TileIndex tile)
{
	return PFStationCornerHeight((int)TileX(tile), (int)TileY(tile));
}

static int PFPostStationSlope(TileIndex tile)
{
	/* Preserve the game's own exact slope result everywhere the station commands
	 * cannot change a corner.  Only reconstruct tiles touched by the overlay. */
	if (!PFStationCornersTouch(tile)) return (int)GetTileSlope(tile, NULL);
	int x = (int)TileX(tile), y = (int)TileY(tile);
	int hn = PFStationCornerHeight(x,     y);
	int hw = PFStationCornerHeight(x + 1, y);
	int he = PFStationCornerHeight(x,     y + 1);
	int hs = PFStationCornerHeight(x + 1, y + 1);
	int min_h = hn;
	if (hw < min_h) min_h = hw;
	if (he < min_h) min_h = he;
	if (hs < min_h) min_h = hs;
	int max_h = hn;
	if (hw > max_h) max_h = hw;
	if (he > max_h) max_h = he;
	if (hs > max_h) max_h = hs;
	int slope = 0;
	if (hw > min_h) slope |= SLOPE_W;
	if (hs > min_h) slope |= SLOPE_S;
	if (he > min_h) slope |= SLOPE_E;
	if (hn > min_h) slope |= SLOPE_N;
	if (max_h - min_h > 1) slope |= 16; /* SLOPE_STEEP in OpenTTD 1.0.x */
	return slope;
}

/* Planning happens before station construction.  Reserve the future platform
 * tiles plus the opposite exits and producer depot tile so A* cannot use them
 * as a shortcut.  Starting at the inner exit, the five platform tiles are
 * steps 1..5 inward, the outer exit is step 6, and the producer depot is 7. */
static bool PFReservedPlatform(TileIndex t)
{
	TileIndex q = _pf_p_start;
	byte d = PFReverseDir(_pf_p_dir);
	for (int i = 0; i < 7; i++) {
		if (!PFNeighbour(q, d, &q)) break;
		if (q == t) return true;
	}
	q = _pf_a_start;
	d = PFReverseDir(_pf_a_dir);
	for (int i = 0; i < 6; i++) {
		if (!PFNeighbour(q, d, &q)) break;
		if (q == t) return true;
	}
	return false;
}

static uint32 PFTrackBit(byte track)
{
	return (uint32)1 << track;
}

/* Exact CheckRailSlope masks from OpenTTD 1.0.5 rail_cmd.cpp. */
static const byte _pf_tracks_without_foundation[15] = {
	0x3F, 0x20, 0x04, 0x01, 0x10, 0x00, 0x02, 0x08,
	0x08, 0x02, 0x00, 0x10, 0x01, 0x04, 0x20
};
static const byte _pf_tracks_on_foundation[15] = {
	0x00, 0x10, 0x08, 0x1A, 0x20, 0x3F, 0x29, 0x3F,
	0x04, 0x15, 0x3F, 0x3F, 0x26, 0x3F, 0x3F
};

static bool PFTrackLegal(TileIndex tile, byte terrain, byte track, bool *foundation)
{
	int slope;
	if (terrain == PF_TERRAIN_LEVELLED) {
		slope = 0;
	} else if (terrain == PF_TERRAIN_NATURAL) {
		slope = PFPostStationSlope(tile);
	} else {
		return false; /* a bridge command, not SINGLE_RAIL, owns this tile */
	}
	if (slope < 0 || slope >= 15) return false; /* steep slopes must be levelled */
	byte bit = (byte)PFTrackBit(track);
	if ((_pf_tracks_without_foundation[slope] & bit) != 0) {
		if (foundation != NULL) *foundation = false;
		return true;
	}
	if ((_pf_tracks_on_foundation[slope] & bit) != 0) {
		if (foundation != NULL) *foundation = true;
		return true;
	}
	return false;
}

static uint32 PFHashValue(TileIndex tile, byte in_dir, byte height, byte terrain, byte need_grade)
{
	uint32 h = (uint32)tile * 2654435761U;
	h ^= (uint32)in_dir * 0x9E3779B9U;
	h ^= (uint32)height * 0x85EBCA6BU;
	h ^= (uint32)terrain * 0xC2B2AE35U;
	h ^= (uint32)need_grade * 0x27D4EB2FU;
	return h & (PF_HASH_SIZE - 1);
}

static bool PFSameState(const PFNode &n, TileIndex tile, byte in_dir, byte height, byte terrain, byte need_grade)
{
	return n.tile == tile && n.in_dir == in_dir && n.height == height &&
		n.terrain == terrain && PFNodeNeedGrade(n) == need_grade;
}

/* Returns a hash slot.  If found is true the slot contains this state. */
static int PFFindSlot(TileIndex tile, byte in_dir, byte height, byte terrain, byte need_grade, bool *found)
{
	uint32 slot = PFHashValue(tile, in_dir, height, terrain, need_grade);
	for (int probe = 0; probe < PF_HASH_SIZE; probe++) {
		int v = _pf_hash[slot];
		if (v == 0) { *found = false; return (int)slot; }
		if (PFSameState(_pf_nodes[v - 1], tile, in_dir, height, terrain, need_grade)) {
			*found = true; return (int)slot;
		}
		slot = (slot + 1) & (PF_HASH_SIZE - 1);
	}
	*found = false;
	return -1;
}

static uint32 PFHeuristic(TileIndex tile, int height);

static bool PFHeapLess(int a, int b)
{
	uint32 af = _pf_nodes[a].g + PFHeuristic(_pf_nodes[a].tile, _pf_nodes[a].height);
	uint32 bf = _pf_nodes[b].g + PFHeuristic(_pf_nodes[b].tile, _pf_nodes[b].height);
	if (af != bf) return af < bf;
	return _pf_nodes[a].g > _pf_nodes[b].g; /* on ties, make progress toward goal */
}

static bool PFHeapPush(int node)
{
	if (_pf_heap_count >= PF_NODE_BUDGET) return false;
	int p = _pf_heap_count++;
	while (p > 0) {
		int up = (p - 1) >> 1;
		if (!PFHeapLess(node, _pf_heap[up])) break;
		_pf_heap[p] = _pf_heap[up];
		p = up;
	}
	_pf_heap[p] = node;
	return true;
}

static int PFHeapPop()
{
	if (_pf_heap_count == 0) return -1;
	int answer = _pf_heap[0];
	int tail = _pf_heap[--_pf_heap_count];
	if (_pf_heap_count != 0) {
		int p = 0;
		for (;;) {
			int left = p * 2 + 1;
			if (left >= _pf_heap_count) break;
			int child = left;
			if (left + 1 < _pf_heap_count && PFHeapLess(_pf_heap[left + 1], _pf_heap[left])) child++;
			if (!PFHeapLess(_pf_heap[child], tail)) break;
			_pf_heap[p] = _pf_heap[child];
			p = child;
		}
		_pf_heap[p] = tail;
	}
	return answer;
}

static uint32 PFHeuristic(TileIndex tile, int height)
{
	uint32 d = DistanceManhattan(tile, _pf_a_start);
	int dh = height - _pf_a_height;
	if (dh < 0) dh = -dh;
	/* Weighted A*: route cost optimality is not required here.  The extra goal
	 * pull sharply reduces sideways exploration on the 24..64 tile routes. */
	return d * 14 + (uint32)dh * 3;
}

static bool PFInAncestry(int node, TileIndex tile)
{
	for (int n = node; n >= 0; n = _pf_nodes[n].parent) {
		if (_pf_nodes[n].tile == tile) return true;
	}
	return false;
}

static uint32 PFTerrainCost(TileIndex tile, byte terrain, int height)
{
	if (terrain != PF_TERRAIN_LEVELLED) return 0;
	int delta = height - PFPostStationTileHeight(tile);
	if (delta < 0) delta = -delta;
	return 22 + (uint32)delta * 8;
}

static bool PFAddNode(TileIndex tile, byte in_dir, byte height, byte terrain,
		byte need_grade, uint32 g, int parent, byte action, byte track, byte bridge_id)
{
	bool found;
	int slot = PFFindSlot(tile, in_dir, height, terrain, need_grade, &found);
	if (slot < 0) return false;
	if (found && _pf_nodes[_pf_hash[slot] - 1].g <= g) return true;
	if (_pf_node_count >= PF_NODE_BUDGET) return false;
	int index = _pf_node_count++;
	PFNode &n = _pf_nodes[index];
	n.tile = tile;
	n.g = g;
	n.parent = (int16)parent;
	n.in_dir = in_dir;
	n.height = height;
	n.terrain = terrain;
	n.action_grade = (byte)((action & 3) | ((need_grade & 1) << 2));
	n.track = track;
	n.bridge_id = bridge_id;
	_pf_hash[slot] = index + 1;
	return PFHeapPush(index);
}

static bool PFIsCurrentBest(int node)
{
	PFNode &n = _pf_nodes[node];
	bool found;
	int slot = PFFindSlot(n.tile, n.in_dir, n.height, n.terrain, PFNodeNeedGrade(n), &found);
	return found && slot >= 0 && _pf_hash[slot] == node + 1;
}

/* Add natural terrain and the useful levelled heights.  A move changes height
 * by at most one, so no other target height can be part of this edge. */
static bool PFAddLandCandidates(int parent, TileIndex tile, byte move_dir,
		int from_height, bool current_carries_grade,
		uint32 edge_cost, byte action, byte track)
{
	if (!TileLevelable(tile) || PFReservedPlatform(tile) || PFInAncestry(parent, tile)) return true;
	int raw_h = PFPostStationTileHeight(tile);
	int raw_slope = PFPostStationSlope(tile);
	if (raw_h >= 0 && raw_h <= 15 && raw_h - from_height <= 1 && from_height - raw_h <= 1) {
		bool changed = raw_h != from_height;
		byte need_grade = (changed && !current_carries_grade) ? 1 : 0;
		/* If this tile must carry the grade, a flat natural tile cannot do so.
		 * The exact track-axis check happens when its outgoing edge is known. */
		if (!need_grade || raw_slope != SLOPE_FLAT) {
			uint32 grade_cost = (raw_h == from_height) ? 0 : 3;
			if (!PFAddNode(tile, move_dir, (byte)raw_h, PF_TERRAIN_NATURAL,
					need_grade, _pf_nodes[parent].g + edge_cost + grade_cost, parent, action, track, 0)) return false;
		}
	}
	/* A later LEVEL on a tile sharing a station-controlled corner could try to
	 * terraform the already-built station.  Refuse that candidate; untouched
	 * natural track remains allowed and uses the exact overlaid slope above. */
	if (PFStationCornersTouch(tile)) return true;
	for (int h = from_height - 1; h <= from_height + 1; h++) {
		if (h < 0 || h > 15) continue;
		/* Flat raw terrain at this height is the identical, cheaper natural state. */
		if (h == raw_h && raw_slope == SLOPE_FLAT) continue;
		/* A levelled tile is flat.  It cannot be the side which carries an
		 * otherwise-unhandled one-level transition. */
		if (h != from_height && !current_carries_grade) continue;
		uint32 grade_cost = (h == from_height) ? 0 : 3;
		uint32 g = _pf_nodes[parent].g + edge_cost + grade_cost + PFTerrainCost(tile, PF_TERRAIN_LEVELLED, h);
		if (!PFAddNode(tile, move_dir, (byte)h, PF_TERRAIN_LEVELLED,
				0, g, parent, action, track, 0)) return false;
	}
	return true;
}

static bool PFTryBridge(int node, byte d)
{
	PFNode &cur = _pf_nodes[node];
	/* Both bridge heads are owned by CMD_BUILD_BRIDGE.  Testing that exact
	 * command against the untouched map lets the plan store an exact bridge id;
	 * consequently this conservative implementation never terraforms a head. */
	if (cur.terrain != PF_TERRAIN_NATURAL || PFNodeNeedGrade(cur) || cur.parent < 0 || cur.in_dir != d) return true;
	/* DC_NONE below sees the live pre-station map.  It is exact only when neither
	 * bridge head shares a corner changed by the virtual station levelling. */
	if (PFStationCornersTouch(cur.tile)) return true;
	TileIndex probe;
	if (!PFNeighbour(cur.tile, d, &probe) || !IsWaterTile(probe)) return true;
	int water = 0;
	while (water < PF_MAX_BRIDGE_WATER && IsValidTile(probe) && IsWaterTile(probe)) {
		water++;
		if (!PFNeighbour(probe, d, &probe)) return true;
	}
	if (water == 0 || water > PF_MAX_BRIDGE_WATER || IsWaterTile(probe)) return true;
	if (!TileLevelable(probe) || PFReservedPlatform(probe) || PFInAncestry(node, probe)) return true;
	if (PFStationCornersTouch(probe)) return true;

	uint32 base = ((uint32)TRANSPORT_RAIL << 15) | (0u << 8);
	for (uint id = 0; id < MAX_BRIDGES; id++) {
		CommandCost r = DoCommand(probe, cur.tile, base | id, DC_NONE, CMD_BUILD_BRIDGE);
		if (r.Failed()) continue;
		int h = PFPostStationTileHeight(probe);
		uint32 g = cur.g + 20 + (uint32)(water + 1) * 12;
		return PFAddNode(probe, d, (byte)h, PF_TERRAIN_BRIDGE_HEAD,
				0, g, node, PF_ACTION_BRIDGE, 0, (byte)id);
	}
	return true;
}

static bool PFEmit(RailStep *out, int *n, int max_out, byte kind,
		TileIndex tile, TileIndex other, int value)
{
	if (*n >= max_out) return false;
	out[*n].kind = kind;
	out[*n].tile = tile;
	out[*n].other = other;
	out[*n].value = value;
	(*n)++;
	return true;
}

static bool PFEmitPreparation(const PFNode &n, RailStep *out, int *count, int max_out)
{
	if (n.terrain != PF_TERRAIN_LEVELLED) return true;
	return PFEmit(out, count, max_out, RAILSTEP_LEVEL, n.tile, n.tile, n.height);
}

static TileIndex PFLevelEnd(TileIndex tile)
{
	return TileXY((int)TileX(tile) + 1, (int)TileY(tile) + 1);
}

static bool PFLevelPostcondition(TileIndex tile, int height)
{
	return (int)TileHeight(tile) == height &&
			GetTileSlope(tile, NULL) == SLOPE_FLAT;
}

/* Preflight every command which can be meaningfully tested against the raw
 * map.  TRACK after LEVEL deliberately uses the model + slope tables: DC_NONE
 * would still see the old slope, which is the bug this planner is replacing. */
static bool PFPreflight(const RailStep *plan, int n)
{
	uint32 bridge_base = ((uint32)TRANSPORT_RAIL << 15) | (0u << 8);
	for (int i = 0; i < n; i++) {
		const RailStep &s = plan[i];
		CommandCost r;
		if (s.kind == RAILSTEP_LEVEL) {
			int delta = s.value - PFPostStationTileHeight(s.tile);
			uint32 p2 = (uint32)(uint8)(int8)delta;
			/* CmdLevelLand addresses north-corner coordinates.  A flat route
			 * tile therefore needs its full 2x2 corner rectangle, exactly as
			 * the road planner's ROADSTEP_LEVEL already does. */
			r = DoCommand(PFLevelEnd(s.tile), s.tile, p2,
					DC_NONE, CMD_LEVEL_LAND);
		} else if (s.kind == RAILSTEP_TRACK) {
			bool follows_level = i > 0 && plan[i - 1].kind == RAILSTEP_LEVEL && plan[i - 1].tile == s.tile;
			/* Raw DC_NONE cannot test a slope which exists only after station
			 * levelling.  Search already checked the exact overlaid slope table;
			 * TileLevelable checked the still-clear/tree tile type. */
			if (follows_level || PFStationCornersTouch(s.tile)) continue;
			r = DoCommand(s.tile, 0, (uint32)s.value, DC_NONE, CMD_BUILD_SINGLE_RAIL);
		} else if (s.kind == RAILSTEP_BRIDGE) {
			r = DoCommand(s.other, s.tile, bridge_base | (uint32)s.value, DC_NONE, CMD_BUILD_BRIDGE);
		} else {
			OLn("rail preflight bad kind ", (uint32)i);
			return false;
		}
		/* An already-flat level operation is satisfied, not a terrain failure. */
		if (r.Failed() && !(s.kind == RAILSTEP_LEVEL &&
				OldAICommandAlreadySatisfied(CMD_LEVEL_LAND,
						r.GetErrorMessage()) &&
				PFLevelPostcondition(s.tile, s.value))) {
			OLn("rail preflight step ", (uint32)i);
			OLn("rail preflight err ", (uint32)r.GetErrorMessage());
			return false;
		}
	}
	return true;
}

/*
 * pDir/aDir mean "from the station through its exit and outward".  Thus the
 * synthetic incoming direction at pStart is pDir, while the real approach to
 * aStart must be reverse(aDir).
 */
bool PlanRailRoute(TileIndex pStart, DiagDirection pDir, int pH,
		TileIndex aStart, DiagDirection aDir, int aH,
		RailStep *out, int *nout, int maxOut)
{
	if (nout == NULL) return false;
	*nout = 0;
	if (out == NULL || maxOut <= 0 || pStart == aStart) return false;
	if ((uint)pDir >= 4 || (uint)aDir >= 4 || pH < 0 || pH > 15 || aH < 0 || aH > 15) return false;
	if (!TileLevelable(pStart) || !TileLevelable(aStart)) return false;

	_pf_p_start = pStart; _pf_a_start = aStart;
	_pf_p_dir = (byte)pDir; _pf_a_dir = (byte)aDir; _pf_a_height = aH;
	if (!PFPrepareStationOverlay(pStart, (byte)pDir, pH, aStart, (byte)aDir, aH)) {
		OL("rail station overlay conflict");
		return false;
	}
	memset(_pf_hash, 0, sizeof(_pf_hash));
	_pf_node_count = 0;
	_pf_heap_count = 0;

	/* The first tile must be flat, at station height, and actually carry rail.
	 * Preserve it only if it is already exactly that; otherwise model LEVEL. */
	byte start_terrain = (PFPostStationTileHeight(pStart) == pH && PFPostStationSlope(pStart) == SLOPE_FLAT)
		? PF_TERRAIN_NATURAL : PF_TERRAIN_LEVELLED;
	uint32 start_g = PFTerrainCost(pStart, start_terrain, pH);
	if (!PFAddNode(pStart, (byte)pDir, (byte)pH, start_terrain,
			0, start_g, -1, PF_ACTION_NONE, 0, 0)) return false;

	int goal = -1;
	while (_pf_heap_count != 0) {
		int ni = PFHeapPop();
		if (ni < 0 || !PFIsCurrentBest(ni)) continue;
		PFNode &cur = _pf_nodes[ni];

		/* Last tile: flat at aH; approach direction plus the synthetic edge
		 * into the station must form the station-axis straight. */
		if (cur.tile == aStart && cur.in_dir == PFReverseDir((byte)aDir) &&
				cur.height == (byte)aH && cur.terrain != PF_TERRAIN_BRIDGE_HEAD) {
			bool flat = cur.terrain == PF_TERRAIN_LEVELLED || PFPostStationSlope(cur.tile) == SLOPE_FLAT;
			byte final_track = TrackForEdges((DiagDirection)PFReverseDir(cur.in_dir),
					(DiagDirection)PFReverseDir((byte)aDir));
			if (flat && !PFNodeNeedGrade(cur) && final_track != 0xFF && PFTrackLegal(cur.tile, cur.terrain, final_track, NULL)) {
				goal = ni;
				break;
			}
		}

		for (byte d = 0; d < 4; d++) {
			/* The first tile and a far bridge head must continue straight. */
			if (cur.parent < 0 && d != (byte)pDir) continue;
			if (cur.terrain == PF_TERRAIN_BRIDGE_HEAD && d != cur.in_dir) continue;
			TileIndex next;
			if (!PFNeighbour(cur.tile, d, &next)) continue;

			if (IsWaterTile(next)) {
				if (cur.terrain != PF_TERRAIN_BRIDGE_HEAD && !PFTryBridge(ni, d)) goto exhausted;
				continue;
			}
			if (!TileLevelable(next)) continue;

			if (cur.terrain == PF_TERRAIN_BRIDGE_HEAD) {
				if (!PFAddLandCandidates(ni, next, d, cur.height, false, 10,
						PF_ACTION_LEAVE_BRIDGE, 0)) goto exhausted;
				continue;
			}

			byte track = TrackForEdges((DiagDirection)PFReverseDir(cur.in_dir), (DiagDirection)d);
			if (track == 0xFF) continue; /* U-turn / same edge */
			bool foundation = false;
			if (!PFTrackLegal(cur.tile, cur.terrain, track, &foundation)) continue;
			int slope = (cur.terrain == PF_TERRAIN_NATURAL) ? PFPostStationSlope(cur.tile) : 0;
			bool carries_grade = cur.terrain == PF_TERRAIN_NATURAL && slope > 0 && slope < 15 &&
					track <= 1 && !foundation;
			if (PFNodeNeedGrade(cur) && !carries_grade) continue;
			uint32 edge_cost = 10;
			if (d != cur.in_dir) edge_cost += 7;       /* curve */
			if (foundation) edge_cost += 5;
			if (!PFAddLandCandidates(ni, next, d, cur.height, carries_grade, edge_cost,
					PF_ACTION_TRACK, track)) goto exhausted;
		}
	}

exhausted:
	if (goal < 0) {
		OLn("rail A* nodes, no route ", (uint32)_pf_node_count);
		return false;
	}

	int nr = 0;
	for (int n = goal; _pf_nodes[n].parent >= 0; n = _pf_nodes[n].parent) {
		if (nr >= PF_REVERSE_CAPACITY) {
			OL("rail route reconstruction too long");
			return false;
		}
		_pf_reverse[nr++] = n;
	}
	int count = 0;
	for (int ri = nr - 1; ri >= 0; ri--) {
		PFNode &child = _pf_nodes[_pf_reverse[ri]];
		PFNode &parent = _pf_nodes[child.parent];
		byte action = PFNodeAction(child);
		if (action == PF_ACTION_TRACK) {
			if (!PFEmitPreparation(parent, out, &count, maxOut)) return false;
			if (!PFEmit(out, &count, maxOut, RAILSTEP_TRACK, parent.tile, parent.tile, child.track)) return false;
		} else if (action == PF_ACTION_BRIDGE) {
			if (!PFEmit(out, &count, maxOut, RAILSTEP_BRIDGE,
					parent.tile, child.tile, child.bridge_id)) return false;
		} else if (action != PF_ACTION_LEAVE_BRIDGE) {
			return false;
		}
	}

	PFNode &last = _pf_nodes[goal];
	byte last_track = TrackForEdges((DiagDirection)PFReverseDir(last.in_dir),
			(DiagDirection)PFReverseDir((byte)aDir));
	if (!PFEmitPreparation(last, out, &count, maxOut)) return false;
	if (!PFEmit(out, &count, maxOut, RAILSTEP_TRACK, last.tile, last.tile, last_track)) return false;

	if (!PFPreflight(out, count)) return false;
	*nout = count;
	OLn("rail A* nodes used ", (uint32)_pf_node_count);
	OLn("rail plan steps ", (uint32)count);
	return true;
}

/* Execute and remove plans one synchronized command at a time.  The loops still
 * run to completion in one call in single-player because CcOldAI is immediate;
 * networking returns WAIT at the first queued command and resumes at the saved
 * cursor after its synchronized execution frame. */
OldAIWorkResult RemoveRailPlan(OldAICompany *a, const RailStep *plan, int n)
{
	if (plan == NULL || n < 0) return OAI_WORK_FAILED;
	if (a->cleanup_cursor < 0) a->cleanup_cursor = n - 1;

	while (a->cleanup_cursor >= 0) {
		int i = a->cleanup_cursor;
		const RailStep &s = plan[i];
		if (a->pending_op == OAOP_RAIL_PLAN_REMOVE) {
			OldAIWorkResult wr = OldAICommand(a, OAOP_RAIL_PLAN_REMOVE,
					a->pending_tile, a->pending_p1, a->pending_p2, a->pending_cmd);
			if (wr != OAI_WORK_DONE) return wr;
			a->cleanup_cursor--;
			continue;
		}
		TileIndex tile;
		uint32 p1, p2, cmd;
		if (s.kind == RAILSTEP_LEVEL) {
			a->cleanup_cursor--;
			continue;
		}
		if (s.kind == RAILSTEP_TRACK) {
			if (!IsTileType(s.tile, MP_RAILWAY)) {
				a->cleanup_cursor--;
				continue;
			}
			tile = s.tile; p1 = 0; p2 = (uint32)s.value; cmd = CMD_REMOVE_SINGLE_RAIL;
		} else if (s.kind == RAILSTEP_BRIDGE) {
			if (!IsTileType(s.other, MP_TUNNELBRIDGE)) {
				a->cleanup_cursor--;
				continue;
			}
			tile = s.other; p1 = 0; p2 = 0; cmd = CMD_LANDSCAPE_CLEAR;
		} else {
			OLn("rail rollback bad kind ", (uint32)i);
			return OAI_WORK_FAILED;
		}

		CommandCost test = DoCommand(tile, p1, p2, DC_NONE, cmd);
		if (test.Failed()) {
			OLn("rail rollback step ", (uint32)i);
			OLn("rail rollback test err ", (uint32)test.GetErrorMessage());
			return OAI_WORK_FAILED;
		}
		StringID error;
		OldAIWorkResult wr = OldAICommand(a, OAOP_RAIL_PLAN_REMOVE,
				tile, p1, p2, cmd, NULL, &error);
		if (wr == OAI_WORK_WAIT) return wr;
		if (wr == OAI_WORK_FAILED) {
			OLn("rail rollback real step ", (uint32)i);
			return wr;
		}
		a->cleanup_cursor--;
	}
	a->cleanup_cursor = -1;
	return OAI_WORK_DONE;
}

OldAIWorkResult ExecuteRailPlan(OldAICompany *a, const RailStep *plan, int n)
{
	if (plan == NULL || n < 0) return OAI_WORK_FAILED;
	uint32 bridge_base = ((uint32)TRANSPORT_RAIL << 15) | (0u << 8);
	while (a->plan_cursor < n) {
		int i = a->plan_cursor;
		const RailStep &s = plan[i];
		if (a->pending_op == OAOP_RAIL_PLAN_BUILD) {
			StringID error;
			OldAIWorkResult wr = OldAICommand(a, OAOP_RAIL_PLAN_BUILD,
					a->pending_tile, a->pending_p1, a->pending_p2,
					a->pending_cmd, NULL, &error);
			if (wr == OAI_WORK_WAIT) return wr;
			if (wr == OAI_WORK_FAILED) {
				OLn("rail execute real step ", (uint32)i);
				OLn("rail execute real err ", (uint32)error);
				return wr;
			}
			if (s.kind == RAILSTEP_LEVEL &&
					!PFLevelPostcondition(s.tile, s.value)) {
				OLn("rail execute incomplete level step ", (uint32)i);
				return OAI_WORK_FAILED;
			}
			a->plan_cursor++;
			continue;
		}
		CommandCost test;
		uint32 p1 = 0, p2 = 0, cmd = 0;
		TileIndex command_tile = s.tile;
		if (s.kind == RAILSTEP_LEVEL) {
			int delta = s.value - (int)TileHeight(s.tile);
			p1 = s.tile;
			p2 = (uint32)(uint8)(int8)delta;
			cmd = CMD_LEVEL_LAND;
			command_tile = PFLevelEnd(s.tile);
		} else if (s.kind == RAILSTEP_TRACK) {
			p1 = 0; p2 = (uint32)s.value; cmd = CMD_BUILD_SINGLE_RAIL;
		} else if (s.kind == RAILSTEP_BRIDGE) {
			command_tile = s.other;
			p1 = s.tile;
			p2 = bridge_base | (uint32)s.value;
			cmd = CMD_BUILD_BRIDGE;
		} else {
			OLn("rail execute bad kind ", (uint32)i);
			return OAI_WORK_FAILED;
		}

		test = DoCommand(command_tile, p1, p2, DC_NONE, cmd);
		if (test.Failed()) {
			if (s.kind == RAILSTEP_LEVEL &&
					OldAICommandAlreadySatisfied(CMD_LEVEL_LAND,
							test.GetErrorMessage()) &&
					PFLevelPostcondition(s.tile, s.value)) {
				a->plan_cursor++;
				continue; /* already level */
			}
			OLn("rail execute test step ", (uint32)i);
			OLn("rail execute test kind ", (uint32)s.kind);
			OLn("rail execute test err ", (uint32)test.GetErrorMessage());
			return OAI_WORK_FAILED;
		}
		if (s.kind == RAILSTEP_LEVEL) {
			const Company *co = Company::GetIfValid(_current_company);
			if (co != NULL && test.GetCost() > co->money) {
				OLn("rail level exceeds money at step ", (uint32)i);
				return OAI_WORK_FAILED;
			}
		}
		StringID error;
		OldAIWorkResult wr = OldAICommand(a, OAOP_RAIL_PLAN_BUILD,
				command_tile, p1, p2, cmd, NULL, &error);
		if (wr == OAI_WORK_WAIT) return wr;
		if (wr == OAI_WORK_FAILED) {
			OLn("rail execute real step ", (uint32)i);
			OLn("rail execute real err ", (uint32)error);
			return wr;
		}
		if (s.kind == RAILSTEP_LEVEL &&
				!PFLevelPostcondition(s.tile, s.value)) {
			OLn("rail execute incomplete level step ", (uint32)i);
			return OAI_WORK_FAILED;
		}
		a->plan_cursor++;
	}
	a->plan_cursor = 0;
	return OAI_WORK_DONE;
}

/* ------------------------------------------------------------------------- *
 *  Road trial.  This is deliberately conservative around terraforming: every *
 *  levelled route tile uses one common height, the resulting shared-corner    *
 *  overlay is verified before execution, and ALL LEVEL steps precede roads.  *
 *  That is the property the old build-as-you-go L road did not have.          *
 * ------------------------------------------------------------------------- */

enum RPTerrainMode {
	RP_TERRAIN_NATURAL = 0,
	RP_TERRAIN_LEVELLED = 1,
	RP_TERRAIN_BRIDGE_HEAD = 2
};

enum RPAction {
	RP_ACTION_NONE = 0,
	RP_ACTION_LAND = 1,
	RP_ACTION_BRIDGE = 2,
	RP_ACTION_LEAVE_BRIDGE = 3
};

enum {
	RP_STEP_BUILT = 0x8000,
	RP_STEP_VALUE_MASK = 0x00FF,
	RP_STEP_ORIGINAL_SHIFT = 8
};

static TileIndex _rp_start, _rp_goal;
static TileIndex _rp_stop_a, _rp_stop_b, _rp_depot;
static byte _rp_goal_out, _rp_level_height;

static bool RPReserved(TileIndex tile)
{
	return tile == _rp_stop_a || tile == _rp_stop_b || tile == _rp_depot;
}

static bool RPNearReserved(TileIndex tile)
{
	int x = (int)TileX(tile), y = (int)TileY(tile);
	TileIndex reserved[3] = { _rp_stop_a, _rp_stop_b, _rp_depot };
	for (int i = 0; i < 3; i++) {
		int dx = x - (int)TileX(reserved[i]);
		int dy = y - (int)TileY(reserved[i]);
		if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1) return true;
	}
	return false;
}

static bool RPLandTile(TileIndex tile)
{
	return TileLevelable(tile) || IsNormalRoadTile(tile);
}

static uint32 RPHashValue(TileIndex tile, byte in_dir, byte terrain)
{
	uint32 h = (uint32)tile * 2654435761U;
	h ^= (uint32)in_dir * 0x9E3779B9U;
	h ^= (uint32)terrain * 0x85EBCA6BU;
	return h & (RP_HASH_SIZE - 1);
}

static bool RPSameState(const PFNode &n, TileIndex tile, byte in_dir, byte terrain)
{
	return n.tile == tile && n.in_dir == in_dir && n.terrain == terrain;
}

static int RPFindSlot(TileIndex tile, byte in_dir, byte terrain, bool *found)
{
	uint32 slot = RPHashValue(tile, in_dir, terrain);
	for (int probe = 0; probe < RP_HASH_SIZE; probe++) {
		int v = _pf_hash[slot];
		if (v == 0) {
			*found = false;
			return (int)slot;
		}
		if (RPSameState(_pf_nodes[v - 1], tile, in_dir, terrain)) {
			*found = true;
			return (int)slot;
		}
		slot = (slot + 1) & (RP_HASH_SIZE - 1);
	}
	*found = false;
	return -1;
}

static bool RPAddNode(TileIndex tile, byte in_dir, byte terrain, uint32 g,
		int parent, byte action, byte bridge_id)
{
	bool found;
	int slot = RPFindSlot(tile, in_dir, terrain, &found);
	if (slot < 0) return false;
	if (found && _pf_nodes[_pf_hash[slot] - 1].g <= g) return true;
	if (_pf_node_count >= RP_NODE_BUDGET) return false;

	int index = _pf_node_count++;
	PFNode &n = _pf_nodes[index];
	n.tile = tile;
	n.g = g;
	n.parent = (int16)parent;
	n.in_dir = in_dir;
	n.height = _rp_level_height; /* lets the shared PF heap use its heuristic */
	n.terrain = terrain;
	n.action_grade = action;
	n.track = 0;
	n.bridge_id = bridge_id;
	_pf_hash[slot] = index + 1;
	return PFHeapPush(index);
}

static bool RPIsCurrentBest(int node)
{
	PFNode &n = _pf_nodes[node];
	bool found;
	int slot = RPFindSlot(n.tile, n.in_dir, n.terrain, &found);
	return found && slot >= 0 && _pf_hash[slot] == node + 1;
}

static RoadBits RPPresentRoadBits(TileIndex tile)
{
	return IsNormalRoadTile(tile) ? GetRoadBits(tile, ROADTYPE_ROAD) : ROAD_NONE;
}

/* Exact raw-map command test for an unmodified tile.  Existing pieces and
 * junctions are accepted; only the missing half-road bits are submitted. */
static bool RPRoadLegalRaw(TileIndex tile, RoadBits required)
{
	RoadBits present = RPPresentRoadBits(tile);
	RoadBits missing = (RoadBits)(required & ~present);
	if (missing == ROAD_NONE) return true;
	if (!RPLandTile(tile)) return false;
	CommandCost r = DoCommand(tile, missing | (ROADTYPE_ROAD << 4), 0, DC_NONE, CMD_BUILD_ROAD);
	return r.Succeeded() || OldAICommandAlreadySatisfied(
			CMD_BUILD_ROAD, r.GetErrorMessage());
}

static bool RPLevelAllowed(TileIndex tile)
{
	if (!TileLevelable(tile) || RPNearReserved(tile)) return false;
	int x = (int)TileX(tile), y = (int)TileY(tile);
	if (x + 1 >= (int)MapSizeX() || y + 1 >= (int)MapSizeY()) return false;
	TileIndex end = TileXY(x + 1, y + 1);
	int delta = (int)_rp_level_height - (int)TileHeight(tile);
	uint32 p2 = (uint32)(uint8)(int8)delta;
	/* CmdLevelLand operates on TileHeight corner coordinates.  A 2x2 corner
	 * rectangle is therefore the exact four-corner footprint of one road tile. */
	CommandCost r = DoCommand(end, tile, p2, DC_NONE, CMD_LEVEL_LAND);
	return r.Succeeded() || OldAICommandAlreadySatisfied(
			CMD_LEVEL_LAND, r.GetErrorMessage());
}

static bool RPAddLandCandidates(int parent, TileIndex tile, byte move_dir,
		uint32 edge_cost, byte action)
{
	if (RPReserved(tile) || PFInAncestry(parent, tile) || !RPLandTile(tile)) return true;

	/* Keep the exact natural terrain candidate.  Its road-bit command is tested
	 * when the outgoing direction makes the complete shape known. */
	if (!RPAddNode(tile, move_dir, RP_TERRAIN_NATURAL,
			_pf_nodes[parent].g + edge_cost, parent, action, 0)) return false;

	/* A flat candidate is useful for steep/awkward clear land.  All such tiles
	 * share one height so overlapping corner areas can never disagree. */
	if (TileLevelable(tile) && GetTileSlope(tile, NULL) != SLOPE_FLAT && RPLevelAllowed(tile)) {
		int dh = (int)TileHeight(tile) - (int)_rp_level_height;
		if (dh < 0) dh = -dh;
		if (!RPAddNode(tile, move_dir, RP_TERRAIN_LEVELLED,
				_pf_nodes[parent].g + edge_cost + 28 + (uint32)dh * 8,
				parent, action, 0)) return false;
	}
	return true;
}

static bool RPTryBridge(int node, byte d)
{
	PFNode &cur = _pf_nodes[node];
	/* Bridge heads and their slopes are owned by CMD_BUILD_BRIDGE.  Test that
	 * exact 1.0.5 command on untouched terrain and store the accepted bridge id. */
	if (cur.terrain != RP_TERRAIN_NATURAL || cur.parent < 0 || cur.in_dir != d ||
			!TileLevelable(cur.tile)) return true;
	TileIndex probe;
	if (!PFNeighbour(cur.tile, d, &probe) || !IsWaterTile(probe)) return true;

	int water = 0;
	while (water < RP_MAX_BRIDGE_WATER && IsValidTile(probe) && IsWaterTile(probe)) {
		water++;
		if (!PFNeighbour(probe, d, &probe)) return true;
	}
	if (water == 0 || water > RP_MAX_BRIDGE_WATER || IsWaterTile(probe)) return true;
	if (RPReserved(probe) || !TileLevelable(probe) || PFInAncestry(node, probe)) return true;

	uint32 base = ((uint32)TRANSPORT_ROAD << 15) |
			(RoadTypeToRoadTypes(ROADTYPE_ROAD) << 8);
	for (uint id = 0; id < MAX_BRIDGES; id++) {
		CommandCost r = DoCommand(probe, cur.tile, base | id, DC_NONE, CMD_BUILD_BRIDGE);
		if (r.Failed()) continue;
		uint32 g = cur.g + 18 + (uint32)(water + 1) * 8;
		return RPAddNode(probe, d, RP_TERRAIN_BRIDGE_HEAD, g,
				node, RP_ACTION_BRIDGE, (byte)id);
	}
	return true;
}

static bool RPBuildPath(int goal, int *count)
{
	int n = 0;
	for (int p = goal; p >= 0; p = _pf_nodes[p].parent) {
		if (n >= RP_REVERSE_CAPACITY) return false;
		_pf_reverse[n++] = p;
	}
	for (int i = 0; i < n / 2; i++) {
		int t = _pf_reverse[i];
		_pf_reverse[i] = _pf_reverse[n - 1 - i];
		_pf_reverse[n - 1 - i] = t;
	}
	*count = n;
	return true;
}

static bool RPLevelOwnsCorner(int cx, int cy, int path_count)
{
	for (int i = 0; i < path_count; i++) {
		const PFNode &n = _pf_nodes[_pf_reverse[i]];
		if (n.terrain != RP_TERRAIN_LEVELLED) continue;
		int x = (int)TileX(n.tile), y = (int)TileY(n.tile);
		if ((cx == x || cx == x + 1) && (cy == y || cy == y + 1)) return true;
	}
	return false;
}

static bool RPTileTouchedByLevel(TileIndex tile, int path_count)
{
	int x = (int)TileX(tile), y = (int)TileY(tile);
	return RPLevelOwnsCorner(x, y, path_count) ||
			RPLevelOwnsCorner(x + 1, y, path_count) ||
			RPLevelOwnsCorner(x, y + 1, path_count) ||
			RPLevelOwnsCorner(x + 1, y + 1, path_count);
}

static int RPVirtualCornerHeight(int x, int y, int path_count)
{
	if (RPLevelOwnsCorner(x, y, path_count)) return (int)_rp_level_height;
	return (int)TileHeight(TileXY(x, y));
}

/* We use the game's exact DoCommand test for untouched natural slopes.  For a
 * natural tile whose corners a future LEVEL changes, accept only an exactly
 * flat virtual result.  This is conservative, but cannot guess wrong about
 * CheckRoadSlope or foundations. */
static bool RPVirtualFlat(TileIndex tile, int path_count)
{
	int x = (int)TileX(tile), y = (int)TileY(tile);
	int h = RPVirtualCornerHeight(x, y, path_count);
	return RPVirtualCornerHeight(x + 1, y, path_count) == h &&
			RPVirtualCornerHeight(x, y + 1, path_count) == h &&
			RPVirtualCornerHeight(x + 1, y + 1, path_count) == h;
}

/* If the final virtual corner field contains a >1 step at the edge of the
 * levelled union, CmdTerraformLand would recursively alter an unmodelled
 * outside corner. Reject that trial instead of pretending the 2x2 LEVEL is
 * local. */
static bool RPLevelBoundarySafe(int path_count)
{
	for (int i = 0; i < path_count; i++) {
		const PFNode &n = _pf_nodes[_pf_reverse[i]];
		if (n.terrain != RP_TERRAIN_LEVELLED) continue;
		int x = (int)TileX(n.tile), y = (int)TileY(n.tile);
		for (int cy = y; cy <= y + 1; cy++) {
			for (int cx = x; cx <= x + 1; cx++) {
				int h = RPVirtualCornerHeight(cx, cy, path_count);
				for (int d = 0; d < 4; d++) {
					int nx = cx + (d == 0 ? -1 : (d == 1 ? 1 : 0));
					int ny = cy + (d == 2 ? -1 : (d == 3 ? 1 : 0));
					if (nx < 0 || ny < 0 || nx >= (int)MapSizeX() || ny >= (int)MapSizeY()) return false;
					int nh = RPVirtualCornerHeight(nx, ny, path_count);
					if (h - nh > 1 || nh - h > 1) return false;
				}
			}
		}
	}
	return true;
}

static RoadBits RPEdgeBits(byte incoming_move, byte outgoing)
{
	return (RoadBits)(DiagDirToRoadBits((DiagDirection)PFReverseDir(incoming_move)) |
			DiagDirToRoadBits((DiagDirection)outgoing));
}

static bool RPValidateModelledPath(int path_count)
{
	for (int i = 0; i < path_count; i++) {
		PFNode &cur = _pf_nodes[_pf_reverse[i]];
		if (cur.terrain == RP_TERRAIN_BRIDGE_HEAD) {
			if (RPTileTouchedByLevel(cur.tile, path_count)) return false;
			continue;
		}

		if (i + 1 < path_count &&
				_pf_nodes[_pf_reverse[i + 1]].action_grade == RP_ACTION_BRIDGE) {
			/* CMD_BUILD_BRIDGE owns this near head. */
			if (RPTileTouchedByLevel(cur.tile, path_count)) return false;
			continue;
		}

		byte out_dir = (i + 1 < path_count)
				? _pf_nodes[_pf_reverse[i + 1]].in_dir : _rp_goal_out;
		RoadBits bits = RPEdgeBits(cur.in_dir, out_dir);
		if (cur.terrain == RP_TERRAIN_NATURAL) {
			if (RPTileTouchedByLevel(cur.tile, path_count)) {
				if (!RPVirtualFlat(cur.tile, path_count)) return false;
			} else if (!RPRoadLegalRaw(cur.tile, bits)) {
				return false;
			}
		}
	}
	return true;
}

static bool RPEmit(RoadStep *out, int *count, int max_out, byte kind,
		TileIndex tile, TileIndex other, uint16 data)
{
	if (*count >= max_out) return false;
	RoadStep &s = out[*count];
	s.tile = tile;
	s.other = other;
	s.data = data;
	s.kind = kind;
	s.unused = 0;
	(*count)++;
	return true;
}

static bool RPEmitRoadTile(RoadStep *out, int *count, int max_out,
		TileIndex tile, RoadBits required)
{
	RoadBits original = RPPresentRoadBits(tile);
	RoadBits missing = (RoadBits)(required & ~original);
	if (missing == ROAD_NONE) return true;
	uint16 data = (uint16)missing | ((uint16)original << RP_STEP_ORIGINAL_SHIFT);
	return RPEmit(out, count, max_out, ROADSTEP_ROAD, tile, tile, data);
}

static bool RPPreflight(const RoadStep *plan, int n, int path_count)
{
	uint32 bridge_base = ((uint32)TRANSPORT_ROAD << 15) |
			(RoadTypeToRoadTypes(ROADTYPE_ROAD) << 8);
	for (int i = 0; i < n; i++) {
		const RoadStep &s = plan[i];
		CommandCost r;
		if (s.kind == ROADSTEP_LEVEL) {
			int delta = (int)(s.data & RP_STEP_VALUE_MASK) - (int)TileHeight(s.tile);
			r = DoCommand(s.other, s.tile, (uint32)(uint8)(int8)delta, DC_NONE, CMD_LEVEL_LAND);
		} else if (s.kind == ROADSTEP_ROAD) {
			/* Raw terrain is deliberately not used to judge a road which follows
			 * virtual earthworks; RPValidateModelledPath proved it flat. */
			if (RPTileTouchedByLevel(s.tile, path_count)) continue;
			RoadBits bits = (RoadBits)(s.data & 0x0F);
			r = DoCommand(s.tile, bits | (ROADTYPE_ROAD << 4), 0, DC_NONE, CMD_BUILD_ROAD);
		} else if (s.kind == ROADSTEP_BRIDGE) {
			r = DoCommand(s.other, s.tile,
					bridge_base | (uint32)(s.data & RP_STEP_VALUE_MASK),
					DC_NONE, CMD_BUILD_BRIDGE);
		} else {
			OLn("road preflight bad kind ", (uint32)i);
			return false;
		}
		uint32 cmd = s.kind == ROADSTEP_LEVEL ? CMD_LEVEL_LAND :
				(s.kind == ROADSTEP_ROAD ? CMD_BUILD_ROAD : CMD_BUILD_BRIDGE);
		if (r.Failed() && !OldAICommandAlreadySatisfied(
				cmd, r.GetErrorMessage())) {
			OLn("road preflight step ", (uint32)i);
			OLn("road preflight err ", (uint32)r.GetErrorMessage());
			return false;
		}
	}
	return true;
}

/*
 * stopA-frontA and frontB-stopB are existing straight town-road edges.  The
 * planned line runs frontA..frontB, with synthetic endpoint directions forcing
 * it to connect to both future drive-through stops without modifying their
 * pure-straight road tiles.  depot is reserved from both road and earthworks.
 */
bool PlanRoadRoute(TileIndex stopA, TileIndex frontA,
		TileIndex stopB, TileIndex frontB, TileIndex depot,
		RoadStep *out, int *nout, int max_out)
{
	if (nout == NULL) return false;
	*nout = 0;
	if (out == NULL || max_out <= 0 || frontA == frontB) return false;
	byte start_in = DiagDirBetween(stopA, frontA);
	byte goal_out = DiagDirBetween(frontB, stopB);
	if (start_in == 0xFF || goal_out == 0xFF) return false;
	if (!IsNormalRoadTile(stopA) || !IsNormalRoadTile(frontA) ||
			!IsNormalRoadTile(stopB) || !IsNormalRoadTile(frontB)) return false;

	_rp_start = frontA;
	_rp_goal = frontB;
	_rp_stop_a = stopA;
	_rp_stop_b = stopB;
	_rp_depot = depot;
	_rp_goal_out = goal_out;
	_rp_level_height = (byte)TileHeight(frontA);

	/* Reuse the rail A* buffers.  PFHeapLess reads _pf_a_start/_pf_a_height. */
	_pf_a_start = _rp_goal;
	_pf_a_height = _rp_level_height;
	memset(_pf_hash, 0, RP_HASH_SIZE * sizeof(_pf_hash[0]));
	_pf_node_count = 0;
	_pf_heap_count = 0;

	if (!RPAddNode(_rp_start, start_in, RP_TERRAIN_NATURAL,
			0, -1, RP_ACTION_NONE, 0)) return false;

	int goal = -1;
	while (_pf_heap_count != 0) {
		int ni = PFHeapPop();
		if (ni < 0 || !RPIsCurrentBest(ni)) continue;
		PFNode &cur = _pf_nodes[ni];

		if (cur.tile == _rp_goal && cur.terrain != RP_TERRAIN_BRIDGE_HEAD) {
			RoadBits last_bits = RPEdgeBits(cur.in_dir, _rp_goal_out);
			if (cur.terrain == RP_TERRAIN_LEVELLED || RPRoadLegalRaw(cur.tile, last_bits)) {
				goal = ni;
				break;
			}
		}

		for (byte d = 0; d < 4; d++) {
			if (d == PFReverseDir(cur.in_dir)) continue; /* U-turn */
			if (cur.terrain == RP_TERRAIN_BRIDGE_HEAD && d != cur.in_dir) continue;
			TileIndex next;
			if (!PFNeighbour(cur.tile, d, &next)) continue;

			if (IsWaterTile(next)) {
				if (cur.terrain != RP_TERRAIN_BRIDGE_HEAD && !RPTryBridge(ni, d)) goto road_exhausted;
				continue;
			}
			if (!RPLandTile(next) || RPReserved(next)) continue;

			uint32 edge_cost = 10 + (d != cur.in_dir ? 3 : 0);
			if (cur.terrain == RP_TERRAIN_BRIDGE_HEAD) {
				if (!RPAddLandCandidates(ni, next, d, edge_cost,
						RP_ACTION_LEAVE_BRIDGE)) goto road_exhausted;
				continue;
			}

			RoadBits current_bits = RPEdgeBits(cur.in_dir, d);
			if (cur.terrain == RP_TERRAIN_NATURAL &&
					!RPRoadLegalRaw(cur.tile, current_bits)) continue;
			if (!RPAddLandCandidates(ni, next, d, edge_cost,
					RP_ACTION_LAND)) goto road_exhausted;
		}
	}

road_exhausted:
	if (goal < 0) {
		OLn("road A* nodes, no route ", (uint32)_pf_node_count);
		return false;
	}

	int path_count = 0;
	if (!RPBuildPath(goal, &path_count)) {
		OL("road route reconstruction too long");
		return false;
	}
	if (!RPValidateModelledPath(path_count)) {
		OL("road model rejected shared-corner earthworks");
		return false;
	}
	if (!RPLevelBoundarySafe(path_count)) {
		OL("road model rejected cascading earthworks");
		return false;
	}

	int count = 0;
	/* All earthworks first.  They have one target height and their complete
	 * shared-corner result was checked above before a road object can exist. */
	for (int i = 0; i < path_count; i++) {
		PFNode &n = _pf_nodes[_pf_reverse[i]];
		if (n.terrain != RP_TERRAIN_LEVELLED) continue;
		TileIndex end = TileXY((int)TileX(n.tile) + 1, (int)TileY(n.tile) + 1);
		if (!RPEmit(out, &count, max_out, ROADSTEP_LEVEL,
				n.tile, end, (uint16)_rp_level_height)) return false;
	}

	for (int i = 0; i + 1 < path_count; i++) {
		PFNode &cur = _pf_nodes[_pf_reverse[i]];
		PFNode &child = _pf_nodes[_pf_reverse[i + 1]];
		if (child.action_grade == RP_ACTION_BRIDGE) {
			if (!RPEmit(out, &count, max_out, ROADSTEP_BRIDGE,
					cur.tile, child.tile, (uint16)child.bridge_id)) return false;
		} else if (cur.terrain != RP_TERRAIN_BRIDGE_HEAD) {
			RoadBits bits = RPEdgeBits(cur.in_dir, child.in_dir);
			if (!RPEmitRoadTile(out, &count, max_out, cur.tile, bits)) return false;
		}
	}
	PFNode &last = _pf_nodes[_pf_reverse[path_count - 1]];
	if (last.terrain == RP_TERRAIN_BRIDGE_HEAD) return false;
	if (!RPEmitRoadTile(out, &count, max_out, last.tile,
			RPEdgeBits(last.in_dir, _rp_goal_out))) return false;

	if (!RPPreflight(out, count, path_count)) return false;
	*nout = count;
	OLn("road A* nodes used ", (uint32)_pf_node_count);
	OLn("road plan path tiles ", (uint32)path_count);
	OLn("road plan steps ", (uint32)count);
	return true;
}

OldAIWorkResult RemoveRoadPlan(OldAICompany *a, RoadStep *plan, int n)
{
	if (plan == NULL || n < 0) return OAI_WORK_FAILED;
	if (a->cleanup_cursor < 0) {
		a->cleanup_cursor = n - 1;
		a->cleanup_phase = 0;
	}

	while (a->cleanup_cursor >= 0) {
		int i = a->cleanup_cursor;
		RoadStep &s = plan[i];
		if (a->pending_op == OAOP_ROAD_PLAN_REMOVE) {
			OldAIWorkResult wr = OldAICommand(a, OAOP_ROAD_PLAN_REMOVE,
					a->pending_tile, a->pending_p1, a->pending_p2, a->pending_cmd);
			if (wr != OAI_WORK_DONE) return wr;
			if (s.kind == ROADSTEP_BRIDGE) {
				s.data &= (uint16)~RP_STEP_BUILT;
				a->cleanup_cursor--;
				a->cleanup_phase = 0;
			} else if (a->cleanup_phase < 2) {
				a->cleanup_phase++;
			} else {
				s.data &= (uint16)~RP_STEP_BUILT;
				a->cleanup_cursor--;
				a->cleanup_phase = 0;
			}
			continue;
		}
		if ((s.data & RP_STEP_BUILT) == 0 || s.kind == ROADSTEP_LEVEL) {
			a->cleanup_cursor--;
			a->cleanup_phase = 0;
			continue;
		}

		if (s.kind == ROADSTEP_BRIDGE) {
			if (!IsTileType(s.other, MP_TUNNELBRIDGE)) {
				s.data &= (uint16)~RP_STEP_BUILT;
				a->cleanup_cursor--;
				continue;
			}
			CommandCost test = DoCommand(s.other, 0, 0, DC_NONE, CMD_LANDSCAPE_CLEAR);
			if (test.Failed()) {
				OLn("road bridge rollback err ", (uint32)test.GetErrorMessage());
				return OAI_WORK_FAILED;
			}
			OldAIWorkResult wr = OldAICommand(a, OAOP_ROAD_PLAN_REMOVE,
					s.other, 0, 0, CMD_LANDSCAPE_CLEAR);
			if (wr != OAI_WORK_DONE) return wr;
			s.data &= (uint16)~RP_STEP_BUILT;
			a->cleanup_cursor--;
			continue;
		}
		if (s.kind != ROADSTEP_ROAD) {
			OLn("road rollback bad kind ", (uint32)i);
			return OAI_WORK_FAILED;
		}

		RoadBits added = (RoadBits)(s.data & 0x0F);
		if (a->cleanup_phase < 2) {
			Axis axis = (Axis)a->cleanup_phase;
			RoadBits axis_bits = axis == AXIS_X ? (RoadBits)(ROAD_NE | ROAD_SW)
					: (RoadBits)(ROAD_SE | ROAD_NW);
			if ((added & axis_bits) != 0 &&
					(RPPresentRoadBits(s.tile) & axis_bits) != 0) {
				uint32 p2 = 1u | ((uint32)axis << 2) |
						((uint32)ROADTYPE_ROAD << 3);
				OldAIWorkResult wr = OldAICommand(a, OAOP_ROAD_PLAN_REMOVE,
						s.tile, s.tile, p2, CMD_REMOVE_LONG_ROAD);
				if (wr != OAI_WORK_DONE) return wr;
			}
			a->cleanup_phase++;
			continue;
		}

		RoadBits original = (RoadBits)((s.data >> RP_STEP_ORIGINAL_SHIFT) & 0x0F);
		RoadBits missing = (RoadBits)(original & ~RPPresentRoadBits(s.tile));
		if (a->cleanup_phase == 2 && missing != ROAD_NONE) {
			CommandCost test = DoCommand(s.tile,
					missing | (ROADTYPE_ROAD << 4), 0, DC_NONE, CMD_BUILD_ROAD);
			if (test.Failed() && !OldAICommandAlreadySatisfied(
					CMD_BUILD_ROAD, test.GetErrorMessage())) {
				OLn("road rollback restore err ", (uint32)test.GetErrorMessage());
				return OAI_WORK_FAILED;
			}
			if (test.Succeeded()) {
				OldAIWorkResult wr = OldAICommand(a, OAOP_ROAD_PLAN_REMOVE,
						s.tile, missing | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD);
				if (wr != OAI_WORK_DONE) return wr;
			}
		}
		s.data &= (uint16)~RP_STEP_BUILT;
		a->cleanup_cursor--;
		a->cleanup_phase = 0;
	}
	a->cleanup_cursor = -1;
	a->cleanup_phase = 0;
	return OAI_WORK_DONE;
}

OldAIWorkResult ExecuteRoadPlan(OldAICompany *a, RoadStep *plan, int n)
{
	if (plan == NULL || n < 0) return OAI_WORK_FAILED;
	uint32 bridge_base = ((uint32)TRANSPORT_ROAD << 15) |
			(RoadTypeToRoadTypes(ROADTYPE_ROAD) << 8);

	while (a->plan_cursor < n) {
		int i = a->plan_cursor;
		RoadStep &s = plan[i];
		if (a->pending_op == OAOP_ROAD_PLAN_BUILD) {
			StringID error;
			OldAIWorkResult wr = OldAICommand(a, OAOP_ROAD_PLAN_BUILD,
					a->pending_tile, a->pending_p1, a->pending_p2,
					a->pending_cmd, NULL, &error);
			if (wr == OAI_WORK_WAIT) return wr;
			if (wr == OAI_WORK_FAILED) {
				OLn("road execute real step ", (uint32)i);
				OLn("road execute real err ", (uint32)error);
				return wr;
			}
			if (s.kind != ROADSTEP_LEVEL) s.data |= RP_STEP_BUILT;
			a->plan_cursor++;
			continue;
		}
		TileIndex command_tile = s.tile;
		uint32 p1 = 0, p2 = 0, cmd = 0;
		if (s.kind == ROADSTEP_LEVEL) {
			int delta = (int)(s.data & RP_STEP_VALUE_MASK) - (int)TileHeight(s.tile);
			p1 = s.tile;
			p2 = (uint32)(uint8)(int8)delta;
			cmd = CMD_LEVEL_LAND;
			command_tile = s.other;
		} else if (s.kind == ROADSTEP_ROAD) {
			p1 = (s.data & 0x0F) | (ROADTYPE_ROAD << 4);
			cmd = CMD_BUILD_ROAD;
		} else if (s.kind == ROADSTEP_BRIDGE) {
			command_tile = s.other;
			p1 = s.tile;
			p2 = bridge_base | (uint32)(s.data & RP_STEP_VALUE_MASK);
			cmd = CMD_BUILD_BRIDGE;
		} else {
			OLn("road execute bad kind ", (uint32)i);
			return OAI_WORK_FAILED;
		}

		CommandCost test = DoCommand(command_tile, p1, p2, DC_NONE, cmd);
		if (test.Failed()) {
			/* An already-present object is usable but not attempt-owned, so
			 * leave RP_STEP_BUILT clear. */
			if (OldAICommandAlreadySatisfied(cmd, test.GetErrorMessage())) {
				a->plan_cursor++;
				continue;
			}
			OLn("road execute test step ", (uint32)i);
			OLn("road execute test err ", (uint32)test.GetErrorMessage());
			return OAI_WORK_FAILED;
		}
		StringID error;
		OldAIWorkResult wr = OldAICommand(a, OAOP_ROAD_PLAN_BUILD,
				command_tile, p1, p2, cmd, NULL, &error);
		if (wr == OAI_WORK_WAIT) return wr;
		if (wr == OAI_WORK_FAILED) {
			OLn("road execute real step ", (uint32)i);
			OLn("road execute real err ", (uint32)error);
			return wr;
		}
		if (s.kind != ROADSTEP_LEVEL) s.data |= RP_STEP_BUILT;
		a->plan_cursor++;
	}
	a->plan_cursor = 0;
	return OAI_WORK_DONE;
}
