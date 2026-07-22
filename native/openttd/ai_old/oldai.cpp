/* Native C++ computer opponent for the Amiga 68k port.
 *
 * The stock 1.0.5 AI is Squirrel, and its AI_VMSuspend exception cannot unwind
 * through the VM in this port's Hunk executable, so any Squirrel AI aborts the
 * game. This is plain C++ that drives an AI company through the native
 * DoCommandP - which throws nothing - so it sidesteps the whole problem.
 *
 * It follows the strategy of the pre-1.0 in-tree road AI ("trolly"): a per-
 * company state machine that finds a town, builds two bus stops and a depot,
 * buys a bus and puts it on a route. Written against the 1.0.5 API (not a
 * line-by-line port of the drifted 0.6.3 source) and reusing the game's own
 * road-vehicle pathfinder, which already works here.
 */
#include "../stdafx.h"
#include "../openttd.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../command_func.h"
#include "../settings_type.h"
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
#include "../cargotype.h"
#include "../engine_base.h"
#include "../engine_func.h"
#include "../vehicle_base.h"
#include "../vehicle_func.h"
#include "../order_base.h"
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
	OAS_ORDERS,
	OAS_START,
	OAS_DONE,
	OAS_GIVEUP,
};

struct OldAICompany {
	bool active;
	uint age;
	OldAIState state;
	int  tries;

