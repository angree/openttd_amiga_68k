/* Native C++ computer opponent for the Amiga 68k port.
 *
 * The stock 1.0.5 AI is Squirrel, and its AI_VMSuspend exception cannot unwind
 * through the VM in this port's Hunk executable, so any Squirrel AI aborts the
 * game. This is plain C++ that drives an AI company through the native
 * DoCommandP - which throws nothing - so it sidesteps the whole problem.
 *
 * It is a per-company native state machine for industry cargo trains, passenger
 * trains and intra-town buses. Written against the 1.0.5 API (not a line-by-line
 * port of the drifted 0.6.3 source).
 */
#include "../stdafx.h"
#include "../openttd.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../command_func.h"
#include "../settings_type.h"
#include "../economy_type.h"
#include "../economy_func.h"
#include "../debug.h"
#include "../town.h"
#include "../map_func.h"
#include "../tile_map.h"
#include "../slope_type.h"
#include "../road_map.h"
#include "../road_func.h"
#include "../water_map.h"
#include "../bridge.h"
#include "../transport_type.h"
#include "../station_map.h"
#include "../rail_map.h"
#include "../rail_type.h"
#include "../track_func.h"
#include "../cargotype.h"
#include "../engine_base.h"
#include "../engine_func.h"
#include "../vehicle_base.h"
#include "../vehicle_func.h"
#include "../order_base.h"
#include "../industry.h"
#include "../cargo_type.h"
#include "../core/random_func.hpp"
#include "oldai.h"

#include <string.h>

extern "C" void OldAI_Log(const char *s);
#define OL(s) OldAI_Log(s)

/* Log "text = N" without any printf (avoids the soft-float printf path). */
static void OLn(const char *text, uint32 n)
{
	char buf[80];
	int i = 0;
	while (text[i] && i < 60) { buf[i] = text[i]; i++; }
	char num[12]; int j = 0;
	if (n == 0) num[j++] = '0';
	while (n > 0 && j < 11) { num[j++] = (char)('0' + (n % 10)); n /= 10; }
	while (j > 0) buf[i++] = num[--j];
	buf[i] = '\0';
	OL(buf);
}

/* Test a command; on failure log its error StringID and return false. On
 * success execute it for real. Lets us see WHY a build was refused. */
static bool TryCmd(const char *what, TileIndex tile, uint32 p1, uint32 p2, uint32 cmd, const char *text = NULL)
{
	CommandCost r = DoCommand(tile, p1, p2, DC_NONE, cmd, text);
	if (r.Failed()) {
		if (r.GetErrorMessage() == 2699) return true; /* already in requested state */
		OLn(what, (uint32)r.GetErrorMessage());
		return false;
	}
	return DoCommandP(tile, p1, p2, cmd, NULL, text);
}

enum OldAIState {
	OAS_IDLE = 0,
	OAS_PLAN,          ///< pick two towns and the road endpoints
	OAS_BUILD_ROAD,    ///< build the L-shaped road between the towns
	OAS_BUILD_STOP_A,
	OAS_BUILD_STOP_B,
	OAS_BUILD_DEPOT,
	OAS_BUILD_BUS,
	OAS_BCLEANUP,      ///< retry removal/refund of a failed intra-town bus attempt
	OAS_ORDERS,
	OAS_START,
	/* Shared train states; cargo and town-passenger routes use the same build and
	 * rollback machinery. OAS_TPLAN also dispatches unlocked intra-town buses. */
	OAS_TPLAN,          ///< choose a cash-tier route and run its free pre-plan
	OAS_TBUILD_STA_A,   ///< build the producer rail station (full-load end)
	OAS_TBUILD_STA_B,   ///< build the accepter rail station (unload end)
	OAS_TBUILD_RAIL,    ///< execute the saved free exact-terrain rail plan
	OAS_TBUILD_DEPOT,   ///< build the in-line rail depot at the producer's outer end
	OAS_TBUILD_TRAIN,   ///< build loco + cargo wagons, order full-load/unload, start
	OAS_TCLEANUP,       ///< retry removal until a failed train attempt leaves no objects
	OAS_DONE,
	OAS_GIVEUP,
};

enum OldAIRouteKind {
	OARK_CARGO_TRAIN = 0,
	OARK_PASSENGER_TRAIN,
	OARK_TOWN_BUS,
};

struct OldAICompany {
	bool active;
	uint age;
	OldAIState state;
	int  tries;

	TileIndex aend, bend, corner;  ///< the L-road: aend -> corner (row) -> bend (col)
	TileIndex stopA, frontA;
	TileIndex stopB, frontB;
	RoadBits stopA_road, stopB_road; ///< town road bits restored if failed stop cleanup clears them
	TileIndex depot, depot_front;
	DiagDirection depot_dir;   ///< direction from depot_front (road) to depot
	StationID staA, staB;
	VehicleID bus;
	OldAIRouteKind route_kind; ///< machinery currently building cargo rail, passenger rail, or bus
	int routes_done;           ///< how many complete routes this AI has built
	int buses_on_route;        ///< buses started on the current route
	int town_skip;             ///< towns skipped because they had no buildable spot

	/* Shared train-route fields. Cargo uses producer/accepter industries;
	 * passengers use two town centres in prodP_tile/prodA_tile. Each endpoint gets a
	 * 5-long, 1-platform station (base = north/west tile; axis in staX_axis: 0=X,
	 * 1=Y). staX_exit is the station's INNER exit (the end that faces the main
	 * line). trStaP carries the full-load order, trStaA the unload order. The rail
	 * depot sits in-line one tile beyond the producer station's OUTER end: tdepot
	 * faces tdepot_front (the outer exit, a one-tile spur straight through the
	 * platform). */
	CargoID   tr_cargo;
	TileIndex prodP_tile, prodA_tile;
	TileIndex staP_tile, staA_tile;
	byte      staP_axis, staA_axis;
	TileIndex staP_exit, staA_exit;
	StationID trStaP, trStaA;
	TileIndex tdepot, tdepot_front;
	VehicleID train;
	byte      route_p_h; ///< producer station's independently chosen flat height
	byte      route_a_h; ///< accepter station's independently chosen flat height
	bool      attempt_sta_p;   ///< current attempt built the producer station
	bool      attempt_sta_a;   ///< current attempt built the accepter station
	bool      attempt_line;    ///< saved plan may own complete or partial line objects
	bool      attempt_spur;    ///< current attempt built the outer depot spur
	bool      attempt_depot;   ///< current attempt built the train depot
	bool      attempt_train_vehicle; ///< current attempt has built its loco in the depot
	bool      attempt_loose_wagon; ///< a built carriage still needs attaching to the loco
	VehicleID loose_wagon;     ///< retry-safe carriage id while CMD_MOVE_RAIL_VEHICLE waits
	byte      attempt_carriages; ///< carriages already attached to the current loco
	Money     attempt_money0;  ///< company money when the current attempt began
	bool      attempt_costing; ///< an attempt is spending; refund its net cost if it fails
	bool      attempt_bus_stop_a; ///< current bus attempt built stop A
	bool      attempt_bus_stop_b; ///< current bus attempt built stop B
	bool      attempt_bus_depot;  ///< current bus attempt built its road depot
	bool      attempt_bus_road;   ///< current bus attempt added the depot connector road bit
	uint      cooldown_until;  ///< _oldai_tick before which no new line may start (per-line cooldown)
};

static OldAICompany _oldai[MAX_COMPANIES];
static uint _oldai_tick;

void OldAI_Initialize()
{
	memset(_oldai, 0, sizeof(_oldai));
	_oldai_tick = 0;
}

void OldAI_Start(CompanyID company)
{
	assert(company < MAX_COMPANIES);
	OldAICompany *a = &_oldai[company];
	memset(a, 0, sizeof(*a));
	a->active = true;
	a->state  = OAS_IDLE;

	/* is_ai=true but no Squirrel instance - null the pointers so the stock AI
	 * save/load/tick code (guarded in ai_core.cpp) skips this company instead of
	 * dereferencing garbage (the autosave crashed with #80000005 otherwise). */
	Company *c = Company::GetIfValid(company);
	if (c != NULL) { c->ai_instance = NULL; c->ai_info = NULL; }

	OL("OldAI_Start: native C++ AI company created");
	DEBUG(ai, 0, "OldAI: company %d started", (int)company);
}

void OldAI_CompanyDied(CompanyID company)
{
	assert(company < MAX_COMPANIES);
	_oldai[company].active = false;
}

/* Cycle through the towns that have some population, so successive routes are
 * spread across the map instead of piling into one town (whose straight roads
 * run out once they become stations). Returns NULL if no town qualifies. */
static const Town *FindTownForRoute(int which)
{
	int n = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) if (t->population >= 100) n++;
	if (n == 0) return NULL;
	int pick = which % n;
	int i = 0;
	FOR_ALL_TOWNS(t) {
		if (t->population < 100) continue;
		if (i == pick) return t;
		i++;
	}
	return NULL;
}

/* Building on a slope needs foundations or gets refused (the original TTD
 * required flat land; 1.0.5 refuses stops/depots/road on the wrong slope with
 * STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION). Requiring flat tiles for the build
 * tile AND the tile the entrance faces is the simple, reliable rule. */
static bool IsFlat(TileIndex t)
{
	return GetTileSlope(t, NULL) == SLOPE_FLAT;
}

/* A stop/depot bay connects to its road only if BOTH tiles are flat AND at the
 * same height. If the road tile is one level higher/lower the connecting road
 * slopes and the build is refused (STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION).
 * The player's GUI auto-levels; we must pick tiles that already match. */
static bool FlatSameHeight(TileIndex bay, TileIndex road)
{
	uint hb, hr;
	if (GetTileSlope(bay,  &hb) != SLOPE_FLAT) return false;
	if (GetTileSlope(road, &hr) != SLOPE_FLAT) return false;
	return hb == hr;
}

/* A stop only earns if its catchment covers town houses (which both generate
 * and accept passengers). Count house tiles within a small radius. */
static bool HousesNear(TileIndex t)
{
	int cx = TileX(t), cy = TileY(t), houses = 0;
	for (int dy = -3; dy <= 3; dy++) {
		for (int dx = -3; dx <= 3; dx++) {
			int x = cx + dx, y = cy + dy;
			if (x < 0 || y < 0 || x >= (int)MapSizeX() || y >= (int)MapSizeY()) continue;
			if (IsTileType(TileXY(x, y), MP_HOUSE)) houses++;
		}
	}
	return houses >= 4;
}

/* A drive-through stop spot: a PURE straight (ROAD_X/ROAD_Y), flat town-road
 * tile next to houses. The stop is built on it; 'front' is the road tile along
 * the same axis (needed for the stop's orientation). Reliable placement - it
 * does not need a scarce clear bay. */
static bool FindStopSpot(TileIndex centre, TileIndex avoid, int mindist, TileIndex *stop, TileIndex *front)
{
	int cx = TileX(centre), cy = TileY(centre);
	for (int r = 1; r < 24; r++) {
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				if (abs(dx) != r && abs(dy) != r) continue;   /* ring only */
				int x = cx + dx, y = cy + dy;
				if (x < 2 || y < 2 || x >= (int)MapMaxX() - 1 || y >= (int)MapMaxY() - 1) continue;
				TileIndex rt = TileXY(x, y);
				if (!IsNormalRoadTile(rt) || !IsFlat(rt) || !HousesNear(rt)) continue;
				RoadBits rb = GetRoadBits(rt, ROADTYPE_ROAD);
				TileIndex f;
				if (rb == ROAD_X)      f = TileXY(x + 1, y);
				else if (rb == ROAD_Y) f = TileXY(x, y + 1);
				else continue;
				if (!IsNormalRoadTile(f)) continue;
				if (avoid != INVALID_TILE) {
					int md = abs((int)TileX(rt) - (int)TileX(avoid)) + abs((int)TileY(rt) - (int)TileY(avoid));
					if (md < mindist) continue;
				}
				*stop = rt; *front = f; return true;
			}
		}
	}
	return false;
}

/* Passenger cargo id (varies with cargo translation). */
static CargoID PassengerCargo()
{
	const CargoSpec *pass = CargoSpec::Get(CT_PASSENGERS);
	if (pass->IsValid() && pass->town_effect == TE_PASSENGERS) return CT_PASSENGERS;
	for (CargoID cid = 0; cid < NUM_CARGO; cid++) {
		const CargoSpec *cs = CargoSpec::Get(cid);
		if (cs->IsValid() && cs->town_effect == TE_PASSENGERS) return cid;
	}
	return CT_INVALID;
}

/* First buildable road engine that carries passengers (a bus). */
static EngineID FindBusEngine(CompanyID company)
{
	CargoID pass = PassengerCargo();
	const Engine *e;
	FOR_ALL_ENGINES_OF_TYPE(e, VEH_ROAD) {
		if (!IsEngineBuildable(e->index, VEH_ROAD, company)) continue;
		if (e->GetDefaultCargoType() == pass) return e->index;
	}
	return INVALID_ENGINE;
}

/* A road tile that has a straight through-connection in at least one axis (so a
 * drive-through stop can sit on it), with the neighbouring road tile as 'front'.
 * 'avoid' + 'mindist' let the caller ask for a tile far from the first stop, so
 * the two stops make a real route. Returns false if none in box. */
static bool FindStraightRoad(TileIndex centre, TileIndex avoid, int mindist, TileIndex *tile, TileIndex *front)
{
	int cx = TileX(centre), cy = TileY(centre);
	for (int r = 1; r < 28; r++) {
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				int x = cx + dx, y = cy + dy;
				if (x < 2 || y < 2 || x >= (int)MapMaxX() - 1 || y >= (int)MapMaxY() - 1) continue;
				TileIndex t = TileXY(x, y);
				if (!IsNormalRoadTile(t)) continue;
				/* A drive-through stop needs a PURE straight road: the tile must
				 * have no road bits in the perpendicular axis, or the command
				 * refuses it (STR_ERROR_DRIVE_THROUGH_DIRECTION, station_cmd.cpp).
				 * So accept only exactly ROAD_X or exactly ROAD_Y. */
				RoadBits rb = GetRoadBits(t, ROADTYPE_ROAD);
				TileIndex f;
				if (rb == ROAD_X)      f = TileXY(x + 1, y); /* straight E-W, front east */
				else if (rb == ROAD_Y) f = TileXY(x, y + 1); /* straight N-S, front south */
				else continue;
				if (!IsNormalRoadTile(f)) continue;
				if (!IsFlat(t) || !IsFlat(f)) continue;  /* both the stop tile and its through-neighbour flat */
				if (avoid != INVALID_TILE) {
					int md = abs((int)TileX(t) - (int)TileX(avoid)) + abs((int)TileY(t) - (int)TileY(avoid));
					if (md < mindist) continue;
				}
				*tile = t; *front = f;
				return true;
			}
		}
	}
	return false;
}

/* A buildable (clear) tile adjacent to a road tile, for a depot. Also returns
 * the direction from the road tile to the depot tile, so the caller can build a
 * connecting road bit (a depot next to a road does NOT auto-connect; the road
 * tile needs a piece on the shared edge or the depot is a dead end). */
static bool FindDepotSpot(TileIndex near, TileIndex avoid1, TileIndex avoid2, TileIndex *depot, TileIndex *front, DiagDirection *dir)
{
	int cx = TileX(near), cy = TileY(near);
	for (int r = 1; r < 10; r++) {
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				int x = cx + dx, y = cy + dy;
				if (x < 1 || y < 1 || x >= (int)MapMaxX() || y >= (int)MapMaxY()) continue;
				TileIndex rt = TileXY(x, y);
				if (rt == avoid1 || rt == avoid2) continue;
				if (!IsNormalRoadTile(rt) || !IsFlat(rt)) continue;   /* road (exit) tile flat */
				for (DiagDirection d = DIAGDIR_BEGIN; d < DIAGDIR_END; d++) {
					TileIndex ct = rt + TileOffsByDiagDir(d);
					if (ct == avoid1 || ct == avoid2) continue;
					if (IsValidTile(ct) && IsTileType(ct, MP_CLEAR) && FlatSameHeight(ct, rt)) {
						*depot = ct; *front = rt; *dir = d; return true;
					}
				}
			}
		}
	}
	return false;
}

static uint EntranceDir(TileIndex tile, TileIndex front)
{
	if (TileX(tile) == TileX(front)) return (TileY(tile) < TileY(front)) ? 1 : 3;
	return (TileX(tile) < TileX(front)) ? 2 : 0;
}

/* A clear, flat, same-height bay tile next to the given road tile (a neighbour),
 * != avoid. Used to pick the stop/depot bay AFTER the road is built, so the
 * road never sits on the tile we wanted for the stop. */
static bool FindBayAt(TileIndex road, TileIndex avoid, TileIndex *bay)
{
	for (DiagDirection d = DIAGDIR_BEGIN; d < DIAGDIR_END; d++) {
		TileIndex ct = road + TileOffsByDiagDir(d);
		if (ct == avoid) continue;
		if (IsValidTile(ct) && IsTileType(ct, MP_CLEAR) && FlatSameHeight(ct, road)) {
			*bay = ct; return true;
		}
	}
	return false;
}

/* The DiagDirection d such that 'to' == 'from' + offset(d), for adjacent tiles. */
static DiagDirection DirFromTo(TileIndex from, TileIndex to)
{
	for (DiagDirection d = DIAGDIR_BEGIN; d < DIAGDIR_END; d++) {
		if ((TileIndex)(from + TileOffsByDiagDir(d)) == to) return d;
	}
	return INVALID_DIAGDIR;
}

/* Add a road piece on 'road' toward the adjacent 'bay' tile, so a stop or depot
 * bay actually joins the road. The game does NOT auto-connect a stop built by
 * command the way the player's GUI does, so we must add the bit ourselves;
 * without it the buses can never reach the stop and get lost. Harmless if the
 * piece already exists (STR_ERROR_ALREADY_BUILT). */
static void ConnectBay(TileIndex road, TileIndex bay)
{
	DiagDirection d = DirFromTo(road, bay);
	if (d == INVALID_DIAGDIR) return;
	DoCommandP(road, DiagDirToRoadBits(d) | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD);
}

/* A town other than 'from' in the requested distance band.  Bias toward the
 * target distance, but keep a random term so a bad pair is not selected forever.
 * Passenger rail uses progressively wider bands as the company becomes richer. */
static const Town *FindPartnerTown(const Town *from, uint min_dist, uint max_dist, uint target_dist)
{
	const Town *best = NULL; uint bestscore = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) {
		if (t == from || t->population < 100) continue;
		uint d = DistanceManhattan(from->xy, t->xy);
		if (d < min_dist || d > max_dist) continue;
		uint score = (d > target_dist ? d - target_dist : target_dist - d) + RandomRange(16);
		if (best == NULL || score < bestscore) { best = t; bestscore = score; }
	}
	return best;
}

/* A flat, clear, buildable tile near 'centre' (searching outward), where the
 * road can start/end without ploughing through the town's houses. */
static bool FlatClearNear(TileIndex centre, TileIndex *out)
{
	int cx = TileX(centre), cy = TileY(centre);
	for (int r = 3; r < 12; r++) {
		for (int dy = -r; dy <= r; dy++) {
			for (int dx = -r; dx <= r; dx++) {
				if (abs(dx) != r && abs(dy) != r) continue;   /* ring only */
				int x = cx + dx, y = cy + dy;
				if (x < 3 || y < 3 || x >= (int)MapMaxX() - 3 || y >= (int)MapMaxY() - 3) continue;
				TileIndex t = TileXY(x, y);
				if (IsTileType(t, MP_CLEAR) && IsFlat(t)) { *out = t; return true; }
			}
		}
	}
	return false;
}

/* Build a straight road along a row or column (CMD_BUILD_LONG_ROAD). Both tiles
 * must share a row or column. Returns true on success (or if it was already
 * there). Fails on water or immovable obstacles. */
static bool BuildLongRoad(const char *what, TileIndex start, TileIndex end)
{
	if (start == end) return true;
	uint32 p2 = (TileY(start) != TileY(end) ? 4 : 0)   /* axis */
	          | ((start < end) ? 1 : 2)                /* which half */
	          | (ROADTYPE_ROAD << 3)
	          | (1u << 6);                             /* build over existing */
	CommandCost r = DoCommand(start, end, p2, DC_NONE, CMD_BUILD_LONG_ROAD);
	if (r.Failed()) { OLn(what, (uint32)r.GetErrorMessage()); return false; }
	return DoCommandP(start, end, p2, CMD_BUILD_LONG_ROAD);
}