	TileIndex aend, bend, corner;  ///< the L-road: aend -> corner (row) -> bend (col)
	TileIndex stopA, frontA;
	TileIndex stopB, frontB;
	TileIndex depot, depot_front;
	DiagDirection depot_dir;   ///< direction from depot_front (road) to depot
	StationID staA, staB;
	VehicleID bus;
	int routes_done;           ///< how many complete routes this AI has built
	int buses_on_route;        ///< buses started on the current route
	int town_skip;             ///< towns skipped because they had no buildable spot
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

/* The town nearest to 'from' (other than 'from' itself) whose distance is in a
 * sensible band for a profitable bus route. NULL if none. */
static const Town *FindPartnerTown(const Town *from)
{
	const Town *best = NULL; uint bestd = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) {
		if (t == from || t->population < 100) continue;
		uint d = DistanceManhattan(from->xy, t->xy);
		if (d < 15 || d > 90) continue;          /* long enough to earn, not absurd */
		if (best == NULL || d < bestd) { best = t; bestd = d; }
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
static bool BuildBusStop(TileIndex tile, TileIndex front)
{
	uint entrance = (TileY(tile) != TileY(front)) ? 1 : 0;   /* AXIS: 0=X, 1=Y */
	uint p2 = 32;                                    /* STATION_NEW (not join) */
	p2 |= 2;                                          /* drive-through */
	p2 |= 0;                                          /* bus (not truck) */
	p2 |= RoadTypeToRoadTypes(ROADTYPE_ROAD) << 2;    /* road, not tram */
	p2 |= ((uint)INVALID_STATION) << 16;
	return TryCmd("stop err", tile, entrance, p2, CMD_BUILD_ROAD_STOP);
}

static void RunCompany(CompanyID cid)
{
	OldAICompany *a = &_oldai[cid];

	switch (a->state) {
		case OAS_IDLE:
			if (a->age >= 8) a->state = OAS_PLAN;
			break;

		case OAS_PLAN: {
			/* Stop once we have plenty of routes, or if cash runs low. */
			const Company *co = Company::GetIfValid(cid);
			if (co != NULL && co->money < 30000) { OL("plan: low on cash; pausing expansion"); break; }
			if (a->routes_done >= 12) { OL("plan: enough routes built"); a->state = OAS_DONE; break; }

			const Town *t = FindTownForRoute(a->routes_done + a->town_skip);
			if (t == NULL) { OL("plan: no towns"); a->state = OAS_GIVEUP; break; }
			/* Two DRIVE-THROUGH stops on straight house-side roads in one town,
			 * far apart, connected by the town's own road network (reliable). The
			 * buses turn around at the town's junctions. This is the version that
			 * reliably completed routes; inter-town road-building (OAS_BUILD_ROAD
			 * + the pathfinder/bridge helpers) is kept in the file for later. */
			if (!FindStopSpot(t->xy, INVALID_TILE, 0, &a->stopA, &a->frontA) ||
			    !FindStopSpot(t->xy, a->stopA, 10, &a->stopB, &a->frontB) ||
			    a->stopA == a->stopB ||
			    !FindDepotSpot(a->frontA, a->stopA, a->stopB, &a->depot, &a->depot_front, &a->depot_dir)) {
				OL("plan: town has no two straight house-roads; next town");
				if (++a->town_skip > 80) { OL("plan: nothing buildable left"); a->state = OAS_DONE; }
				break;
			}
			OLn("plan: in-town route, stop distance = ", DistanceManhattan(a->stopA, a->stopB));
			a->buses_on_route = 0;
			a->tries = 0;
			a->state = OAS_BUILD_STOP_A;
			break;
		}

		case OAS_BUILD_ROAD:
			OL("pathfinding a road between the towns (bridging water)");
			if (!BuildRoadPath(a->frontA, a->frontB)) {
				OL("could not connect the towns; next pair");
				if (++a->town_skip > 80) a->state = OAS_DONE; else a->state = OAS_PLAN;
				break;
			}
			OL("towns connected by road");
			/* The depot needs a clear bay next to a town road near stop A. */
			if (!FindDepotSpot(a->frontA, a->stopA, a->stopB, &a->depot, &a->depot_front, &a->depot_dir)) {
				OL("no depot spot; next pair");
				if (++a->town_skip > 80) a->state = OAS_DONE; else a->state = OAS_PLAN;
				break;
			}
			a->tries = 0;
			a->state = OAS_BUILD_STOP_A;
			break;

		case OAS_BUILD_STOP_A:
			OL("building bus stop A");
			if (BuildBusStop(a->stopA, a->frontA)) {
				a->staA = GetStationIndex(a->stopA);
				OL("stop A built");
				a->state = OAS_BUILD_STOP_B;
			} else if (++a->tries > 6) { OL("stop A failed; next town"); a->tries = 0; a->town_skip++; a->state = OAS_PLAN; }
			break;

		case OAS_BUILD_STOP_B:
			OL("building bus stop B");
			if (BuildBusStop(a->stopB, a->frontB)) {
				a->staB = GetStationIndex(a->stopB);
				OL("stop B built");
				a->state = OAS_BUILD_DEPOT;
			} else if (++a->tries > 6) { OL("stop B failed; next town"); a->tries = 0; a->town_skip++; a->state = OAS_PLAN; }
			break;

		case OAS_BUILD_DEPOT:
			OL("building road depot");
			if (TryCmd("depot err", a->depot, EntranceDir(a->depot, a->depot_front) | (0 << 2), 0, CMD_BUILD_ROAD_DEPOT)) {
				OL("depot built");
				/* Connect it: add a road piece on the road tile toward the depot,
				 * else the depot is a dead end the bus can never leave. */
				TryCmd("connect err", a->depot_front,
				       DiagDirToRoadBits(a->depot_dir) | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD);
				OL("depot connected to road");
				a->state = OAS_BUILD_BUS;
			} else if (++a->tries > 6) { OL("depot failed; next town"); a->tries = 0; a->town_skip++; a->state = OAS_PLAN; }
			break;

		case OAS_BUILD_BUS: {
			/* Build one bus per tick, order it A<->B and start it, until the
			 * route has a few buses. Then move on and build another route, so
			 * the AI runs a real network and can actually turn a profit. */
			EngineID e = FindBusEngine(cid);
			if (e == INVALID_ENGINE) { OL("no buildable bus engine yet; waiting"); break; }

			/* plain DoCommandP (no test-first) so _new_vehicle_id is the real id */
			if (!DoCommandP(a->depot, e, 0, GetCmdBuildVeh(VEH_ROAD))) {
				if (++a->tries > 8) { OL("bus build failed; next route"); a->tries = 0; a->routes_done++; a->state = OAS_PLAN; }
				break;
			}
			VehicleID bus = _new_vehicle_id;

			/* orders A then B; FAR_END is required for road vehicles */
			Order oa; oa.MakeGoToStation(a->staA); oa.SetStopLocation(OSL_PLATFORM_FAR_END); oa.SetNonStopType(ONSF_STOP_EVERYWHERE);
			Order ob; ob.MakeGoToStation(a->staB); ob.SetStopLocation(OSL_PLATFORM_FAR_END); ob.SetNonStopType(ONSF_STOP_EVERYWHERE);
			DoCommandP(0, bus | (0 << 16), oa.Pack(), CMD_INSERT_ORDER);
			DoCommandP(0, bus | (1 << 16), ob.Pack(), CMD_INSERT_ORDER);
			DoCommandP(0, bus, 0, CMD_START_STOP_VEHICLE);

			a->buses_on_route++;
			OLn("bus started, route now has buses = ", a->buses_on_route);

			/* Cap at 2 per route: more than that over-saturates a town's
			 * passenger supply and drives the route into a loss. */
			if (a->buses_on_route >= 2) {
				a->routes_done++;
				a->tries = 0;
				OLn("ROUTE COMPLETE, total routes = ", a->routes_done);
				a->state = OAS_PLAN;   /* build the next route */
			}
			break;
		}

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