/* Build a road bridge between two land bridge-heads on the same row/column,
 * spanning the water/valley between them. Tries each bridge type and uses the
 * first the game accepts for that span. */
static bool BuildBridgeSpan(TileIndex head1, TileIndex head2)
{
	uint32 type = ((uint32)TRANSPORT_ROAD << 15) | (RoadTypeToRoadTypes(ROADTYPE_ROAD) << 8);
	for (uint id = 0; id < MAX_BRIDGES; id++) {
		CommandCost r = DoCommand(head2, head1, type | id, DC_NONE, CMD_BUILD_BRIDGE);
		if (r.Succeeded()) return DoCommandP(head2, head1, type | id, CMD_BUILD_BRIDGE);
	}
	return false;
}

/* Directed road builder from 'from' to 'to': step toward the goal, laying road
 * on land and bridging over water to the far shore. Turns to the other axis
 * when the preferred step is blocked. Not a full A* (it can be defeated by
 * concave coastlines), but it handles straight runs, L-turns and water gaps -
 * which is what most town pairs need. Returns true if it reached the goal. */
static bool BuildRoadPath(TileIndex from, TileIndex to)
{
	TileIndex cur = from;
	for (int guard = 0; guard < 400 && cur != to; guard++) {
		int dx = (int)TileX(to) - (int)TileX(cur);
		int dy = (int)TileY(to) - (int)TileY(cur);
		if (dx == 0 && dy == 0) break;

		/* candidate step directions, preferred (longer) axis first */
		DiagDirection cand[2]; int nc = 0;
		DiagDirection xd = dx > 0 ? DIAGDIR_SW : DIAGDIR_NE;
		DiagDirection yd = dy > 0 ? DIAGDIR_SE : DIAGDIR_NW;
		if (abs(dx) >= abs(dy)) { if (dx) cand[nc++] = xd; if (dy) cand[nc++] = yd; }
		else                    { if (dy) cand[nc++] = yd; if (dx) cand[nc++] = xd; }

		bool moved = false;
		for (int i = 0; i < nc && !moved; i++) {
			DiagDirection d = cand[i];
			TileIndex next = cur + TileOffsByDiagDir(d);
			if (!IsValidTile(next)) continue;

			if (IsWaterTile(next)) {
				/* find the far shore, within bridge length */
				TileIndex probe = next; int span = 1;
				while (IsValidTile(probe) && IsWaterTile(probe) && span <= 16) {
					probe = probe + TileOffsByDiagDir(d); span++;
				}
				if (IsValidTile(probe) && !IsWaterTile(probe) && span <= 16 &&
				    BuildBridgeSpan(cur, probe)) {
					cur = probe; moved = true;
				}
			} else if (BuildLongRoad("path err", cur, next)) {
				cur = next; moved = true;
			}
		}
		if (!moved) return false;   /* stuck */
	}
	return DistanceManhattan(cur, to) <= 1;
}

/* Drive-through bus stop built directly on a straight town-road tile next to
 * houses. No separate clear bay is needed (bays are scarce in dense town
 * centres), so placement is reliable. Turnaround happens via the town's own
 * road junctions at each end of an inter-town route. 'tile' is the straight
 * road tile; 'front' is the adjacent road tile along the same axis. */
static CommandCost TestBusStop(TileIndex tile, TileIndex front)
{
	uint entrance = (TileY(tile) != TileY(front)) ? 1 : 0;   /* AXIS: 0=X, 1=Y */
	uint p2 = 32;                                    /* STATION_NEW (not join) */
	p2 |= 2;                                          /* drive-through */
	p2 |= 0;                                          /* bus (not truck) */
	p2 |= RoadTypeToRoadTypes(ROADTYPE_ROAD) << 2;    /* road, not tram */
	p2 |= ((uint)INVALID_STATION) << 16;
	return DoCommand(tile, entrance, p2, DC_NONE, CMD_BUILD_ROAD_STOP);
}

static bool BuildBusStop(TileIndex tile, TileIndex front)
{
	uint entrance = (TileY(tile) != TileY(front)) ? 1 : 0;
	uint p2 = 32 | 2 | (RoadTypeToRoadTypes(ROADTYPE_ROAD) << 2) | ((uint)INVALID_STATION << 16);
	return TryCmd("stop err", tile, entrance, p2, CMD_BUILD_ROAD_STOP);
}

/* ------------------------------------------------------------------------- *
 *  CARGO train route builder (industry -> industry; curves, terraform,       *
 *  bridges). Full-load at the producer, unload-and-pay at the accepter.      *
 * ------------------------------------------------------------------------- */

/* A tile we may build rail / station on: valid, and either clear land or TREES
 * (a tree tile is cleared for free when we level it or lay track, so a forest
 * is not an obstacle - and OpenTTD maps are covered in trees, so rejecting them
 * made almost every inter-industry corridor look blocked). Water is handled
 * separately (bridged). Everything else - industry, house, station, road/rail
 * we do not own - is a real obstacle. */
static bool TileLevelable(TileIndex t)
{
	return IsValidTile(t) && (IsTileType(t, MP_CLEAR) || IsTileType(t, MP_TREES));
}

/* DiagDirection (NE=0, SE=1, SW=2, NW=3) from 'from' to the orthogonally
 * adjacent 'to'. 0xFF if they are not one tile apart along a single axis. */
static byte DiagDirBetween(TileIndex from, TileIndex to)
{
	int dx = (int)TileX(to) - (int)TileX(from);
	int dy = (int)TileY(to) - (int)TileY(from);
	if (dx ==  1 && dy == 0) return 2;   /* SW  (+x) */
	if (dx == -1 && dy == 0) return 0;   /* NE  (-x) */
	if (dy ==  1 && dx == 0) return 1;   /* SE  (+y) */
	if (dy == -1 && dx == 0) return 3;   /* NW  (-y) */
	return 0xFF;
}

/* Track (0..5) that joins the two given tile edges, or 0xFF if e1 == e2.
 * {NE,SW}=X(0) {NW,SE}=Y(1) {NE,NW}=UPPER(2) {SW,SE}=LOWER(3)
 * {NW,SW}=LEFT(4) {NE,SE}=RIGHT(5). Verified against ai_rail.cpp SimulateDrag. */
static byte TrackForEdges(DiagDirection e1, DiagDirection e2)
{
	if (e1 == e2) return 0xFF;
	static const byte tbl[4][4] = {
		/* e1 = NE(0) */ { 0xFF, 5,    0,    2    },
		/* e1 = SE(1) */ { 5,    0xFF, 3,    1    },
		/* e1 = SW(2) */ { 0,    3,    0xFF, 4    },
		/* e1 = NW(3) */ { 2,    1,    4,    0xFF },
	};
	return tbl[e1][e2];
}

/* Fixed-array, modelled-terrain A* and its guarded executor.  Kept in an
 * include fragment so it can reuse the private OldAI helpers above without
 * exporting them to the rest of OpenTTD. */
#include "oldai_pathfinder.cpp"

enum { OLDAI_MAX_RAIL_PLAN = 768 };
static RailStep _oldai_rail_plan[MAX_COMPANIES][OLDAI_MAX_RAIL_PLAN];
static int _oldai_rail_plan_count[MAX_COMPANIES];

static void ResetTrainAttempt(CompanyID cid, OldAICompany *a)
{
	a->attempt_sta_p = false;
	a->attempt_sta_a = false;
	a->attempt_line = false;
	a->attempt_spur = false;
	a->attempt_depot = false;
	a->attempt_train_vehicle = false;
	a->attempt_loose_wagon = false;
	a->attempt_carriages = 0;
	if (cid < MAX_COMPANIES) _oldai_rail_plan_count[cid] = 0;
}

/* Ordered L-path tile list from exitP to exitA (both ends included): leg1 runs
 * along exitP's row to the corner (TileX(exitA), TileY(exitP)), leg2 down that
 * column to exitA. Returns the tile count, or 0 on overflow of 'maxn'. */
static int BuildLPath(TileIndex exitP, TileIndex exitA, TileIndex *out, int maxn)
{
	int px = TileX(exitP), py = TileY(exitP);
	int ax = TileX(exitA), ay = TileY(exitA);
	int n = 0;
	if (n >= maxn) return 0;
	out[n++] = exitP;
	int stepx = (ax > px) ? 1 : -1;
	for (int x = px; x != ax; ) { x += stepx; if (n >= maxn) return 0; out[n++] = TileXY(x, py); }
	int stepy = (ay > py) ? 1 : -1;
	for (int y = py; y != ay; ) { y += stepy; if (n >= maxn) return 0; out[n++] = TileXY(ax, y); }
	return n;
}

/* Corridor feasibility, cheap map scan (no DoCommand): every path tile must be
 * level-able land, or belong to a water span whose far shore is within 16 tiles
 * (bridgeable). Any immovable obstacle rejects the route. */
static bool CorridorFeasible(const TileIndex *path, int n)
{
	int i = 0;
	while (i < n) {
		if (TileLevelable(path[i])) { i++; continue; }
		if (IsValidTile(path[i]) && IsWaterTile(path[i])) {
			int span = 0;
			while (i < n && IsValidTile(path[i]) && IsWaterTile(path[i]) && span <= 16) { i++; span++; }
			if (span > 16) return false;                /* water span too wide to bridge */
			if (i >= n) return false;                   /* water ran off the end */
			if (!TileLevelable(path[i])) return false;  /* far shore not buildable */
			continue;
		}
		return false;   /* immovable obstacle */
	}
	return true;
}

/* The platform end tile adjacent to a station's exit (used only as a geometry
 * reference so the exit's own track curves correctly - never built on). */
static TileIndex PlatformAdj(TileIndex base, byte axis, TileIndex exit)
{
	int bx = TileX(base), by = TileY(base);
	if (axis == 0) return (TileX(exit) < bx) ? TileXY(bx, by) : TileXY(bx + 4, by);
	return             (TileY(exit) < by) ? TileXY(bx, by) : TileXY(bx, by + 4);
}

/* Find a 5-long, 1-platform station footprint (plus its two exit tiles) in the
 * catchment of the industry at 'ind', with all 7 tiles level-able. Prefer the
 * axis + position whose chosen exit points toward 'toward'. Returns the base
 * (north/west) tile, the axis (0=X,1=Y) and the inner exit that faces 'toward'. */
static bool FindIndustryStationSpot(TileIndex ind, TileIndex toward, TileIndex *base, byte *axis, TileIndex *exit)
{
	int cx = TileX(ind), cy = TileY(ind);
	bool found = false;
	int bestscore = -1;
	for (int dy = -4; dy <= 4; dy++) {
		for (int dx = -4; dx <= 4; dx++) {
			int sx = cx + dx, sy = cy + dy;
			for (int ax = 0; ax < 2; ax++) {
				TileIndex e0, e1;
				bool ok = true;
				if (ax == 0) {                 /* axis X: platform sx..sx+4, exits sx-1 / sx+5 */
					if (sx < 2 || sx + 5 >= (int)MapMaxX() || sy < 1 || sy >= (int)MapMaxY()) continue;
					for (int i = -1; i <= 5 && ok; i++) if (!TileLevelable(TileXY(sx + i, sy))) ok = false;
					e0 = TileXY(sx - 1, sy); e1 = TileXY(sx + 5, sy);
				} else {                       /* axis Y: platform sy..sy+4, exits sy-1 / sy+5 */
					if (sy < 2 || sy + 5 >= (int)MapMaxY() || sx < 1 || sx >= (int)MapMaxX()) continue;
					for (int i = -1; i <= 5 && ok; i++) if (!TileLevelable(TileXY(sx, sy + i))) ok = false;
					e0 = TileXY(sx, sy - 1); e1 = TileXY(sx, sy + 5);
				}
				if (!ok) continue;
				/* The station only serves the industry if it is inside its
				 * catchment. The nearest platform tile must sit within 3 tiles
				 * of the industry (CA_TRAIN is 4) - otherwise the train delivers
				 * to a station that reaches nothing and the route just loses
				 * money, which is exactly what "the stop by the power station is
				 * too far" was. Require it, then orient the exit toward the
				 * partner as before. */
				int mind = 1 << 30;
				for (int i = 0; i <= 4; i++) {
					TileIndex pt = (ax == 0) ? TileXY(sx + i, sy) : TileXY(sx, sy + i);
					int dd = (int)DistanceManhattan(pt, ind);
					if (dd < mind) mind = dd;
				}
				if (mind > 3) continue;   /* out of catchment - skip */
				TileIndex ex = (DistanceManhattan(e0, toward) <= DistanceManhattan(e1, toward)) ? e0 : e1;
				/* Closest to the industry wins; exit-toward-partner breaks ties. */
				int score = 100000 - 1000 * mind - (int)DistanceManhattan(ex, toward) + (int)RandomRange(2500);
				if (score > bestscore) {
					bestscore = score; found = true;
					*base = TileXY(sx, sy); *axis = (byte)ax; *exit = ex;
				}
			}
		}
	}
	return found;
}

/* Town equivalent of FindIndustryStationSpot.  The whole seven-tile strip must
 * be level-able, and at least one platform tile must be within Manhattan radius
 * 3 of a house.  Searching around the town centre keeps that house in the chosen
 * town's built-up area without depending on a version-specific house->town API. */
static bool FindTownStationSpot(const Town *town, TileIndex toward, TileIndex *base, byte *axis, TileIndex *exit)
{
	int cx = (int)TileX(town->xy), cy = (int)TileY(town->xy);
	bool found = false;
	int bestscore = -1;
	for (int dy = -10; dy <= 10; dy++) {
		for (int dx = -10; dx <= 10; dx++) {
			int sx = cx + dx, sy = cy + dy;
			for (int ax = 0; ax < 2; ax++) {
				TileIndex e0, e1;
				bool ok = true;
				if (ax == 0) {
					if (sx < 2 || sx + 5 >= (int)MapMaxX() || sy < 1 || sy >= (int)MapMaxY()) continue;
					for (int i = -1; i <= 5 && ok; i++) if (!TileLevelable(TileXY(sx + i, sy))) ok = false;
					e0 = TileXY(sx - 1, sy); e1 = TileXY(sx + 5, sy);
				} else {
					if (sy < 2 || sy + 5 >= (int)MapMaxY() || sx < 1 || sx >= (int)MapMaxX()) continue;
					for (int i = -1; i <= 5 && ok; i++) if (!TileLevelable(TileXY(sx, sy + i))) ok = false;
					e0 = TileXY(sx, sy - 1); e1 = TileXY(sx, sy + 5);
				}
				if (!ok) continue;

				int nearest_house = 1 << 30;
				bool dense_catchment = false;
				for (int i = 0; i < 5; i++) {
					int px = ax == 0 ? sx + i : sx;
					int py = ax == 0 ? sy : sy + i;
					if (HousesNear(TileXY(px, py))) dense_catchment = true;
					for (int hy = py - 3; hy <= py + 3; hy++) {
						for (int hx = px - 3; hx <= px + 3; hx++) {
							if (hx < 0 || hy < 0 || hx >= (int)MapSizeX() || hy >= (int)MapSizeY()) continue;
							if (!IsTileType(TileXY(hx, hy), MP_HOUSE)) continue;
							int hd = abs(hx - px) + abs(hy - py);
							if (hd <= 3 && hd < nearest_house) nearest_house = hd;
						}
					}
				}
				if (nearest_house > 3 || !dense_catchment) continue;
				TileIndex ex = DistanceManhattan(e0, toward) <= DistanceManhattan(e1, toward) ? e0 : e1;
				int centre_dist = abs(sx - cx) + abs(sy - cy);
				int score = 100000 - 2000 * nearest_house - 20 * centre_dist -
						(int)DistanceManhattan(ex, toward) + (int)RandomRange(2500);
				if (score > bestscore) {
					bestscore = score; found = true;
					*base = TileXY(sx, sy); *axis = (byte)ax; *exit = ex;
				}
			}
		}
	}
	return found;
}

/* An accepter in a caller-selected distance band. Short cargo keeps the proven
 * 24..64 range; companies at GBP100k also get a 48..128 long-line choice. */
static Industry *FindNearestAccepter(const Industry *P, CargoID C, int min_dist, int max_dist, int target_dist)
{
	Industry *A; Industry *best = NULL; int bestscore = 1 << 30;
	FOR_ALL_INDUSTRIES(A) {
		if (A == P) continue;
		bool accepts = false;
		for (int j = 0; j < 3; j++) if (A->accepts_cargo[j] == C) { accepts = true; break; }
		if (!accepts) continue;
		int d = (int)DistanceManhattan(P->location.tile, A->location.tile);
		if (d < min_dist || d > max_dist) continue;
		int score = ((d > target_dist) ? (d - target_dist) : (target_dist - d)) + (int)RandomRange(8);
		if (score < bestscore) { bestscore = score; best = A; }
	}
	return best;
}

/* Do not select a producer which already has one of this company's stations
 * nearby.  Completed routes therefore spread across industries, and a cleanup
 * regression cannot create another station beside the same producer. */
static bool IndustryHasCompanyStation(const Industry *ind, CompanyID company)
{
	int cx = (int)TileX(ind->location.tile);
	int cy = (int)TileY(ind->location.tile);
	int minx = cx > 8 ? cx - 8 : 1;
	int miny = cy > 8 ? cy - 8 : 1;
	int maxx = cx + 8 < (int)MapMaxX() ? cx + 8 : (int)MapMaxX() - 1;
	int maxy = cy + 8 < (int)MapMaxY() ? cy + 8 : (int)MapMaxY() - 1;
	for (int y = miny; y <= maxy; y++) {
		for (int x = minx; x <= maxx; x++) {
			TileIndex t = TileXY(x, y);
			if (IsTileType(t, MP_STATION) && GetTileOwner(t) == company) return true;
		}
	}
	return false;
}

/* Level the whole 5-tile run + both exit tiles to this station's own height.
 * Test all seven commands before changing anything, then recompute each signed
 * delta immediately before executing (an adjacent terraform may have changed
 * TileHeight meanwhile). */
static bool LevelStationFootprint(TileIndex base, byte axis, int height)
{
	int bx = TileX(base), by = TileY(base);
	/* The 7-tile strip: the 5 platform tiles plus the exit tile at each end. */
	TileIndex t0 = (axis == 0) ? TileXY(bx - 1, by) : TileXY(bx, by - 1);
	TileIndex t6 = (axis == 0) ? TileXY(bx + 5, by) : TileXY(bx, by + 5);
	/* Level the WHOLE strip to `height` in ONE area operation. Doing it tile by
	 * tile (as before) re-sloped each neighbour's shared corner - terrain is
	 * corner-based - so the footprint ended up NOT flat and
	 * CMD_BUILD_RAIL_STATION then failed with err 2677 (flat land required).
	 * CmdLevelLand levels the whole rectangle (p1..tile) to TileHeight(p1)+p2 at
	 * once, so all seven tiles come out flat at `height` - exactly the terrain the
	 * free plan modelled for its station-footprint corner overlay. p2 is the
	 * signed height delta off the START tile t0. */
	uint32 p2 = (uint32)(uint8)(int8)(height - (int)TileHeight(t0));
	CommandCost r = DoCommand(t6, t0, p2, DC_NONE, CMD_LEVEL_LAND);
	if (r.Failed()) {
		if (r.GetErrorMessage() == 2699) return true;   /* already flat at height */
		OLn("station area-level preflight err ", (uint32)r.GetErrorMessage());
		return false;
	}
	return DoCommandP(t6, t0, p2, CMD_LEVEL_LAND);
}

/* Build a rail bridge from the land tile 'nearHead' across water to the far land
 * tile 'farHead'. Tries every bridge type and uses the first the game accepts.
 * p2 = (TRANSPORT_RAIL<<15)|(railtype<<8)|bridge_id, railtype 0 (see ai_bridge.cpp). */
static bool BuildRailBridge(TileIndex nearHead, TileIndex farHead)
{
	uint32 base = ((uint32)TRANSPORT_RAIL << 15) | (0u << 8);   /* railtype 0 */
	for (uint id = 0; id < MAX_BRIDGES; id++) {
		CommandCost r = DoCommand(farHead, nearHead, base | id, DC_NONE, CMD_BUILD_BRIDGE);
		if (r.Succeeded()) return DoCommandP(farHead, nearHead, base | id, CMD_BUILD_BRIDGE);
	}
	OL("rail bridge: no bridge type accepted");
	return false;
}

/* Execute the plan produced for free in OAS_TPLAN.  PlanRailRoute modelled the
 * exact corner heights created by both seven-tile station levelling strips, so
 * station construction makes the live map match the saved plan rather than
 * invalidating it. */
static bool BuildRailLine(CompanyID cid, OldAICompany *a)
{
	if (cid >= MAX_COMPANIES || _oldai_rail_plan_count[cid] <= 0) return false;
	OLn("tbuild: executing saved exact plan steps = ", (uint)_oldai_rail_plan_count[cid]);

	/* Mark ownership before execution.  If the executor's immediate prefix
	 * rollback is ever incomplete, OAS_TCLEANUP retries the whole plan safely. */
	a->attempt_line = true;
	return ExecuteRailPlan(_oldai_rail_plan[cid], _oldai_rail_plan_count[cid]);
}

/* Plan the in-line depot beyond the producer station's OUTER end (the platform
 * end opposite the main-line exit). tdepot_front is the outer exit (spur tile);
 * tdepot is one tile further out, facing back toward the station. */
static bool PlanProducerDepot(OldAICompany *a)
{
	int bx = TileX(a->staP_tile), by = TileY(a->staP_tile);
	TileIndex outer, dep;
	if (a->staP_axis == 0) {                 /* axis X, platform bx..bx+4 */
		if (TileX(a->staP_exit) > bx) { outer = TileXY(bx - 1, by); dep = TileXY(bx - 2, by); }
		else                          { outer = TileXY(bx + 5, by); dep = TileXY(bx + 6, by); }
	} else {                                 /* axis Y, platform by..by+4 */
		if (TileY(a->staP_exit) > by) { outer = TileXY(bx, by - 1); dep = TileXY(bx, by - 2); }
		else                          { outer = TileXY(bx, by + 5); dep = TileXY(bx, by + 6); }
	}
	if (!TileLevelable(outer) || !TileLevelable(dep)) return false;
	a->tdepot_front = outer;
	a->tdepot = dep;
	return true;
}

static bool RemoveAttemptTrack(TileIndex tile, byte track, const char *what)
{
	if (!IsTileType(tile, MP_RAILWAY)) return true;
	CommandCost test = DoCommand(tile, 0, (uint32)track, DC_NONE, CMD_REMOVE_SINGLE_RAIL);
	if (test.Failed()) {
		OLn(what, (uint32)test.GetErrorMessage());
		return false;
	}
	if (!DoCommandP(tile, 0, (uint32)track, CMD_REMOVE_SINGLE_RAIL)) {
		OL("cleanup track real command failed");
		return false;
	}
	return true;
}

static bool ClearAttemptTile(TileIndex tile, const char *what)
{
	CommandCost test = DoCommand(tile, 0, 0, DC_NONE, CMD_LANDSCAPE_CLEAR);
	if (test.Failed()) {
		OLn(what, (uint32)test.GetErrorMessage());
		return false;
	}
	if (!DoCommandP(tile, 0, 0, CMD_LANDSCAPE_CLEAR)) {
		OL("cleanup clear real command failed");
		return false;
	}
	return true;
}

static bool RemoveAttemptStation(TileIndex base, byte axis)
{
	bool ok = true;
	int bx = (int)TileX(base), by = (int)TileY(base);
	for (int i = 0; i < 5; i++) {
		TileIndex tile = axis == 0 ? TileXY(bx + i, by) : TileXY(bx, by + i);
		if (!IsTileType(tile, MP_STATION)) continue; /* already removed */
		if (GetTileOwner(tile) != _current_company) {
			OL("cleanup station ownership changed");
			ok = false;
			continue;
		}
		if (!ClearAttemptTile(tile, "cleanup station err ")) ok = false;
	}
	return ok;
}

/* Retry-safe cleanup of every infrastructure object owned by the current train
 * attempt.  Earthworks are deliberately retained: reversing individual LEVEL
 * commands on corner terrain could alter neighbouring industry/town property. */
static bool CleanupTrainAttempt(CompanyID cid, OldAICompany *a)
{
	/* A vehicle is never created until every fallible infrastructure step has
	 * succeeded. Do not tear a depot out from under one if a future edit violates
	 * that invariant; the build state retains/retries the active attempt instead. */
	if (a->attempt_train_vehicle) {
		OL("train cleanup refused: attempt vehicle still in depot");
		return false;
	}
	bool ok = true;
	if (a->attempt_depot) {
		if (!IsTileType(a->tdepot, MP_RAILWAY) || ClearAttemptTile(a->tdepot, "cleanup depot err ")) {
			a->attempt_depot = false;
		} else {
			ok = false;
		}
	}
	if (a->attempt_spur) {
		byte spur = (a->staP_axis == 0) ? 0u : 1u;
		if (RemoveAttemptTrack(a->tdepot_front, spur, "cleanup spur err ")) {
			a->attempt_spur = false;
		} else {
			ok = false;
		}
	}
	if (a->attempt_line) {
		if (RemoveRailPlan(_oldai_rail_plan[cid], _oldai_rail_plan_count[cid])) {
			a->attempt_line = false;
		} else {
			ok = false;
		}
	}
	if (a->attempt_sta_a) {
		if (RemoveAttemptStation(a->staA_tile, a->staA_axis)) {
			a->attempt_sta_a = false;
		} else {
			ok = false;
		}
	}
	if (a->attempt_sta_p) {
		if (RemoveAttemptStation(a->staP_tile, a->staP_axis)) {
			a->attempt_sta_p = false;
		} else {
			ok = false;
		}
	}
	if (ok) {
		_oldai_rail_plan_count[cid] = 0;
		OL("train attempt cleanup complete");
	}
	return ok;
}

/* A failed attempt must cost the AI NOTHING. It kept re-selecting hard pairs and
 * bleeding cash on stations/track it then demolished, and stalled for years with
 * money in the bank. After the attempt is fully cleaned up, credit its entire net
 * spend (build + terraform + demolition) back. Direct write to c->money is safe:
 * single-player, no network, thread_none - nothing to desync. Completed routes
 * keep their cost (attempt_costing is cleared on ROUTE COMPLETE). */
static void RefundFailedAttempt(CompanyID cid, OldAICompany *a)
{
	if (!a->attempt_costing) return;
	Company *c = Company::GetIfValid(cid);
	if (c != NULL) {
		Money spent = a->attempt_money0 - c->money;
		if (spent > 0) {
			c->money += spent;
			/* Also un-count it from THIS YEAR's construction expenses, or the
			 * finance graph keeps accumulating money the AI never really lost -
			 * the refund would balance the bank but not the statistics. Build,
			 * terraform and demolition all book to EXPENSES_CONSTRUCTION;
			 * yearly_expenses[0] is the current year. */
			c->yearly_expenses[0][EXPENSES_CONSTRUCTION] -= spent;
			OLn("refunded failed attempt /1000 = ", (uint)(int)(spent / 1000));
		}
	}
	a->attempt_costing = false;
}

static void AbandonTrainAttempt(CompanyID cid, OldAICompany *a)
{
	a->tries = 0;
	a->town_skip++;
	if (CleanupTrainAttempt(cid, a)) {
		RefundFailedAttempt(cid, a);
		a->state = OAS_TPLAN;
	} else {
		a->state = OAS_TCLEANUP;
	}
}

/* Depot construction is one attempt.  Flags are set immediately after each
 * successful object build so OAS_TCLEANUP can remove it on any later failure. */
static bool BuildProducerTrainDepot(OldAICompany *a)
{
	int delta = (int)TileHeight(a->tdepot_front) - (int)TileHeight(a->tdepot);
	uint32 level_p2 = (uint32)(uint8)(int8)delta;
	CommandCost level = DoCommand(a->tdepot, a->tdepot, level_p2, DC_NONE, CMD_LEVEL_LAND);
	if (level.Failed() && level.GetErrorMessage() != 2699) {
		OLn("train depot level err ", (uint32)level.GetErrorMessage());
		return false;
	}
	if (!level.Failed() && !DoCommandP(a->tdepot, a->tdepot, level_p2, CMD_LEVEL_LAND)) {
		OL("train depot level execute failed");
		return false;
	}

	byte spur = (a->staP_axis == 0) ? 0u /* TRACK_X */ : 1u /* TRACK_Y */;
	CommandCost track = DoCommand(a->tdepot_front, 0, spur, DC_NONE, CMD_BUILD_SINGLE_RAIL);
	if (track.Failed()) {
		OLn("train depot spur err ", (uint32)track.GetErrorMessage());
		return false;
	}
	if (!DoCommandP(a->tdepot_front, 0, spur, CMD_BUILD_SINGLE_RAIL)) {
		OL("train depot spur execute failed");
		return false;
	}
	a->attempt_spur = true;

	uint entrance_dir = (TileX(a->tdepot) == TileX(a->tdepot_front))
			? (TileY(a->tdepot) < TileY(a->tdepot_front) ? 1 : 3)
			: (TileX(a->tdepot) < TileX(a->tdepot_front) ? 2 : 0);
	if (!TryCmd("train depot err ", a->tdepot, 0 /* railtype */, entrance_dir, CMD_BUILD_TRAIN_DEPOT)) return false;
	a->attempt_depot = true;
	return true;
}

/* First buildable train LOCO (not a wagon) with real power. */
static EngineID FindTrainLoco(CompanyID company)
{
	const Engine *e;
	FOR_ALL_ENGINES_OF_TYPE(e, VEH_TRAIN) {
		if (!IsEngineBuildable(e->index, VEH_TRAIN, company)) continue;
		if (e->u.rail.railveh_type == RAILVEH_WAGON) continue;   /* must be a loco */
		if (e->u.rail.power == 0) continue;                      /* must actually pull */
		return e->index;
	}
	return INVALID_ENGINE;
}

/* First buildable train WAGON whose default cargo is 'cargo'. */
static EngineID FindCargoWagon(CompanyID company, CargoID cargo)
{
	const Engine *e;
	FOR_ALL_ENGINES_OF_TYPE(e, VEH_TRAIN) {
		if (!IsEngineBuildable(e->index, VEH_TRAIN, company)) continue;
		if (e->u.rail.railveh_type != RAILVEH_WAGON) continue;
		if (e->GetDefaultCargoType() == cargo) return e->index;
	}
	return INVALID_ENGINE;
}

static void BeginCostedAttempt(CompanyID cid, OldAICompany *a)
{
	const Company *c = Company::GetIfValid(cid);
	a->attempt_money0 = c != NULL ? c->money : (Money)0;
	a->attempt_costing = true;
}

/* Random producer/cargo selection inside one cash-gated distance band. */
static bool SelectCargoPair(CompanyID cid, int min_dist, int max_dist, int target_dist,
		Industry **out_p, Industry **out_a, CargoID *out_c)
{
	Industry *p;
	int producer_count = 0;
	FOR_ALL_INDUSTRIES(p) {
		if (IndustryHasCompanyStation(p, cid)) continue;
		bool feasible = false;
		for (int k = 0; k < 2; k++) {
			CargoID cargo = p->produced_cargo[k];
			if (cargo == CT_INVALID || p->last_month_production[k] == 0) continue;
			if (FindNearestAccepter(p, cargo, min_dist, max_dist, target_dist) != NULL) feasible = true;
		}
		if (feasible) producer_count++;
	}
	if (producer_count == 0) return false;

	int producer_pick = (int)RandomRange(producer_count);
	int producer_index = 0;
	FOR_ALL_INDUSTRIES(p) {
		if (IndustryHasCompanyStation(p, cid)) continue;
		CargoID cargo_list[2];
		Industry *accept_list[2];
		int cargo_count = 0;
		for (int k = 0; k < 2; k++) {
			CargoID cargo = p->produced_cargo[k];
			if (cargo == CT_INVALID || p->last_month_production[k] == 0) continue;
			Industry *accept = FindNearestAccepter(p, cargo, min_dist, max_dist, target_dist);
			if (accept == NULL) continue;
			cargo_list[cargo_count] = cargo;
			accept_list[cargo_count] = accept;
			cargo_count++;
		}
		if (cargo_count == 0) continue;
		if (producer_index++ != producer_pick) continue;
		int pick = (int)RandomRange(cargo_count);
		*out_p = p; *out_a = accept_list[pick]; *out_c = cargo_list[pick];
		return true;
	}
	return false;
}

static bool SelectTownPair(uint min_dist, uint max_dist, uint target_dist,
		const Town **out_a, const Town **out_b)
{
	int town_count = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) if (t->population >= 100) town_count++;
	if (town_count < 2) return false;
	int start = (int)RandomRange(town_count);
	for (int i = 0; i < town_count; i++) {
		const Town *from = FindTownForRoute(start + i);
		if (from == NULL) continue;
		const Town *to = FindPartnerTown(from, min_dist, max_dist, target_dist);
		if (to != NULL) { *out_a = from; *out_b = to; return true; }
	}
	return false;
}

/* Shared FREE pre-plan gate for cargo and passenger rail.  No command has been
 * executed when this returns false; costing begins only after the complete plan
 * has been accepted and cached. */
static bool PrepareFreeRailPlan(CompanyID cid, OldAICompany *a)
{
	if (!PlanProducerDepot(a)) return false;
	a->route_p_h = (byte)TileHeight(a->staP_tile);
	a->route_a_h = (byte)TileHeight(a->staA_tile);
	TileIndex plat_p = PlatformAdj(a->staP_tile, a->staP_axis, a->staP_exit);
	TileIndex plat_a = PlatformAdj(a->staA_tile, a->staA_axis, a->staA_exit);
	byte pdir = DiagDirBetween(plat_p, a->staP_exit);
	byte adir = DiagDirBetween(plat_a, a->staA_exit);
	if (pdir == 0xFF || adir == 0xFF || cid >= MAX_COMPANIES) return false;

	_oldai_rail_plan_count[cid] = 0;
	if (!PlanRailRoute(a->staP_exit, (DiagDirection)pdir, a->route_p_h,
			a->staA_exit, (DiagDirection)adir, a->route_a_h,
			_oldai_rail_plan[cid], &_oldai_rail_plan_count[cid], OLDAI_MAX_RAIL_PLAN)) return false;
	OLn("tplan: free exact plan OK, steps = ", (uint)_oldai_rail_plan_count[cid]);
	BeginCostedAttempt(cid, a);
	a->tries = 0;
	a->state = OAS_TBUILD_STA_A;
	return true;
}

static bool PrepareCargoTrain(CompanyID cid, OldAICompany *a, int min_dist, int max_dist, int target_dist)
{
	Industry *prod = NULL, *accept = NULL;
	CargoID cargo = CT_INVALID;
	if (!SelectCargoPair(cid, min_dist, max_dist, target_dist, &prod, &accept, &cargo)) return false;
	if (FindCargoWagon(cid, cargo) == INVALID_ENGINE) return false;
	ResetTrainAttempt(cid, a);
	a->route_kind = OARK_CARGO_TRAIN;
	a->tr_cargo = cargo;
	a->prodP_tile = prod->location.tile;
	a->prodA_tile = accept->location.tile;
	if (!FindIndustryStationSpot(a->prodP_tile, a->prodA_tile, &a->staP_tile, &a->staP_axis, &a->staP_exit)) return false;
	if (!FindIndustryStationSpot(a->prodA_tile, a->prodP_tile, &a->staA_tile, &a->staA_axis, &a->staA_exit)) return false;
	if (!PrepareFreeRailPlan(cid, a)) return false;
	OLn("tplan: cargo id = ", (uint)cargo);
	OLn("tplan: cargo distance = ", DistanceManhattan(a->prodP_tile, a->prodA_tile));
	return true;
}

static bool PreparePassengerTrain(CompanyID cid, OldAICompany *a, uint min_dist, uint max_dist, uint target_dist)
{
	CargoID passengers = PassengerCargo();
	if (passengers == CT_INVALID || FindCargoWagon(cid, passengers) == INVALID_ENGINE) return false;
	const Town *ta = NULL, *tb = NULL;
	if (!SelectTownPair(min_dist, max_dist, target_dist, &ta, &tb)) return false;
	ResetTrainAttempt(cid, a);
	a->route_kind = OARK_PASSENGER_TRAIN;
	a->tr_cargo = passengers;
	a->prodP_tile = ta->xy;
	a->prodA_tile = tb->xy;
	if (!FindTownStationSpot(ta, tb->xy, &a->staP_tile, &a->staP_axis, &a->staP_exit)) return false;
	if (!FindTownStationSpot(tb, ta->xy, &a->staA_tile, &a->staA_axis, &a->staA_exit)) return false;
	if (!PrepareFreeRailPlan(cid, a)) return false;
	OLn("tplan: passenger cargo id = ", (uint)passengers);
	OLn("tplan: town distance = ", DistanceManhattan(ta->xy, tb->xy));
	return true;
}

static void ResetBusAttempt(OldAICompany *a)
{
	a->attempt_bus_stop_a = false;
	a->attempt_bus_stop_b = false;
	a->attempt_bus_depot = false;
	a->attempt_bus_road = false;
}

static bool PrepareTownBus(CompanyID cid, OldAICompany *a)
{
	int town_count = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) if (t->population >= 100) town_count++;
	if (town_count == 0 || FindBusEngine(cid) == INVALID_ENGINE) return false;
	int start = (int)RandomRange(town_count);
	for (int i = 0; i < town_count; i++) {
		t = FindTownForRoute(start + i);
		if (t == NULL) continue;
		if (!FindStopSpot(t->xy, INVALID_TILE, 0, &a->stopA, &a->frontA)) continue;
		if (!FindStopSpot(t->xy, a->stopA, 10, &a->stopB, &a->frontB) || a->stopA == a->stopB) continue;
		if (DistanceManhattan(t->xy, a->stopA) > 20 || DistanceManhattan(t->xy, a->stopB) > 20) continue;
		if (!FindDepotSpot(a->frontA, a->stopA, a->stopB, &a->depot, &a->depot_front, &a->depot_dir)) continue;
		CommandCost sa = TestBusStop(a->stopA, a->frontA);
		CommandCost sb = TestBusStop(a->stopB, a->frontB);
		CommandCost dp = DoCommand(a->depot, EntranceDir(a->depot, a->depot_front), 0, DC_NONE, CMD_BUILD_ROAD_DEPOT);
		if (sa.Failed() || sb.Failed() || dp.Failed()) continue;
		RoadBits connector = DiagDirToRoadBits(a->depot_dir);
		if ((GetRoadBits(a->depot_front, ROADTYPE_ROAD) & connector) == 0) {
			CommandCost road = DoCommand(a->depot_front, connector | (ROADTYPE_ROAD << 4), 0, DC_NONE, CMD_BUILD_ROAD);
			if (road.Failed() && road.GetErrorMessage() != 2699) continue;
		}
		ResetBusAttempt(a);
		a->stopA_road = GetRoadBits(a->stopA, ROADTYPE_ROAD);
		a->stopB_road = GetRoadBits(a->stopB, ROADTYPE_ROAD);
		a->route_kind = OARK_TOWN_BUS;
		a->buses_on_route = 0;
		a->tries = 0;
		BeginCostedAttempt(cid, a);
		a->state = OAS_BUILD_STOP_A;
		OLn("tplan: free intra-town bus plan, stop distance = ", DistanceManhattan(a->stopA, a->stopB));
		return true;
	}
	return false;
}

static bool RemoveAttemptRoadBit(TileIndex tile, RoadBits bit)
{
	if (!IsNormalRoadTile(tile) || (GetRoadBits(tile, ROADTYPE_ROAD) & bit) == 0) return true;
	/* 1.0.5 has no single-bit road-remove command; CMD_REMOVE_LONG_ROAD removes one
	 * axis over a drag. Our connector is a SPUR perpendicular to the town road, so a
	 * 1-tile drag along the spur's axis removes only the spur. p1 = end tile (= start,
	 * single tile), p2 bit2 = axis, bits3-4 = roadtype (ROADTYPE_ROAD = 0). */
	Axis axis = (bit & (ROAD_NE | ROAD_SW)) ? AXIS_X : AXIS_Y;
	uint32 p2 = ((uint32)axis << 2) | ((uint32)ROADTYPE_ROAD << 3);
	CommandCost test = DoCommand(tile, tile, p2, DC_NONE, CMD_REMOVE_LONG_ROAD);
	if (test.Failed()) { OLn("cleanup bus road err ", (uint32)test.GetErrorMessage()); return false; }
	return DoCommandP(tile, tile, p2, CMD_REMOVE_LONG_ROAD);
}

static bool RestoreAttemptRoad(TileIndex tile, RoadBits original)
{
	RoadBits present = IsNormalRoadTile(tile) ? GetRoadBits(tile, ROADTYPE_ROAD) : ROAD_NONE;
	RoadBits missing = (RoadBits)(original & ~present);
	if (missing == ROAD_NONE) return true;
	return TryCmd("cleanup restore town road err ", tile,
			missing | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD);
}

static bool CleanupBusAttempt(OldAICompany *a)
{
	bool ok = true;
	if (a->attempt_bus_depot) {
		if (!IsTileType(a->depot, MP_ROAD) || ClearAttemptTile(a->depot, "cleanup bus depot err ")) a->attempt_bus_depot = false;
		else ok = false;
	}
	if (a->attempt_bus_road) {
		if (RemoveAttemptRoadBit(a->depot_front, DiagDirToRoadBits(a->depot_dir))) a->attempt_bus_road = false;
		else ok = false;
	}
	if (a->attempt_bus_stop_b) {
		bool cleared = !IsTileType(a->stopB, MP_STATION) || ClearAttemptTile(a->stopB, "cleanup bus stop B err ");
		if (cleared && RestoreAttemptRoad(a->stopB, a->stopB_road)) a->attempt_bus_stop_b = false;
		else ok = false;
	}
	if (a->attempt_bus_stop_a) {
		bool cleared = !IsTileType(a->stopA, MP_STATION) || ClearAttemptTile(a->stopA, "cleanup bus stop A err ");
		if (cleared && RestoreAttemptRoad(a->stopA, a->stopA_road)) a->attempt_bus_stop_a = false;
		else ok = false;
	}
	return ok;
}

static void AbandonBusAttempt(CompanyID cid, OldAICompany *a)
{
	a->tries = 0;
	a->town_skip++;
	if (CleanupBusAttempt(a)) {
		RefundFailedAttempt(cid, a);
		a->state = OAS_TPLAN;
	} else {
		a->state = OAS_BCLEANUP;
	}
}

static void RunCompany(CompanyID cid)
{
	OldAICompany *a = &_oldai[cid];

	switch (a->state) {
		case OAS_IDLE:
			/* After aging, enter the cash-tier route selector. */
			if (a->age >= 8) a->state = OAS_TPLAN;
			break;

		case OAS_PLAN: {
			const Company *co = Company::GetIfValid(cid);
			if (co == NULL || co->money < 300000 || !PrepareTownBus(cid, a)) a->state = OAS_TPLAN;
			break;
		}

		case OAS_BUILD_ROAD:
			/* Deliberately unused: unlocked buses are intra-town and reuse the
			 * selected town's road network; never revive the inter-town road path. */
			a->state = OAS_TPLAN;
			break;

		case OAS_BUILD_STOP_A:
			OL("building bus stop A");
			if (BuildBusStop(a->stopA, a->frontA)) {
				a->staA = GetStationIndex(a->stopA);
				a->attempt_bus_stop_a = true;
				OL("stop A built");
				a->state = OAS_BUILD_STOP_B;
			} else if (++a->tries > 1) { OL("stop A failed; cleaning attempt"); AbandonBusAttempt(cid, a); }
			break;

		case OAS_BUILD_STOP_B:
			OL("building bus stop B");
			if (BuildBusStop(a->stopB, a->frontB)) {
				a->staB = GetStationIndex(a->stopB);
				a->attempt_bus_stop_b = true;
				OL("stop B built");
				a->state = OAS_BUILD_DEPOT;
			} else if (++a->tries > 1) { OL("stop B failed; cleaning attempt"); AbandonBusAttempt(cid, a); }
			break;

		case OAS_BUILD_DEPOT:
			OL("building road depot");
			if (TryCmd("depot err", a->depot, EntranceDir(a->depot, a->depot_front) | (0 << 2), 0, CMD_BUILD_ROAD_DEPOT)) {
				a->attempt_bus_depot = true;
				OL("depot built");
				/* Connect it: add a road piece on the road tile toward the depot,
				 * else the depot is a dead end the bus can never leave. */
				RoadBits connector = DiagDirToRoadBits(a->depot_dir);
				bool connector_missing = (GetRoadBits(a->depot_front, ROADTYPE_ROAD) & connector) == 0;
				if (connector_missing && !TryCmd("connect err", a->depot_front,
						connector | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD)) {
					OL("depot connector failed; cleaning attempt");
					AbandonBusAttempt(cid, a);
					break;
				}
				if (connector_missing) a->attempt_bus_road = true;
				OL("depot connected to road");
				a->state = OAS_BUILD_BUS;
			} else if (++a->tries > 1) { OL("depot failed; cleaning attempt"); AbandonBusAttempt(cid, a); }
			break;

		case OAS_BUILD_BUS: {
			/* One local bus is a complete route; later planner passes interleave
			 * another town or a train rather than saturating this town. */
			EngineID e = FindBusEngine(cid);
			if (e == INVALID_ENGINE) { OL("bus engine disappeared; cleaning attempt"); AbandonBusAttempt(cid, a); break; }

			/* plain DoCommandP (no test-first) so _new_vehicle_id is the real id */
			if (!DoCommandP(a->depot, e, 0, GetCmdBuildVeh(VEH_ROAD))) {
				if (++a->tries > 8) { OL("bus build failed; cleaning attempt"); AbandonBusAttempt(cid, a); }
				break;
			}
			VehicleID bus = _new_vehicle_id;

			/* orders A then B; FAR_END is required for road vehicles */
			Order oa; oa.MakeGoToStation(a->staA); oa.SetStopLocation(OSL_PLATFORM_FAR_END); oa.SetNonStopType(ONSF_STOP_EVERYWHERE);
			Order ob; ob.MakeGoToStation(a->staB); ob.SetStopLocation(OSL_PLATFORM_FAR_END); ob.SetNonStopType(ONSF_STOP_EVERYWHERE);
			DoCommandP(0, bus | (0 << 16), oa.Pack(), CMD_INSERT_ORDER);
			DoCommandP(0, bus | (1 << 16), ob.Pack(), CMD_INSERT_ORDER);
			DoCommandP(0, bus, 0, CMD_START_STOP_VEHICLE);

			a->bus = bus;
			a->buses_on_route = 1;
			/* Put a SECOND bus on the same route - one bus barely serves a town.
			 * Best-effort: if the second fails to build, the route still stands. */
			if (DoCommandP(a->depot, e, 0, GetCmdBuildVeh(VEH_ROAD))) {
				VehicleID bus2 = _new_vehicle_id;
				Order o2a; o2a.MakeGoToStation(a->staA); o2a.SetStopLocation(OSL_PLATFORM_FAR_END); o2a.SetNonStopType(ONSF_STOP_EVERYWHERE);
				Order o2b; o2b.MakeGoToStation(a->staB); o2b.SetStopLocation(OSL_PLATFORM_FAR_END); o2b.SetNonStopType(ONSF_STOP_EVERYWHERE);
				DoCommandP(0, bus2 | (0 << 16), o2a.Pack(), CMD_INSERT_ORDER);
				DoCommandP(0, bus2 | (1 << 16), o2b.Pack(), CMD_INSERT_ORDER);
				DoCommandP(0, bus2, 0, CMD_START_STOP_VEHICLE);
				a->buses_on_route = 2;
			}
			a->routes_done++;
			a->tries = 0;
			a->attempt_costing = false;
			ResetBusAttempt(a); /* completed objects are no longer attempt-owned */
			OLn("BUS ROUTE COMPLETE, total routes = ", a->routes_done);
			a->cooldown_until = _oldai_tick + ((uint)8192 << (4 - _settings_game.difficulty.competitor_speed));
			a->state = OAS_TPLAN;
			break;
		}

		case OAS_BCLEANUP:
			if (CleanupBusAttempt(a)) {
				RefundFailedAttempt(cid, a);
				a->tries = 0;
				a->state = OAS_TPLAN;
			} else {
				a->tries++;
				if ((a->tries & 7) == 1) OL("bus cleanup incomplete; will retry");
			}
			break;

		/* ----------------------------------------------------------------- *
		 *  TRAIN route state machine (one action per tick).                  *
		 * ----------------------------------------------------------------- */
		case OAS_TPLAN: {
			const Company *co = Company::GetIfValid(cid);
			/* Manage the loan by net cash position (money - current_loan):
			 *  - money > 1.5x loan  -> repay the loan in full; a low/zero loan lifts
			 *    the company performance rating a lot.
			 *  - money <  loan ("na minusie", net negative) -> draw the loan to the
			 *    ceiling so there is working capital to build with.
			 *  - in between (solvent but not flush) -> leave the loan as is; do not
			 *    borrow more when already in the black.
			 * Re-fetch the company after any change - money just moved. */
			if (co != NULL) {
				Money money = co->money;
				Money loan  = co->current_loan;
				if (loan > 0 && money > loan + loan / 2) {
					DoCommandP(0, 0, 1, CMD_DECREASE_LOAN);   /* p2=1: repay as much as possible */
					co = Company::GetIfValid(cid);
				} else if (money < loan && loan < _economy.max_loan) {
					Money delta = _economy.max_loan - loan;
					delta -= delta % LOAN_INTERVAL;
					if (delta > 0) {
						DoCommandP(0, (uint32)delta, 2, CMD_INCREASE_LOAN);
						co = Company::GetIfValid(cid);
					}
				}
			}
			if (co != NULL && co->money < 60000) {
				/* Keep a bigger reserve (£60k, was £30k) so a route with pricier
				 * track/terraform - or a costlier loco in a later year - does not
				 * start on money it cannot finish with. Pause until earned. Log
				 * RARELY:
				 * OL() writes to disk synchronously (dos.library), and logging
				 * this every tick floods the HD and drags the whole game to a
				 * crawl - which looked like a freeze. */
				if ((_oldai_tick & 8191) == 0) OL("tplan: low on cash; pausing");
				break;
			}
			if (co == NULL) break;
			/* Per-line cooldown, scaled by competitor_speed (0..4): after each
			 * completed line the AI waits before starting the next, so it does not
			 * build everything almost instantly regardless of the speed setting.
			 * very-fast=4 -> ~0.3 game-year (non-zero); medium=2 -> ~1.2 yr;
			 * very-slow=0 -> ~5 yr. _oldai_tick is a pure game-tick counter
			 * (~27000/year), incremented every OldAI_GameLoop call. */
			if (_oldai_tick < a->cooldown_until) break;
			if (a->routes_done >= 32) { OL("tplan: overall route cap reached"); a->state = OAS_DONE; break; }
			/* Short cargo is always candidate zero. Richer tiers append long cargo,
			 * three passenger-train bands, then intra-town buses. Start at a random
			 * candidate and wrap through the others, so unavailable or unbuildable
			 * choices fall through without grinding one type/pair forever. */
			enum PlanChoice { PC_CARGO_SHORT, PC_CARGO_LONG, PC_PASS_SHORT, PC_PASS_2X, PC_PASS_3X, PC_TOWN_BUS };
			PlanChoice choices[6];
			int choice_count = 0;
			choices[choice_count++] = PC_CARGO_SHORT;
			if (co->money >= 100000) choices[choice_count++] = PC_CARGO_LONG;
			if (co->money >= 120000) choices[choice_count++] = PC_PASS_SHORT;
			if (co->money >= 150000) choices[choice_count++] = PC_PASS_2X;
			if (co->money >= 200000) choices[choice_count++] = PC_PASS_3X;
			if (co->money >= 300000) choices[choice_count++] = PC_TOWN_BUS;

			int first = (int)RandomRange(choice_count);
			bool prepared = false;
			for (int i = 0; i < choice_count && !prepared; i++) {
				switch (choices[(first + i) % choice_count]) {
					case PC_CARGO_SHORT: prepared = PrepareCargoTrain(cid, a, 24, 64, 40); break;
					case PC_CARGO_LONG:  prepared = PrepareCargoTrain(cid, a, 48, 128, 80); break;
					case PC_PASS_SHORT:  prepared = PreparePassengerTrain(cid, a, 20, 60, 40); break;
					case PC_PASS_2X:     prepared = PreparePassengerTrain(cid, a, 40, 120, 80); break;
					case PC_PASS_3X:     prepared = PreparePassengerTrain(cid, a, 60, 180, 120); break;
					case PC_TOWN_BUS:    a->state = OAS_PLAN; prepared = true; break;
				}
			}
			if (!prepared) {
				a->town_skip++;
				if ((_oldai_tick & 1023) == 0) OL("tplan: no buildable unlocked choice this pass");
			}
			break;
		}

		case OAS_TBUILD_STA_A: {
			OL(a->route_kind == OARK_PASSENGER_TRAIN ? "building first town rail station" : "building producer rail station");
			if (!LevelStationFootprint(a->staP_tile, a->staP_axis, a->route_p_h)) {
				OL("producer station terrain changed; next pair");
				AbandonTrainAttempt(cid, a); break;
			}
			/* railtype0 | axis(bit4) | numtracks 1 (bits8..) | plat_len 5 (bits16..) | adjacent (bit24) */
			uint32 p1 = 0u | ((a->staP_axis == 1) ? (1u << 4) : 0u) | (1u << 8) | (5u << 16) | (1u << 24);
			uint32 p2 = ((uint32)INVALID_STATION) << 16;
			if (TryCmd("producer sta err", a->staP_tile, p1, p2, CMD_BUILD_RAIL_STATION)) {
				a->trStaP = GetStationIndex(a->staP_tile);
				a->attempt_sta_p = true;
				OL("producer station built");
				a->state = OAS_TBUILD_STA_B;
			} else if (++a->tries > 1) { OL("producer sta failed; cleaning attempt"); AbandonTrainAttempt(cid, a); }
			break;
		}

		case OAS_TBUILD_STA_B: {
			OL(a->route_kind == OARK_PASSENGER_TRAIN ? "building second town rail station" : "building accepter rail station");
			if (!LevelStationFootprint(a->staA_tile, a->staA_axis, a->route_a_h)) {
				OL("accepter station terrain changed; next pair");
				AbandonTrainAttempt(cid, a); break;
			}
			uint32 p1 = 0u | ((a->staA_axis == 1) ? (1u << 4) : 0u) | (1u << 8) | (5u << 16) | (1u << 24);
			uint32 p2 = ((uint32)INVALID_STATION) << 16;
			if (TryCmd("accepter sta err", a->staA_tile, p1, p2, CMD_BUILD_RAIL_STATION)) {
				a->trStaA = GetStationIndex(a->staA_tile);
				a->attempt_sta_a = true;
				OL("accepter station built");
				a->state = OAS_TBUILD_RAIL;
			} else if (++a->tries > 1) { OL("accepter sta failed; cleaning attempt"); AbandonTrainAttempt(cid, a); }
			break;
		}

		case OAS_TBUILD_RAIL:
			OL(a->route_kind == OARK_PASSENGER_TRAIN ? "laying saved free-trial passenger line" : "laying saved free-trial cargo main line");
			if (BuildRailLine(cid, a)) {
				OL("main line laid");
				a->tries = 0;
				a->state = OAS_TBUILD_DEPOT;
			} else {
				OL("main line failed; cleaning attempt");
				AbandonTrainAttempt(cid, a);
			}
			break;

		case OAS_TBUILD_DEPOT: {
			OL("building in-line depot at producer outer end");
			if (BuildProducerTrainDepot(a)) {
				OL("train depot built and connected");
				a->tries = 0;
				a->state = OAS_TBUILD_TRAIN;
			} else {
				OL("train depot failed; cleaning attempt");
				AbandonTrainAttempt(cid, a);
			}
			break;
		}

		case OAS_TBUILD_TRAIN: {
			EngineID loco = FindTrainLoco(cid);
			if (loco == INVALID_ENGINE) { OL("no buildable loco yet; waiting"); break; }
			EngineID wag = FindCargoWagon(cid, a->tr_cargo);
			if (wag == INVALID_ENGINE) {
				/* Both planners checked this before spending. If availability changed
				 * before the loco exists, this is still a normal refundable failure. */
				if (!a->attempt_train_vehicle) { OL("required carriage disappeared; cleaning attempt"); AbandonTrainAttempt(cid, a); }
				else OL("required carriage unavailable; loco held in depot, waiting");
				break;
			}

			/* Build the loco on one tick and the required first carriage on the
			 * next. Once a loco exists we never abandon/remove its depot; a temporary
			 * carriage shortage just waits, preventing an orphan vehicle. */
			if (!a->attempt_train_vehicle) {
				if (!DoCommandP(a->tdepot, loco, 0, GetCmdBuildVeh(VEH_TRAIN))) {
					if (++a->tries > 8) { OL("loco build failed; cleaning attempt"); AbandonTrainAttempt(cid, a); }
					break;
				}
				a->train = _new_vehicle_id;
				a->attempt_train_vehicle = true;
				a->tries = 0;
				OL(a->route_kind == OARK_PASSENGER_TRAIN ? "passenger loco built" : "cargo loco built");
				break;
			}
			VehicleID locoid = a->train;
			if (a->attempt_loose_wagon) {
				if (!DoCommandP(0, a->loose_wagon | (locoid << 16), 0, CMD_MOVE_RAIL_VEHICLE)) {
					if ((++a->tries & 7) == 1) OL("carriage move failed; retrying without duplication");
					break;
				}
				a->attempt_loose_wagon = false;
				a->attempt_carriages++;
				a->tries = 0;
				OLn("carriage attached, count = ", (uint)a->attempt_carriages);
			}
			if (a->attempt_carriages == 0) {
				if (!DoCommandP(a->tdepot, wag, 0, GetCmdBuildVeh(VEH_TRAIN))) {
					if ((++a->tries & 7) == 1) OL("required first carriage build failed; waiting");
					break;
				}
				a->loose_wagon = _new_vehicle_id;
				a->attempt_loose_wagon = true;
				break; /* attach by stored id next tick */
			}
			for (int i = a->attempt_carriages; i < 5; i++) {
				if (!DoCommandP(a->tdepot, wag, 0, GetCmdBuildVeh(VEH_TRAIN))) { OL("optional carriage build failed; consist is sufficient"); break; }
				a->loose_wagon = _new_vehicle_id;
				a->attempt_loose_wagon = true;
				if (!DoCommandP(0, a->loose_wagon | (locoid << 16), 0, CMD_MOVE_RAIL_VEHICLE)) {
					OL("optional carriage move delayed; retrying next tick");
					break;
				}
				a->attempt_loose_wagon = false;
				a->attempt_carriages++;
				OLn("carriage attached, count = ", (uint)a->attempt_carriages);
			}
			if (a->attempt_loose_wagon) break;

			/* Orders: FULL LOAD at the producer; DEFAULT unload at the accepter, which
			 * unloads AND gets paid (OUFB_UNLOAD would force-dump the cargo for no
			 * money). So set no load/unload flag on the delivery order. */
			Order op; op.MakeGoToStation(a->trStaP); op.SetLoadType(OLFB_FULL_LOAD); op.SetNonStopType(ONSF_STOP_EVERYWHERE);
			Order od; od.MakeGoToStation(a->trStaA); od.SetNonStopType(ONSF_STOP_EVERYWHERE);
			DoCommandP(0, locoid | (0 << 16), op.Pack(), CMD_INSERT_ORDER);
			DoCommandP(0, locoid | (1 << 16), od.Pack(), CMD_INSERT_ORDER);
			DoCommandP(0, locoid, 0, CMD_START_STOP_VEHICLE);

			a->routes_done++;
			a->tries = 0;
			a->attempt_costing = false;   /* route succeeded - its cost stands, no refund */
			OLn(a->route_kind == OARK_PASSENGER_TRAIN ? "PASSENGER TRAIN ROUTE COMPLETE, total = " : "CARGO TRAIN ROUTE COMPLETE, total = ", a->routes_done);
			a->cooldown_until = _oldai_tick + ((uint)8192 << (4 - _settings_game.difficulty.competitor_speed));
			ResetTrainAttempt(cid, a); /* keep completed infrastructure, forget attempt ownership */
			a->state = OAS_TPLAN;   /* plan the next route */
			break;
		}

		case OAS_TCLEANUP:
			if (CleanupTrainAttempt(cid, a)) {
				RefundFailedAttempt(cid, a);
				a->tries = 0;
				a->state = OAS_TPLAN;
			} else {
				a->tries++;
				if ((a->tries & 7) == 1) OL("train cleanup incomplete; will retry");
			}
			break;

		case OAS_DONE:
		case OAS_GIVEUP:
		default:
			break;
	}
}

void OldAI_GameLoop()
{
	_oldai_tick++;
	uint8 speed = _settings_game.difficulty.competitor_speed;
	if ((_oldai_tick & ((1u << (4 - speed)) - 1)) != 0) return;

	CompanyByte old_company = _current_company;
	const Company *c;
	FOR_ALL_COMPANIES(c) {
		if (!c->is_ai) continue;
		CompanyID cid = c->index;
		if (!_oldai[cid].active) continue;
		_current_company = cid;
		_oldai[cid].age++;
		RunCompany(cid);
	}
	_current_company = old_company;
}
