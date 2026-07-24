/* Native C++ computer opponent for the Amiga 68k port.
 *
 * The stock 1.0.5 AI is Squirrel, and its AI_VMSuspend exception cannot unwind
 * through the VM in this port's Hunk executable, so any Squirrel AI aborts the
 * game. This is plain C++ that drives an AI company through the native
 * DoCommandP - which throws nothing - so it sidesteps the whole problem.
 *
 * It is a per-company native state machine for industry cargo trains, passenger
 * trains and town-to-town buses. Written against the 1.0.5 API (not a
 * line-by-line port of the drifted 0.6.3 source).
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
#include "table/strings.h"
#include "../saveload/saveload.h"
#include "oldai.h"
#ifdef ENABLE_NETWORK
#include "../network/network.h"  // _networking, _network_server (server-only AI gate)
#endif

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

enum OldAIState {
	OAS_IDLE = 0,
	OAS_PLAN,          ///< pick two towns and run the free whole-road trial
	OAS_BUILD_ROAD,    ///< execute the saved, fully verified road plan
	OAS_BUILD_STOP_A,
	OAS_BUILD_STOP_B,
	OAS_BUILD_DEPOT,
	OAS_BUILD_BUS,
	OAS_BCLEANUP,      ///< retry removal/refund of a failed town-to-town bus attempt
	OAS_ORDERS,
	OAS_START,
	/* Shared train states; cargo and town-passenger routes use the same build and
	 * rollback machinery. OAS_TPLAN also dispatches unlocked inter-town buses. */
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

enum OldAIWorkResult {
	OAI_WORK_WAIT = 0,
	OAI_WORK_DONE,
	OAI_WORK_FAILED,
};

/* A company may have at most one authoritative command in flight.  The
 * callback is deliberately generic: the continuation is selected by this
 * saved operation tag, not by process-local pointers or closures. */
enum OldAIPendingOp {
	OAOP_NONE = 0,
	OAOP_RAIL_PLAN_BUILD,
	OAOP_ROAD_PLAN_BUILD,
	OAOP_RAIL_PLAN_REMOVE,
	OAOP_ROAD_PLAN_REMOVE,
	OAOP_CLEAR_TILE,
	OAOP_REMOVE_TRACK,
	OAOP_BUILD_BUS_STOP_A,
	OAOP_BUILD_BUS_STOP_B,
	OAOP_BUILD_BUS_DEPOT,
	OAOP_BUILD_BUS_CONNECTOR,
	OAOP_BUILD_BUS,
	OAOP_BUS_ORDER_A,
	OAOP_BUS_ORDER_B,
	OAOP_BUS_DISPATCH,
	OAOP_SELL_BUS,
	OAOP_REMOVE_BUS_CONNECTOR,
	OAOP_RESTORE_ROAD,
	OAOP_LEVEL_STATION_P,
	OAOP_LEVEL_STATION_A,
	OAOP_BUILD_STATION_P,
	OAOP_BUILD_STATION_A,
	OAOP_LEVEL_TRAIN_DEPOT,
	OAOP_BUILD_DEPOT_SPUR,
	OAOP_BUILD_TRAIN_DEPOT,
	OAOP_BUILD_LOCO,
	OAOP_BUILD_WAGON,
	OAOP_MOVE_WAGON,
	OAOP_TRAIN_ORDER_P,
	OAOP_TRAIN_ORDER_A,
	OAOP_TRAIN_START,
	OAOP_DECREASE_LOAN,
	OAOP_INCREASE_LOAN,
	OAOP_REFUND,
};

enum {
	OLDAI_BUS_MIN_COUNT = 2,
	OLDAI_BUS_MAX_COUNT = 8,   ///< array + save-chunk size; do NOT change (save compat)
	OLDAI_BUS_FLEET_MAX = 6,   ///< how many buses a route may actually get (<= MAX_COUNT)
	OLDAI_BUS_ROAD_TILES_PER_BUS = 12,   ///< one bus per ~12 road tiles (was 16: too few on long lines)
	OLDAI_PLAN_MIN_GAP = 128,
	OLDAI_PLAN_MAX_FAIL_STREAK = 5,
	/* DAY_TICKS is 74; 18 game-days is about 40 seconds at normal speed. */
	OLDAI_BUS_DISPATCH_INTERVAL = 18 * DAY_TICKS
};

struct OldAICompany {
	bool active;
	uint32 rng_state;       ///< private AI-only random stream; never touches _random
	uint age;
	OldAIState state;
	int  tries;

	TileIndex stopA, frontA;
	TileIndex stopB, frontB;
	RoadBits stopA_road, stopB_road; ///< town road bits restored if failed stop cleanup clears them
	TileIndex depot, depot_front;
	DiagDirection depot_dir;   ///< direction from depot_front (road) to depot
	StationID staA, staB;
	OldAIRouteKind route_kind; ///< machinery currently building cargo rail, passenger rail, or bus
	int routes_done;           ///< how many complete routes this AI has built
	int buses_on_route;        ///< stopped buses built during the current attempt
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
	bool      attempt_bus_line;   ///< saved road plan may own complete or partial objects
	byte      depot_front_road;   ///< 4-bit mask immediately before adding the connector
	uint      cooldown_until;  ///< _oldai_tick before which no new line may start (per-line cooldown)
	uint      next_plan_tick;  ///< _oldai_tick before which no new PLANNING attempt (A*) may run
	byte      plan_fail_streak; ///< server-local planning backoff; deliberately not saved
	VehicleID dispatch_bus[OLDAI_BUS_MAX_COUNT]; ///< stopped buses, in release order
	uint      next_bus_release_tick; ///< earliest tick at which the queue may release one bus
	byte      bus_target_count;      ///< fleet size selected from the planned road length
	byte      buses_waiting;         ///< completed-route buses still queued in the depot

	/* Network-command continuation.  pending_issued is process/queue state: it
	 * is saved, but cleared on load so a save taken before execution safely
	 * re-posts the command.  pending_done/result are saved for completeness.
	 * plan_cursor and cleanup_cursor make multi-command plan work resumable. */
	OldAIPendingOp pending_op;
	TileIndex pending_tile;
	uint32 pending_p1, pending_p2, pending_cmd;
	bool pending_issued;
	bool pending_done;
	bool pending_success;
	StringID pending_error;
	VehicleID pending_vehicle;
	int plan_cursor;
	int cleanup_cursor;
	byte cleanup_phase;
	byte op_step;
	bool depot_connector_was_missing;
};

static OldAICompany _oldai[MAX_COMPANIES];
static uint _oldai_tick;
static bool _oldai_logged_string_ids;
static void OldAIResetPlans();
static void OldAIResetCompanyPlan(CompanyID company);

static void OldAILogStringIDs()
{
	if (_oldai_logged_string_ids) return;
	OLn("StringID LAND_SLOPED = ", (uint32)STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
	OLn("StringID CAN_T_DO_THIS = ", (uint32)STR_ERROR_CAN_T_DO_THIS);
	OLn("StringID ALREADY_BUILT = ", (uint32)STR_ERROR_ALREADY_BUILT);
	OLn("StringID TOO_HIGH = ", (uint32)STR_ERROR_TOO_HIGH);
	OLn("StringID ALREADY_LEVELLED = ", (uint32)STR_ERROR_ALREADY_LEVELLED);
	_oldai_logged_string_ids = true;
}

static uint32 OldAISeed(uint32 game_seed, CompanyID company)
{
	uint32 x = game_seed ^ (0x9E3779B9U * ((uint32)company + 1U));
	x ^= x >> 16;
	x *= 0x85EBCA6BU;
	x ^= x >> 13;
	return x;
}

static uint32 OldAIRandom(OldAICompany *a)
{
	a->rng_state = a->rng_state * 1664525U + 1013904223U;
	return a->rng_state;
}

static uint32 OldAIRandomRange(OldAICompany *a, uint32 limit)
{
	assert(limit != 0);
	return OldAIRandom(a) % limit;
}

static uint8 OldAICompetitorSpeed()
{
	uint8 speed = _settings_game.difficulty.competitor_speed;
	if (speed > 4) speed = 4;
	return speed;
}

static uint OldAIPlanningGap(const OldAICompany *a)
{
	byte streak = a->plan_fail_streak;
	if (streak > OLDAI_PLAN_MAX_FAIL_STREAK) {
		streak = OLDAI_PLAN_MAX_FAIL_STREAK;
	}
	return (uint)OLDAI_PLAN_MIN_GAP << streak;
}

void OldAI_Initialize()
{
	memset(_oldai, 0, sizeof(_oldai));
	for (CompanyID cid = COMPANY_FIRST; cid < MAX_COMPANIES; cid++) {
		_oldai[cid].cleanup_cursor = -1;
	}
	_oldai_tick = 0;
	_oldai_logged_string_ids = false;
	OldAIResetPlans();
}

void OldAI_Start(CompanyID company)
{
	assert(company < MAX_COMPANIES);
	OldAICompany *a = &_oldai[company];
	memset(a, 0, sizeof(*a));
	a->active = true;
	a->state  = OAS_IDLE;
	a->rng_state = OldAISeed(_settings_game.game_creation.generation_seed, company);
	a->cleanup_cursor = -1;
	OldAIResetCompanyPlan(company);

	/* is_ai=true but no Squirrel instance - null the pointers so the stock AI
	 * save/load/tick code (guarded in ai_core.cpp) skips this company instead of
	 * dereferencing garbage (the autosave crashed with #80000005 otherwise). */
	Company *c = Company::GetIfValid(company);
	if (c != NULL) { c->ai_instance = NULL; c->ai_info = NULL; }

	/* Print the values compiled into this exact target.  The Amiga port has
	 * extra strings relative to stock 1.0.5, so old numeric comments are not a
	 * reliable decoder for command failures. */
	OldAILogStringIDs();
	OL("OldAI_Start: native C++ AI company created");
	DEBUG(ai, 0, "OldAI: company %d started", (int)company);
}

void OldAI_CompanyDied(CompanyID company)
{
	assert(company < MAX_COMPANIES);
	_oldai[company].active = false;
}

/* Appended after CMD_SET_TIMETABLE_START in the 1.0.5 command enum/table.
 * Keep this assertion beside the implementation so a later command table edit
 * cannot silently make the AI post a different command. */
typedef char OldAIRefundCommandMustBe109[(CMD_OLDAI_REFUND == 109) ? 1 : -1];

CommandCost CmdOldAIRefund(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	(void)tile;
	(void)flags;
	(void)text;

	/* This command deliberately has no CMD_SERVER table flag.  In 1.0.5 that
	 * flag executes the procedure as COMPANY_SPECTATOR, so neither this native
	 * AI check nor normal company accounting could succeed.  An ordinary
	 * company command is still protected: the network layer authorizes the
	 * packet's company, and this procedure accepts native-AI companies only. */
	const Company *c = Company::GetIfValid(_current_company);
	if (c == NULL || !c->is_ai || c->ai_instance != NULL) return CMD_ERROR;

	uint64 encoded = ((uint64)p2 << 32) | (uint64)p1;
	Money amount = (Money)encoded;
	if (amount <= 0) return CMD_ERROR;

	/* A negative command cost is income.  The normal command accounting path
	 * changes both company money and this year's construction expenses. */
	return CommandCost(EXPENSES_CONSTRUCTION, -amount);
}

static bool OldAIOpCreatesVehicle(OldAIPendingOp op)
{
	return op == OAOP_BUILD_BUS || op == OAOP_BUILD_LOCO || op == OAOP_BUILD_WAGON;
}

static bool OldAIIsNetworking()
{
#ifdef ENABLE_NETWORK
	return _networking;
#else
	return false;
#endif
}

static bool OldAICommandAlreadySatisfied(uint32 cmd, StringID error)
{
	if (cmd == CMD_LEVEL_LAND) return error == STR_ERROR_ALREADY_LEVELLED;
	if (cmd == CMD_BUILD_ROAD || cmd == CMD_BUILD_SINGLE_RAIL ||
			cmd == CMD_BUILD_BRIDGE) return error == STR_ERROR_ALREADY_BUILT;
	return false;
}

static bool OldAIStationTerrainError(StringID error)
{
	return error == STR_ERROR_FLAT_LAND_REQUIRED ||
			error == STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION;
}

/* Consume "already present" as success only for idempotent pending operations
 * which do not acquire attempt ownership.  In particular, do not apply this
 * to plan road/rail builds: their callers mark successful real commands as
 * attempt-owned. */
static bool OldAIPendingErrorMeansDone(const OldAICompany *a, StringID error)
{
	return (a->pending_cmd == CMD_LEVEL_LAND ||
			a->pending_op == OAOP_RESTORE_ROAD) &&
			OldAICommandAlreadySatisfied(a->pending_cmd, error);
}

/* Registered as callback 0x17 in network/network_command.cpp.  On the server
 * _current_company is the company stored in the CommandPacket while the
 * callback runs, so no pointer or client-local lookup key crosses the wire. */
void CcOldAI(const CommandCost &result, TileIndex tile, uint32 p1, uint32 p2)
{
	(void)tile;
	(void)p1;
	(void)p2;

	if (_current_company >= MAX_COMPANIES) return;
	OldAICompany *a = &_oldai[_current_company];
	if (!a->active || a->pending_op == OAOP_NONE) return;

	StringID error = result.Failed() ? result.GetErrorMessage() : INVALID_STRING_ID;
	a->pending_success = result.Succeeded() || OldAIPendingErrorMeansDone(a, error);
	a->pending_error = a->pending_success ? INVALID_STRING_ID : error;
	if (result.Succeeded() && OldAIOpCreatesVehicle(a->pending_op)) {
		a->pending_vehicle = _new_vehicle_id;
	}
	a->pending_done = true;
}

/* Post one command, or consume the completion of the matching command already
 * in flight.  In single-player CcOldAI runs inside DoCommandP, so this function
 * returns DONE/FAILED immediately and the old one-tick action topology is kept.
 * In networking it returns WAIT until the synchronized command frame executes.
 *
 * A loaded in-flight record has pending_issued=false and is posted again.  The
 * save represents the pre-command state, so this cannot duplicate an executed
 * command from that save point. */
static OldAIWorkResult OldAICommand(OldAICompany *a, OldAIPendingOp op,
		TileIndex tile, uint32 p1, uint32 p2, uint32 cmd,
		VehicleID *new_vehicle = NULL, StringID *error = NULL)
{
	if (a->pending_op == OAOP_NONE) {
		a->pending_op = op;
		a->pending_tile = tile;
		a->pending_p1 = p1;
		a->pending_p2 = p2;
		a->pending_cmd = cmd;
		a->pending_issued = false;
		a->pending_done = false;
		a->pending_success = false;
		a->pending_error = INVALID_STRING_ID;
		a->pending_vehicle = INVALID_VEHICLE;
	} else if (a->pending_op != op) {
		OL("OldAI pending command/state mismatch");
		return OAI_WORK_WAIT;
	}

	if (!a->pending_issued) {
		a->pending_issued = true;
		bool accepted = DoCommandP(a->pending_tile, a->pending_p1, a->pending_p2,
				a->pending_cmd, CcOldAI);
		/* Outside networking DoCommandP has executed the command before it
		 * returns.  CcOldAI normally records that result synchronously.  Keep
		 * an explicit fallback for command paths which do not invoke a
		 * callback (notably a rejected command): the boolean is the final
		 * single-player result, and _new_vehicle_id is still the value which
		 * the pre-D code consumed immediately after DoCommandP.
		 *
		 * Never synthesize completion while networking.  There "accepted"
		 * only means queued; the synchronized callback remains authoritative. */
		if (!a->pending_done && !OldAIIsNetworking()) {
			StringID error = accepted ? INVALID_STRING_ID : _error_message;
			a->pending_success = accepted || OldAIPendingErrorMeansDone(a, error);
			a->pending_error = a->pending_success ? INVALID_STRING_ID : error;
			if (accepted && OldAIOpCreatesVehicle(a->pending_op)) {
				a->pending_vehicle = _new_vehicle_id;
			}
			a->pending_done = true;
		}
	}
	if (!a->pending_done) return OAI_WORK_WAIT;

	bool success = a->pending_success;
	if (new_vehicle != NULL) *new_vehicle = a->pending_vehicle;
	if (error != NULL) *error = a->pending_error;
	a->pending_op = OAOP_NONE;
	a->pending_issued = false;
	a->pending_done = false;
	a->pending_vehicle = INVALID_VEHICLE;
	return success ? OAI_WORK_DONE : OAI_WORK_FAILED;
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

/* A town other than 'from' in the requested distance band.  Bias toward the
 * target distance, but keep a random term so a bad pair is not selected forever.
 * Passenger rail uses progressively wider bands as the company becomes richer. */
static const Town *FindPartnerTown(OldAICompany *a, const Town *from, uint min_dist, uint max_dist, uint target_dist)
{
	const Town *best = NULL; uint bestscore = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) {
		if (t == from || t->population < 100) continue;
		uint d = DistanceManhattan(from->xy, t->xy);
		if (d < min_dist || d > max_dist) continue;
		uint score = (d > target_dist ? d - target_dist : target_dist - d) + OldAIRandomRange(a, 16);
		if (best == NULL || score < bestscore) { best = t; bestscore = score; }
	}
	return best;
}

/* The retired inter-town builder committed a greedy L one tile/leg at a time.
 * Its attempted per-tile flattening changed shared terrain corners after earlier
 * road pieces had already been laid, so later CMD_BUILD_LONG_ROAD calls saw
 * different slopes and left paid partial roads on failure.  PlanRoadRoute now
 * models the whole corner overlay, verifies every road/bridge first, and delays
 * every real command until the complete route has passed its free trial. */

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

static OldAIWorkResult BuildBusStop(OldAICompany *a, OldAIPendingOp op,
		TileIndex tile, TileIndex front)
{
	uint entrance = (TileY(tile) != TileY(front)) ? 1 : 0;
	uint p2 = 32 | 2 | (RoadTypeToRoadTypes(ROADTYPE_ROAD) << 2) | ((uint)INVALID_STATION << 16);
	if (a->pending_op != op) {
		CommandCost test = DoCommand(tile, entrance, p2, DC_NONE, CMD_BUILD_ROAD_STOP);
		if (test.Failed()) {
			OLn("stop err", (uint32)test.GetErrorMessage());
			return OAI_WORK_FAILED;
		}
	}
	return OldAICommand(a, op, tile, entrance, p2, CMD_BUILD_ROAD_STOP);
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

enum {
	OLDAI_MAX_RAIL_PLAN = 768,
	OLDAI_MAX_ROAD_PLAN = 512
};

/* One company can only be preparing one route kind at a time.  Overlay road
 * plans with the existing rail-plan storage instead of adding another large
 * per-company array on the 68k target. */
union OldAIPlan {
	RailStep rail[OLDAI_MAX_RAIL_PLAN];
	RoadStep road[OLDAI_MAX_RAIL_PLAN];
};
typedef char OldAIPlanMustNotGrow[
		(sizeof(OldAIPlan) == sizeof(RailStep) * OLDAI_MAX_RAIL_PLAN) ? 1 : -1];
static OldAIPlan _oldai_plan[MAX_COMPANIES];
static int _oldai_rail_plan_count[MAX_COMPANIES];
static int _oldai_road_plan_count[MAX_COMPANIES];

static void OldAIResetPlans()
{
	memset(_oldai_plan, 0, sizeof(_oldai_plan));
	memset(_oldai_rail_plan_count, 0, sizeof(_oldai_rail_plan_count));
	memset(_oldai_road_plan_count, 0, sizeof(_oldai_road_plan_count));
}

static void OldAIResetCompanyPlan(CompanyID company)
{
	assert(company < MAX_COMPANIES);
	memset(&_oldai_plan[company], 0, sizeof(_oldai_plan[company]));
	_oldai_rail_plan_count[company] = 0;
	_oldai_road_plan_count[company] = 0;
}

/* ------------------------------------------------------------------------- *
 * Native OldAI save chunk.  This has its own format version because the port
 * cannot spend a global OpenTTD savegame-version number for every native-AI
 * state extension.  Every value is emitted bytewise in a defined order; no
 * compiler padding, enum width, host endianness, or raw struct layout leaks
 * into the savegame.
 * ------------------------------------------------------------------------- */
enum { OLDAI_CHUNK_VERSION = 1 };

static bool _oldai_sl_count_only;
static size_t _oldai_sl_length;

static void OldAISaveU8(uint8 v)
{
	if (_oldai_sl_count_only) {
		_oldai_sl_length++;
	} else {
		SlWriteByte(v);
	}
}

static void OldAISaveU16(uint16 v)
{
	OldAISaveU8((uint8)(v >> 8));
	OldAISaveU8((uint8)v);
}

static void OldAISaveU32(uint32 v)
{
	OldAISaveU8((uint8)(v >> 24));
	OldAISaveU8((uint8)(v >> 16));
	OldAISaveU8((uint8)(v >> 8));
	OldAISaveU8((uint8)v);
}

static void OldAISaveU64(uint64 v)
{
	OldAISaveU32((uint32)(v >> 32));
	OldAISaveU32((uint32)v);
}

static void OldAISaveCompany(CompanyID cid, const OldAICompany *a)
{
	OldAISaveU8((uint8)cid);
	OldAISaveU32(a->rng_state);
	OldAISaveU32((uint32)a->age);
	OldAISaveU8((uint8)a->state);
	OldAISaveU32((uint32)a->tries);

	OldAISaveU32(a->stopA); OldAISaveU32(a->frontA);
	OldAISaveU32(a->stopB); OldAISaveU32(a->frontB);
	OldAISaveU8((uint8)a->stopA_road); OldAISaveU8((uint8)a->stopB_road);
	OldAISaveU32(a->depot); OldAISaveU32(a->depot_front);
	OldAISaveU8((uint8)a->depot_dir);
	OldAISaveU32((uint32)a->staA); OldAISaveU32((uint32)a->staB);
	OldAISaveU8((uint8)a->route_kind);
	OldAISaveU32((uint32)a->routes_done);
	OldAISaveU32((uint32)a->buses_on_route);
	OldAISaveU32((uint32)a->town_skip);

	OldAISaveU8((uint8)a->tr_cargo);
	OldAISaveU32(a->prodP_tile); OldAISaveU32(a->prodA_tile);
	OldAISaveU32(a->staP_tile); OldAISaveU32(a->staA_tile);
	OldAISaveU8(a->staP_axis); OldAISaveU8(a->staA_axis);
	OldAISaveU32(a->staP_exit); OldAISaveU32(a->staA_exit);
	OldAISaveU32((uint32)a->trStaP); OldAISaveU32((uint32)a->trStaA);
	OldAISaveU32(a->tdepot); OldAISaveU32(a->tdepot_front);
	OldAISaveU32((uint32)a->train);
	OldAISaveU8(a->route_p_h); OldAISaveU8(a->route_a_h);

	OldAISaveU8(a->attempt_sta_p); OldAISaveU8(a->attempt_sta_a);
	OldAISaveU8(a->attempt_line); OldAISaveU8(a->attempt_spur);
	OldAISaveU8(a->attempt_depot); OldAISaveU8(a->attempt_train_vehicle);
	OldAISaveU8(a->attempt_loose_wagon);
	OldAISaveU32((uint32)a->loose_wagon);
	OldAISaveU8(a->attempt_carriages);
	OldAISaveU64((uint64)a->attempt_money0);
	OldAISaveU8(a->attempt_costing);
	OldAISaveU8(a->attempt_bus_stop_a); OldAISaveU8(a->attempt_bus_stop_b);
	OldAISaveU8(a->attempt_bus_depot); OldAISaveU8(a->attempt_bus_road);
	OldAISaveU8(a->attempt_bus_line);
	OldAISaveU8(a->depot_front_road);
	OldAISaveU32((uint32)a->cooldown_until);
	OldAISaveU32((uint32)a->next_plan_tick);
	/* plan_fail_streak is server-local scheduling state.  It intentionally
	 * resets to zero on load and is not part of the synchronized save chunk. */
	for (int i = 0; i < OLDAI_BUS_MAX_COUNT; i++) {
		OldAISaveU32((uint32)a->dispatch_bus[i]);
	}
	OldAISaveU32((uint32)a->next_bus_release_tick);
	OldAISaveU8(a->bus_target_count);
	OldAISaveU8(a->buses_waiting);

	OldAISaveU8((uint8)a->pending_op);
	OldAISaveU32(a->pending_tile);
	OldAISaveU32(a->pending_p1); OldAISaveU32(a->pending_p2);
	OldAISaveU32(a->pending_cmd);
	OldAISaveU8(a->pending_issued); OldAISaveU8(a->pending_done);
	OldAISaveU8(a->pending_success);
	OldAISaveU32((uint32)a->pending_error);
	OldAISaveU32((uint32)a->pending_vehicle);
	OldAISaveU32((uint32)a->plan_cursor);
	OldAISaveU32((uint32)a->cleanup_cursor);
	OldAISaveU8(a->cleanup_phase);
	OldAISaveU8(a->op_step);
	OldAISaveU8(a->depot_connector_was_missing);

	int rail_count = _oldai_rail_plan_count[cid];
	int road_count = _oldai_road_plan_count[cid];
	assert(rail_count >= 0 && rail_count <= OLDAI_MAX_RAIL_PLAN);
	assert(road_count >= 0 && road_count <= OLDAI_MAX_ROAD_PLAN);
	assert(rail_count == 0 || road_count == 0);
	OldAISaveU16((uint16)rail_count);
	OldAISaveU16((uint16)road_count);
	for (int i = 0; i < rail_count; i++) {
		const RailStep &s = _oldai_plan[cid].rail[i];
		OldAISaveU8(s.kind);
		OldAISaveU32(s.tile);
		OldAISaveU32(s.other);
		OldAISaveU32((uint32)s.value);
	}
	for (int i = 0; i < road_count; i++) {
		const RoadStep &s = _oldai_plan[cid].road[i];
		OldAISaveU8(s.kind);
		OldAISaveU32(s.tile);
		OldAISaveU32(s.other);
		OldAISaveU16(s.data);
	}
}

static void OldAISaveData()
{
	OldAISaveU16(OLDAI_CHUNK_VERSION);
	OldAISaveU32((uint32)_oldai_tick);
	uint8 active_count = 0;
	for (CompanyID cid = COMPANY_FIRST; cid < MAX_COMPANIES; cid++) {
		if (_oldai[cid].active) active_count++;
	}
	OldAISaveU8(active_count);
	for (CompanyID cid = COMPANY_FIRST; cid < MAX_COMPANIES; cid++) {
		if (_oldai[cid].active) OldAISaveCompany(cid, &_oldai[cid]);
	}
}

static void Save_OLAI()
{
	_oldai_sl_count_only = true;
	_oldai_sl_length = 0;
	OldAISaveData();
	SlSetLength(_oldai_sl_length);
	_oldai_sl_count_only = false;
	OldAISaveData();
}

static uint8 OldAILoadU8(size_t *left)
{
	if (*left < 1) SlErrorCorrupt("truncated OLAI chunk");
	(*left)--;
	return SlReadByte();
}

static uint16 OldAILoadU16(size_t *left)
{
	uint16 hi = OldAILoadU8(left);
	return (uint16)((hi << 8) | OldAILoadU8(left));
}

static uint32 OldAILoadU32(size_t *left)
{
	uint32 a = OldAILoadU8(left);
	uint32 b = OldAILoadU8(left);
	uint32 c = OldAILoadU8(left);
	uint32 d = OldAILoadU8(left);
	return (a << 24) | (b << 16) | (c << 8) | d;
}

static uint64 OldAILoadU64(size_t *left)
{
	uint64 hi = OldAILoadU32(left);
	return (hi << 32) | OldAILoadU32(left);
}

static void OldAILoadCompany(size_t *left, uint32 *seen)
{
	CompanyID cid = (CompanyID)OldAILoadU8(left);
	if (cid >= MAX_COMPANIES || HasBit(*seen, cid)) {
		SlErrorCorrupt("invalid or duplicate OLAI company");
	}
	SetBit(*seen, cid);
	OldAICompany *a = &_oldai[cid];
	a->active = true;
	a->rng_state = OldAILoadU32(left);
	a->age = (uint)OldAILoadU32(left);
	a->state = (OldAIState)OldAILoadU8(left);
	a->tries = (int32)OldAILoadU32(left);
	if (a->state < OAS_IDLE || a->state > OAS_GIVEUP) SlErrorCorrupt("invalid OLAI state");

	a->stopA = OldAILoadU32(left); a->frontA = OldAILoadU32(left);
	a->stopB = OldAILoadU32(left); a->frontB = OldAILoadU32(left);
	a->stopA_road = (RoadBits)OldAILoadU8(left);
	a->stopB_road = (RoadBits)OldAILoadU8(left);
	a->depot = OldAILoadU32(left); a->depot_front = OldAILoadU32(left);
	a->depot_dir = (DiagDirection)OldAILoadU8(left);
	a->staA = (StationID)OldAILoadU32(left);
	a->staB = (StationID)OldAILoadU32(left);
	a->route_kind = (OldAIRouteKind)OldAILoadU8(left);
	if (a->route_kind > OARK_TOWN_BUS) SlErrorCorrupt("invalid OLAI route kind");
	a->routes_done = (int32)OldAILoadU32(left);
	a->buses_on_route = (int32)OldAILoadU32(left);
	a->town_skip = (int32)OldAILoadU32(left);

	a->tr_cargo = (CargoID)OldAILoadU8(left);
	a->prodP_tile = OldAILoadU32(left); a->prodA_tile = OldAILoadU32(left);
	a->staP_tile = OldAILoadU32(left); a->staA_tile = OldAILoadU32(left);
	a->staP_axis = OldAILoadU8(left); a->staA_axis = OldAILoadU8(left);
	a->staP_exit = OldAILoadU32(left); a->staA_exit = OldAILoadU32(left);
	a->trStaP = (StationID)OldAILoadU32(left);
	a->trStaA = (StationID)OldAILoadU32(left);
	a->tdepot = OldAILoadU32(left); a->tdepot_front = OldAILoadU32(left);
	a->train = (VehicleID)OldAILoadU32(left);
	a->route_p_h = OldAILoadU8(left); a->route_a_h = OldAILoadU8(left);

	a->attempt_sta_p = OldAILoadU8(left) != 0;
	a->attempt_sta_a = OldAILoadU8(left) != 0;
	a->attempt_line = OldAILoadU8(left) != 0;
	a->attempt_spur = OldAILoadU8(left) != 0;
	a->attempt_depot = OldAILoadU8(left) != 0;
	a->attempt_train_vehicle = OldAILoadU8(left) != 0;
	a->attempt_loose_wagon = OldAILoadU8(left) != 0;
	a->loose_wagon = (VehicleID)OldAILoadU32(left);
	a->attempt_carriages = OldAILoadU8(left);
	a->attempt_money0 = (Money)OldAILoadU64(left);
	a->attempt_costing = OldAILoadU8(left) != 0;
	a->attempt_bus_stop_a = OldAILoadU8(left) != 0;
	a->attempt_bus_stop_b = OldAILoadU8(left) != 0;
	a->attempt_bus_depot = OldAILoadU8(left) != 0;
	a->attempt_bus_road = OldAILoadU8(left) != 0;
	a->attempt_bus_line = OldAILoadU8(left) != 0;
	a->depot_front_road = OldAILoadU8(left);
	a->cooldown_until = (uint)OldAILoadU32(left);
	a->next_plan_tick = (uint)OldAILoadU32(left);
	for (int i = 0; i < OLDAI_BUS_MAX_COUNT; i++) {
		a->dispatch_bus[i] = (VehicleID)OldAILoadU32(left);
	}
	a->next_bus_release_tick = (uint)OldAILoadU32(left);
	a->bus_target_count = OldAILoadU8(left);
	a->buses_waiting = OldAILoadU8(left);
	if (a->bus_target_count > OLDAI_BUS_MAX_COUNT ||
			a->buses_waiting > OLDAI_BUS_MAX_COUNT ||
			a->buses_on_route < 0 || a->buses_on_route > OLDAI_BUS_MAX_COUNT) {
		SlErrorCorrupt("invalid OLAI bus queue");
	}

	a->pending_op = (OldAIPendingOp)OldAILoadU8(left);
	a->pending_tile = OldAILoadU32(left);
	a->pending_p1 = OldAILoadU32(left); a->pending_p2 = OldAILoadU32(left);
	a->pending_cmd = OldAILoadU32(left);
	a->pending_issued = OldAILoadU8(left) != 0;
	a->pending_done = OldAILoadU8(left) != 0;
	a->pending_success = OldAILoadU8(left) != 0;
	a->pending_error = (StringID)OldAILoadU32(left);
	a->pending_vehicle = (VehicleID)OldAILoadU32(left);
	a->plan_cursor = (int32)OldAILoadU32(left);
	a->cleanup_cursor = (int32)OldAILoadU32(left);
	a->cleanup_phase = OldAILoadU8(left);
	a->op_step = OldAILoadU8(left);
	a->depot_connector_was_missing = OldAILoadU8(left) != 0;
	if (a->pending_op < OAOP_NONE || a->pending_op > OAOP_REFUND) {
		SlErrorCorrupt("invalid OLAI pending operation");
	}
	if (a->pending_op == OAOP_NONE) {
		a->pending_issued = false;
		a->pending_done = false;
	} else if (!a->pending_done) {
		/* The engine's transient network command queue is not part of OLAI. */
		a->pending_issued = false;
	}

	int rail_count = (int)OldAILoadU16(left);
	int road_count = (int)OldAILoadU16(left);
	if (rail_count > OLDAI_MAX_RAIL_PLAN || road_count > OLDAI_MAX_ROAD_PLAN ||
			(rail_count != 0 && road_count != 0)) {
		SlErrorCorrupt("invalid OLAI plan count");
	}
	_oldai_rail_plan_count[cid] = rail_count;
	_oldai_road_plan_count[cid] = road_count;
	for (int i = 0; i < rail_count; i++) {
		RailStep &s = _oldai_plan[cid].rail[i];
		s.kind = OldAILoadU8(left);
		s.tile = OldAILoadU32(left);
		s.other = OldAILoadU32(left);
		s.value = (int32)OldAILoadU32(left);
		if (s.kind > RAILSTEP_BRIDGE) SlErrorCorrupt("invalid OLAI rail step");
	}
	for (int i = 0; i < road_count; i++) {
		RoadStep &s = _oldai_plan[cid].road[i];
		s.kind = OldAILoadU8(left);
		s.tile = OldAILoadU32(left);
		s.other = OldAILoadU32(left);
		s.data = OldAILoadU16(left);
		s.unused = 0;
		if (s.kind > ROADSTEP_BRIDGE) SlErrorCorrupt("invalid OLAI road step");
	}

	/* Version 1 saves written by the first cursor implementation did not mark
	 * the station-build or refund half of these compound states.  Recover the
	 * substep from the authoritative pending operation / ownership flags so a
	 * save made in the regression cannot re-enter levelling or cleanup forever.
	 * Preserve a valid saved station substep when there is no pending command:
	 * that is a retry after levelling, and must not level the footprint again. */
	if (a->state == OAS_TBUILD_STA_A) {
		if (a->pending_op == OAOP_LEVEL_STATION_P) {
			a->op_step = 0;
		} else if (a->pending_op == OAOP_BUILD_STATION_P) {
			a->op_step = 1;
		} else if (a->op_step > 1) {
			SlErrorCorrupt("invalid OLAI producer station substep");
		}
	} else if (a->state == OAS_TBUILD_STA_B) {
		if (a->pending_op == OAOP_LEVEL_STATION_A) {
			a->op_step = 0;
		} else if (a->pending_op == OAOP_BUILD_STATION_A) {
			a->op_step = 1;
		} else if (a->op_step > 1) {
			SlErrorCorrupt("invalid OLAI accepter station substep");
		}
	} else if (a->state == OAS_TCLEANUP) {
		bool objects_left = a->attempt_sta_p || a->attempt_sta_a ||
				a->attempt_line || a->attempt_spur || a->attempt_depot ||
				a->attempt_train_vehicle || a->attempt_loose_wagon;
		a->op_step = (a->pending_op == OAOP_REFUND || !objects_left) ? 1 : 0;
	} else if (a->state == OAS_BCLEANUP) {
		bool objects_left = a->attempt_bus_stop_a || a->attempt_bus_stop_b ||
				a->attempt_bus_depot || a->attempt_bus_road ||
				a->attempt_bus_line || a->buses_on_route != 0;
		a->op_step = (a->pending_op == OAOP_REFUND || !objects_left) ? 1 : 0;
	}

	if (a->plan_cursor < 0 ||
			a->plan_cursor > (rail_count != 0 ? rail_count : road_count) ||
			a->cleanup_cursor < -1 ||
			a->cleanup_cursor >= OLDAI_MAX_RAIL_PLAN ||
			a->cleanup_phase > 2) {
		SlErrorCorrupt("invalid OLAI plan cursor");
	}

	/* SAVE-RESUME SAFETY. A save taken mid-attempt restores a pending command and
	 * a build/cleanup cursor; resuming it threw an uncaught C++ exception on this
	 * toolchain (fragile Hunk unwind) the instant the loaded game ran a tick, so
	 * loading any save made while the AI was building killed the game. Discard the
	 * in-flight attempt and restart planning from a clean state instead. This is
	 * deterministic (every peer loads the identical save and resets identically,
	 * so it stays MP-safe) and keeps the durable state that matters - RNG, age,
	 * completed routes, per-line cooldown and the next-plan tick. Only the single
	 * interrupted attempt is lost; its half-built objects simply stay on the map. */
	if (a->pending_op != OAOP_NONE ||
			(a->state != OAS_IDLE && a->state != OAS_TPLAN &&
			 a->state != OAS_DONE && a->state != OAS_GIVEUP)) {
		a->attempt_sta_p = a->attempt_sta_a = a->attempt_line = a->attempt_spur = false;
		a->attempt_depot = a->attempt_train_vehicle = a->attempt_loose_wagon = false;
		a->attempt_carriages = 0;
		a->attempt_costing = false;
		a->attempt_bus_stop_a = a->attempt_bus_stop_b = false;
		a->attempt_bus_depot = a->attempt_bus_road = a->attempt_bus_line = false;
		if (cid < MAX_COMPANIES) {
			_oldai_rail_plan_count[cid] = 0;
			_oldai_road_plan_count[cid] = 0;
		}
		a->plan_cursor = 0;
		a->cleanup_cursor = -1;
		a->cleanup_phase = 0;
		a->op_step = 0;
		a->pending_op = OAOP_NONE;
		a->pending_issued = false;
		a->pending_done = false;
		a->pending_cmd = 0;
		a->state = OAS_TPLAN;
	}
}

static void Load_OLAI()
{
	size_t left = SlGetFieldLength();
	uint16 version = OldAILoadU16(&left);
	if (version != OLDAI_CHUNK_VERSION) SlErrorCorrupt("unsupported OLAI chunk version");

	/* This also supplies the old-save default when the chunk is absent: normal
	 * game-load initialization calls OldAI_Initialize before chunk dispatch. */
	OldAI_Initialize();
	_oldai_tick = (uint)OldAILoadU32(&left);
	uint8 active_count = OldAILoadU8(&left);
	if (active_count > MAX_COMPANIES) SlErrorCorrupt("invalid OLAI company count");
	uint32 seen = 0;
	for (uint i = 0; i < active_count; i++) OldAILoadCompany(&left, &seen);
	if (left != 0) SlErrorCorrupt("trailing data in OLAI chunk");
	OldAILogStringIDs();
}

/* Registration point in 1.0.5 saveload/saveload.cpp:
 *   extern const ChunkHandler _oldai_chunk_handlers[];
 * and _oldai_chunk_handlers immediately after _ai_chunk_handlers in the
 * _chunk_handlers[] list. */
extern const ChunkHandler _oldai_chunk_handlers[] = {
	{ 'OLAI', Save_OLAI, Load_OLAI, NULL, CH_RIFF | CH_LAST },
};

/* Count drivable tiles in the saved plan. LEVEL steps add no length; a bridge
 * contributes both heads and every tile between them. */
static int RoadPlanTileLength(const RoadStep *plan, int n)
{
	int length = 0;
	for (int i = 0; i < n; i++) {
		if (plan[i].kind == ROADSTEP_ROAD) {
			length++;
		} else if (plan[i].kind == ROADSTEP_BRIDGE) {
			length += (int)DistanceManhattan(plan[i].tile, plan[i].other) + 1;
		}
	}
	return length;
}

static byte BusCountForRoadLength(int road_tiles)
{
	int count = (road_tiles + OLDAI_BUS_ROAD_TILES_PER_BUS - 1) /
			OLDAI_BUS_ROAD_TILES_PER_BUS;
	if (count < OLDAI_BUS_MIN_COUNT) count = OLDAI_BUS_MIN_COUNT;
	if (count > OLDAI_BUS_FLEET_MAX) count = OLDAI_BUS_FLEET_MAX;
	return (byte)count;
}

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
	a->plan_cursor = 0;
	a->cleanup_cursor = -1;
	a->cleanup_phase = 0;
	a->op_step = 0;
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
static bool FindIndustryStationSpot(OldAICompany *a, TileIndex ind, TileIndex toward, TileIndex *base, byte *axis, TileIndex *exit)
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
				int score = 100000 - 1000 * mind - (int)DistanceManhattan(ex, toward) + (int)OldAIRandomRange(a, 2500);
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
static bool FindTownStationSpot(OldAICompany *a, const Town *town, TileIndex toward, TileIndex *base, byte *axis, TileIndex *exit)
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
						(int)DistanceManhattan(ex, toward) + (int)OldAIRandomRange(a, 2500);
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
static Industry *FindNearestAccepter(OldAICompany *a, const Industry *P, CargoID C, int min_dist, int max_dist, int target_dist)
{
	Industry *A; Industry *best = NULL; int bestscore = 1 << 30;
	FOR_ALL_INDUSTRIES(A) {
		if (A == P) continue;
		bool accepts = false;
		for (int j = 0; j < 3; j++) if (A->accepts_cargo[j] == C) { accepts = true; break; }
		if (!accepts) continue;
		int d = (int)DistanceManhattan(P->location.tile, A->location.tile);
		if (d < min_dist || d > max_dist) continue;
		int score = ((d > target_dist) ? (d - target_dist) : (target_dist - d)) + (int)OldAIRandomRange(a, 8);
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

/* CMD_LEVEL_LAND addresses CORNERS using TileIndex coordinates.  A seven-tile
 * station strip occupies a 2x8 rectangle of corners, not the 1x7 rectangle of
 * tile north-corners.  The pathfinder's station overlay models this same 16
 * corner rectangle. */
static void StationFootprintArea(TileIndex base, byte axis,
		TileIndex *start, TileIndex *end)
{
	int bx = TileX(base), by = TileY(base);
	*start = axis == 0 ? TileXY(bx - 1, by) : TileXY(bx, by - 1);
	*end = axis == 0 ? TileXY(bx + 6, by + 1) : TileXY(bx + 1, by + 6);
}

static bool StationFootprintIsLevel(TileIndex base, byte axis, int height)
{
	int bx = TileX(base), by = TileY(base);
	for (int i = -1; i <= 5; i++) {
		TileIndex tile = axis == 0 ? TileXY(bx + i, by) : TileXY(bx, by + i);
		if ((int)TileHeight(tile) != height ||
				GetTileSlope(tile, NULL) != SLOPE_FLAT) return false;
	}
	return true;
}

static bool StationFootprintLevelable(TileIndex base, byte axis, int height)
{
	TileIndex start, end;
	StationFootprintArea(base, axis, &start, &end);
	uint32 p2 = (uint32)(uint8)(int8)(height - (int)TileHeight(start));
	CommandCost r = DoCommand(end, start, p2, DC_NONE, CMD_LEVEL_LAND);
	return r.Succeeded() ||
			(OldAICommandAlreadySatisfied(CMD_LEVEL_LAND,
					r.GetErrorMessage()) &&
			StationFootprintIsLevel(base, axis, height));
}

/* Level the platform plus both exits to the station's chosen height.  Validate
 * the postcondition because CMD_LEVEL_LAND is CMD_NO_TEST and can return a
 * successful partial result when execution runs out of available money. */
static OldAIWorkResult LevelStationFootprint(OldAICompany *a, OldAIPendingOp op,
		TileIndex base, byte axis, int height)
{
	TileIndex start, end;
	StationFootprintArea(base, axis, &start, &end);
	uint32 p2 = (uint32)(uint8)(int8)(height - (int)TileHeight(start));
	OldAIWorkResult wr;
	if (a->pending_op == op) {
		wr = OldAICommand(a, op, end, start, p2, CMD_LEVEL_LAND);
		if (wr != OAI_WORK_DONE) return wr;
		if (!StationFootprintIsLevel(base, axis, height)) {
			OL("station area-level incomplete after execute");
			return OAI_WORK_FAILED;
		}
		return OAI_WORK_DONE;
	}
	CommandCost r = DoCommand(end, start, p2, DC_NONE, CMD_LEVEL_LAND);
	if (r.Failed()) {
		if (OldAICommandAlreadySatisfied(CMD_LEVEL_LAND, r.GetErrorMessage()) &&
				StationFootprintIsLevel(base, axis, height)) {
			return OAI_WORK_DONE;
		}
		OLn("station area-level preflight err ", (uint32)r.GetErrorMessage());
		return OAI_WORK_FAILED;
	}
	const Company *co = Company::GetIfValid(_current_company);
	if (co != NULL && r.GetCost() > co->money) {
		OL("station area-level exceeds available money");
		return OAI_WORK_FAILED;
	}
	wr = OldAICommand(a, op, end, start, p2, CMD_LEVEL_LAND);
	if (wr != OAI_WORK_DONE) return wr;
	if (!StationFootprintIsLevel(base, axis, height)) {
		OL("station area-level incomplete after execute");
		return OAI_WORK_FAILED;
	}
	return OAI_WORK_DONE;
}

/* Execute the plan produced for free in OAS_TPLAN.  PlanRailRoute modelled the
 * exact corner heights created by both seven-tile station levelling strips, so
 * station construction makes the live map match the saved plan rather than
 * invalidating it. */
static OldAIWorkResult BuildRailLine(CompanyID cid, OldAICompany *a)
{
	if (cid >= MAX_COMPANIES || _oldai_rail_plan_count[cid] <= 0) return OAI_WORK_FAILED;
	OLn("tbuild: executing saved exact plan steps = ", (uint)_oldai_rail_plan_count[cid]);

	/* Mark ownership before execution.  A failed or interrupted prefix is
	 * removed by the callback-resumable OAS_TCLEANUP path. */
	a->attempt_line = true;
	return ExecuteRailPlan(a, _oldai_plan[cid].rail, _oldai_rail_plan_count[cid]);
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

static OldAIWorkResult RemoveAttemptTrack(OldAICompany *a, TileIndex tile, byte track, const char *what)
{
	if (a->pending_op == OAOP_REMOVE_TRACK) {
		return OldAICommand(a, OAOP_REMOVE_TRACK, tile, 0, (uint32)track,
				CMD_REMOVE_SINGLE_RAIL);
	}
	if (!IsTileType(tile, MP_RAILWAY)) return OAI_WORK_DONE;
	CommandCost test = DoCommand(tile, 0, (uint32)track, DC_NONE, CMD_REMOVE_SINGLE_RAIL);
	if (test.Failed()) {
		OLn(what, (uint32)test.GetErrorMessage());
		return OAI_WORK_FAILED;
	}
	return OldAICommand(a, OAOP_REMOVE_TRACK, tile, 0, (uint32)track,
			CMD_REMOVE_SINGLE_RAIL);
}

static OldAIWorkResult ClearAttemptTile(OldAICompany *a, TileIndex tile, const char *what)
{
	if (a->pending_op == OAOP_CLEAR_TILE) {
		return OldAICommand(a, OAOP_CLEAR_TILE, tile, 0, 0, CMD_LANDSCAPE_CLEAR);
	}
	CommandCost test = DoCommand(tile, 0, 0, DC_NONE, CMD_LANDSCAPE_CLEAR);
	if (test.Failed()) {
		OLn(what, (uint32)test.GetErrorMessage());
		return OAI_WORK_FAILED;
	}
	return OldAICommand(a, OAOP_CLEAR_TILE, tile, 0, 0, CMD_LANDSCAPE_CLEAR);
}

static OldAIWorkResult RemoveAttemptStation(OldAICompany *a, TileIndex base, byte axis)
{
	int bx = (int)TileX(base), by = (int)TileY(base);
	if (a->cleanup_cursor < 0) a->cleanup_cursor = 4;
	while (a->cleanup_cursor >= 0) {
		int i = a->cleanup_cursor;
		TileIndex tile = axis == 0 ? TileXY(bx + i, by) : TileXY(bx, by + i);
		if (a->pending_op == OAOP_CLEAR_TILE) {
			OldAIWorkResult wr = ClearAttemptTile(a, tile, "cleanup station err ");
			if (wr != OAI_WORK_DONE) return wr;
			a->cleanup_cursor--;
			continue;
		}
		if (!IsTileType(tile, MP_STATION)) {
			a->cleanup_cursor--;
			continue;
		}
		if (GetTileOwner(tile) != _current_company) {
			OL("cleanup station ownership changed");
			return OAI_WORK_FAILED;
		}
		OldAIWorkResult wr = ClearAttemptTile(a, tile, "cleanup station err ");
		if (wr != OAI_WORK_DONE) return wr;
		a->cleanup_cursor--;
	}
	a->cleanup_cursor = -1;
	return OAI_WORK_DONE;
}

/* Retry-safe cleanup of every infrastructure object owned by the current train
 * attempt.  Earthworks are deliberately retained: reversing individual LEVEL
 * commands on corner terrain could alter neighbouring industry/town property. */
static OldAIWorkResult CleanupTrainAttempt(CompanyID cid, OldAICompany *a)
{
	/* A vehicle is never created until every fallible infrastructure step has
	 * succeeded. Do not tear a depot out from under one if a future edit violates
	 * that invariant; the build state retains/retries the active attempt instead. */
	if (a->attempt_train_vehicle) {
		OL("train cleanup refused: attempt vehicle still in depot");
		return OAI_WORK_FAILED;
	}
	if (a->attempt_depot) {
		if (!IsTileType(a->tdepot, MP_RAILWAY) && a->pending_op != OAOP_CLEAR_TILE) {
			a->attempt_depot = false;
		} else {
			OldAIWorkResult wr = ClearAttemptTile(a, a->tdepot, "cleanup depot err ");
			if (wr != OAI_WORK_DONE) return wr;
			a->attempt_depot = false;
		}
	}
	if (a->attempt_spur) {
		byte spur = (a->staP_axis == 0) ? 0u : 1u;
		OldAIWorkResult wr = RemoveAttemptTrack(a, a->tdepot_front, spur, "cleanup spur err ");
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_spur = false;
	}
	if (a->attempt_line) {
		OldAIWorkResult wr = RemoveRailPlan(a, _oldai_plan[cid].rail,
				_oldai_rail_plan_count[cid]);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_line = false;
	}
	if (a->attempt_sta_a) {
		OldAIWorkResult wr = RemoveAttemptStation(a, a->staA_tile, a->staA_axis);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_sta_a = false;
	}
	if (a->attempt_sta_p) {
		OldAIWorkResult wr = RemoveAttemptStation(a, a->staP_tile, a->staP_axis);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_sta_p = false;
	}
	_oldai_rail_plan_count[cid] = 0;
	OL("train attempt cleanup complete");
	return OAI_WORK_DONE;
}

/* Credit a failed attempt through the synchronized command framework.  The
 * command's negative construction cost reverses both money and the expense
 * ledger on every peer. */
static OldAIWorkResult RefundFailedAttempt(CompanyID cid, OldAICompany *a)
{
	if (!a->attempt_costing) return OAI_WORK_DONE;
	if (a->pending_op == OAOP_REFUND) {
		uint64 encoded = ((uint64)a->pending_p2 << 32) | (uint64)a->pending_p1;
		OldAIWorkResult wr = OldAICommand(a, OAOP_REFUND, 0,
				a->pending_p1, a->pending_p2, CMD_OLDAI_REFUND);
		if (wr == OAI_WORK_DONE) {
			a->attempt_costing = false;
			OLn("refunded failed attempt /1000 = ",
					(uint)(int)((Money)encoded / 1000));
		}
		return wr;
	}

	Company *c = Company::GetIfValid(cid);
	if (c == NULL) return OAI_WORK_FAILED;
	Money spent = a->attempt_money0 - c->money;
	if (spent <= 0) {
		a->attempt_costing = false;
		return OAI_WORK_DONE;
	}
	uint64 encoded = (uint64)spent;
	OldAIWorkResult wr = OldAICommand(a, OAOP_REFUND, 0, (uint32)encoded,
			(uint32)(encoded >> 32), CMD_OLDAI_REFUND);
	if (wr == OAI_WORK_DONE) {
		a->attempt_costing = false;
		OLn("refunded failed attempt /1000 = ", (uint)(int)(spent / 1000));
	}
	return wr;
}

static void AbandonTrainAttempt(CompanyID cid, OldAICompany *a)
{
	a->tries = 0;
	a->town_skip++;
	if (a->plan_fail_streak < OLDAI_PLAN_MAX_FAIL_STREAK) {
		a->plan_fail_streak++;
	}
	uint gap = OldAIPlanningGap(a);
	a->next_plan_tick = _oldai_tick + gap;
	OLn("planning backoff ticks = ", gap);
	/* op_step is also used by station/depot/vehicle construction.  Cleanup has
	 * its own two phases: 0 removes attempt-owned objects, 1 waits for the
	 * synchronized refund. */
	a->op_step = 0;
	a->state = OAS_TCLEANUP;
	OldAIWorkResult wr = CleanupTrainAttempt(cid, a);
	if (wr == OAI_WORK_DONE) {
		a->op_step = 1;
		wr = RefundFailedAttempt(cid, a);
		if (wr == OAI_WORK_DONE) {
			a->op_step = 0;
			a->state = OAS_TPLAN;
		}
	}
}

/* Depot construction is one attempt.  Flags are set immediately after each
 * successful object build so OAS_TCLEANUP can remove it on any later failure. */
static OldAIWorkResult BuildProducerTrainDepot(OldAICompany *a)
{
	if (a->op_step == 0) {
		int delta = (int)TileHeight(a->tdepot_front) - (int)TileHeight(a->tdepot);
		uint32 level_p2 = (uint32)(uint8)(int8)delta;
		if (a->pending_op != OAOP_LEVEL_TRAIN_DEPOT) {
			CommandCost level = DoCommand(a->tdepot, a->tdepot, level_p2,
					DC_NONE, CMD_LEVEL_LAND);
			if (level.Failed()) {
				if (!OldAICommandAlreadySatisfied(CMD_LEVEL_LAND,
						level.GetErrorMessage())) {
					OLn("train depot level err ", (uint32)level.GetErrorMessage());
					return OAI_WORK_FAILED;
				}
				a->op_step = 1;
			}
		}
		if (a->op_step == 0) {
			OldAIWorkResult wr = OldAICommand(a, OAOP_LEVEL_TRAIN_DEPOT,
					a->tdepot, a->tdepot, level_p2, CMD_LEVEL_LAND);
			if (wr != OAI_WORK_DONE) return wr;
			a->op_step = 1;
		}
	}

	byte spur = (a->staP_axis == 0) ? 0u /* TRACK_X */ : 1u /* TRACK_Y */;
	if (a->op_step == 1) {
		if (a->pending_op != OAOP_BUILD_DEPOT_SPUR) {
			CommandCost track = DoCommand(a->tdepot_front, 0, spur,
					DC_NONE, CMD_BUILD_SINGLE_RAIL);
			if (track.Failed()) {
				OLn("train depot spur err ", (uint32)track.GetErrorMessage());
				return OAI_WORK_FAILED;
			}
		}
		OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_DEPOT_SPUR,
				a->tdepot_front, 0, spur, CMD_BUILD_SINGLE_RAIL);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_spur = true;
		a->op_step = 2;
	}

	uint entrance_dir = (TileX(a->tdepot) == TileX(a->tdepot_front))
			? (TileY(a->tdepot) < TileY(a->tdepot_front) ? 1 : 3)
			: (TileX(a->tdepot) < TileX(a->tdepot_front) ? 2 : 0);
	if (a->pending_op != OAOP_BUILD_TRAIN_DEPOT) {
		CommandCost test = DoCommand(a->tdepot, 0, entrance_dir,
				DC_NONE, CMD_BUILD_TRAIN_DEPOT);
		if (test.Failed()) {
			OLn("train depot err ", (uint32)test.GetErrorMessage());
			return OAI_WORK_FAILED;
		}
	}
	OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_TRAIN_DEPOT,
			a->tdepot, 0, entrance_dir, CMD_BUILD_TRAIN_DEPOT);
	if (wr != OAI_WORK_DONE) return wr;
	a->attempt_depot = true;
	a->op_step = 0;
	return OAI_WORK_DONE;
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
static bool SelectCargoPair(CompanyID cid, OldAICompany *a, int min_dist, int max_dist, int target_dist,
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
			if (FindNearestAccepter(a, p, cargo, min_dist, max_dist, target_dist) != NULL) feasible = true;
		}
		if (feasible) producer_count++;
	}
	if (producer_count == 0) return false;

	int producer_pick = (int)OldAIRandomRange(a, producer_count);
	int producer_index = 0;
	FOR_ALL_INDUSTRIES(p) {
		if (IndustryHasCompanyStation(p, cid)) continue;
		CargoID cargo_list[2];
		Industry *accept_list[2];
		int cargo_count = 0;
		for (int k = 0; k < 2; k++) {
			CargoID cargo = p->produced_cargo[k];
			if (cargo == CT_INVALID || p->last_month_production[k] == 0) continue;
			Industry *accept = FindNearestAccepter(a, p, cargo, min_dist, max_dist, target_dist);
			if (accept == NULL) continue;
			cargo_list[cargo_count] = cargo;
			accept_list[cargo_count] = accept;
			cargo_count++;
		}
		if (cargo_count == 0) continue;
		if (producer_index++ != producer_pick) continue;
		int pick = (int)OldAIRandomRange(a, cargo_count);
		*out_p = p; *out_a = accept_list[pick]; *out_c = cargo_list[pick];
		return true;
	}
	return false;
}

static bool SelectTownPair(OldAICompany *a, uint min_dist, uint max_dist, uint target_dist,
		const Town **out_a, const Town **out_b)
{
	int town_count = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) if (t->population >= 100) town_count++;
	if (town_count < 2) return false;
	int start = (int)OldAIRandomRange(a, town_count);
	for (int i = 0; i < town_count; i++) {
		const Town *from = FindTownForRoute(start + i);
		if (from == NULL) continue;
		const Town *to = FindPartnerTown(a, from, min_dist, max_dist, target_dist);
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
	/* Reject a station pair before the expensive A* unless the exact 2x8 corner
	 * rectangles represented by its virtual terrain overlay are terraformable. */
	if (!StationFootprintLevelable(a->staP_tile, a->staP_axis, a->route_p_h) ||
			!StationFootprintLevelable(a->staA_tile, a->staA_axis,
					a->route_a_h)) return false;
	TileIndex plat_p = PlatformAdj(a->staP_tile, a->staP_axis, a->staP_exit);
	TileIndex plat_a = PlatformAdj(a->staA_tile, a->staA_axis, a->staA_exit);
	byte pdir = DiagDirBetween(plat_p, a->staP_exit);
	byte adir = DiagDirBetween(plat_a, a->staA_exit);
	if (pdir == 0xFF || adir == 0xFF || cid >= MAX_COMPANIES) return false;

	_oldai_rail_plan_count[cid] = 0;
	if (!PlanRailRoute(a->staP_exit, (DiagDirection)pdir, a->route_p_h,
			a->staA_exit, (DiagDirection)adir, a->route_a_h,
			_oldai_plan[cid].rail, &_oldai_rail_plan_count[cid], OLDAI_MAX_RAIL_PLAN)) return false;
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
	if (!SelectCargoPair(cid, a, min_dist, max_dist, target_dist, &prod, &accept, &cargo)) return false;
	if (FindCargoWagon(cid, cargo) == INVALID_ENGINE) return false;
	ResetTrainAttempt(cid, a);
	a->route_kind = OARK_CARGO_TRAIN;
	a->tr_cargo = cargo;
	a->prodP_tile = prod->location.tile;
	a->prodA_tile = accept->location.tile;
	if (!FindIndustryStationSpot(a, a->prodP_tile, a->prodA_tile, &a->staP_tile, &a->staP_axis, &a->staP_exit)) return false;
	if (!FindIndustryStationSpot(a, a->prodA_tile, a->prodP_tile, &a->staA_tile, &a->staA_axis, &a->staA_exit)) return false;
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
	if (!SelectTownPair(a, min_dist, max_dist, target_dist, &ta, &tb)) return false;
	ResetTrainAttempt(cid, a);
	a->route_kind = OARK_PASSENGER_TRAIN;
	a->tr_cargo = passengers;
	a->prodP_tile = ta->xy;
	a->prodA_tile = tb->xy;
	if (!FindTownStationSpot(a, ta, tb->xy, &a->staP_tile, &a->staP_axis, &a->staP_exit)) return false;
	if (!FindTownStationSpot(a, tb, ta->xy, &a->staA_tile, &a->staA_axis, &a->staA_exit)) return false;
	if (!PrepareFreeRailPlan(cid, a)) return false;
	OLn("tplan: passenger cargo id = ", (uint)passengers);
	OLn("tplan: town distance = ", DistanceManhattan(ta->xy, tb->xy));
	return true;
}

static void ResetBusAttempt(CompanyID cid, OldAICompany *a)
{
	a->attempt_bus_stop_a = false;
	a->attempt_bus_stop_b = false;
	a->attempt_bus_depot = false;
	a->attempt_bus_road = false;
	a->attempt_bus_line = false;
	a->plan_cursor = 0;
	a->cleanup_cursor = -1;
	a->cleanup_phase = 0;
	a->op_step = 0;
	a->depot_connector_was_missing = false;
	if (cid < MAX_COMPANIES) _oldai_road_plan_count[cid] = 0;
}

static bool PrepareTownBus(CompanyID cid, OldAICompany *a)
{
	int town_count = 0;
	const Town *t;
	FOR_ALL_TOWNS(t) if (t->population >= 100) town_count++;
	/* One fixed queue is enough because another bus route is not selected until
	 * the completed route's fleet has left its depot. Train work may continue. */
	if (a->buses_waiting != 0 || town_count < 2 ||
			FindBusEngine(cid) == INVALID_ENGINE || cid >= MAX_COMPANIES) return false;
	int start = (int)OldAIRandomRange(a, town_count);
	for (int i = 0; i < town_count; i++) {
		const Town *ta = FindTownForRoute(start + i);
		if (ta == NULL) continue;
		/* Keep the proven partner selector and its RandomRange score term.  The
		 * 16..80 band is long enough to be genuinely inter-town while keeping a
		 * conservative 256-tile reconstructed-path cap useful on 68k. */
		const Town *tb = FindPartnerTown(a, ta, 16, 80, 40);
		if (tb == NULL) continue;
		if (!FindStopSpot(ta->xy, INVALID_TILE, 0, &a->stopA, &a->frontA)) continue;
		if (!FindStopSpot(tb->xy, INVALID_TILE, 0, &a->stopB, &a->frontB)) continue;
		if (a->stopA == a->stopB) continue;
		if (DistanceManhattan(ta->xy, a->stopA) > 20 ||
				DistanceManhattan(tb->xy, a->stopB) > 20) continue;
		if (!FindDepotSpot(a->frontA, a->stopA, a->stopB, &a->depot, &a->depot_front, &a->depot_dir)) continue;

		ResetBusAttempt(cid, a);
		if (!PlanRoadRoute(a->stopA, a->frontA, a->stopB, a->frontB, a->depot,
				_oldai_plan[cid].road, &_oldai_road_plan_count[cid],
				OLDAI_MAX_ROAD_PLAN)) continue;

		/* The stops/depot are still free command tests.  Costing starts only
		 * after the whole connecting road and every support object verifies. */
		CommandCost sa = TestBusStop(a->stopA, a->frontA);
		CommandCost sb = TestBusStop(a->stopB, a->frontB);
		CommandCost dp = DoCommand(a->depot, EntranceDir(a->depot, a->depot_front), 0, DC_NONE, CMD_BUILD_ROAD_DEPOT);
		if (sa.Failed() || sb.Failed() || dp.Failed()) {
			_oldai_road_plan_count[cid] = 0;
			continue;
		}
		RoadBits connector = DiagDirToRoadBits(a->depot_dir);
		if ((GetRoadBits(a->depot_front, ROADTYPE_ROAD) & connector) == 0) {
			CommandCost road = DoCommand(a->depot_front, connector | (ROADTYPE_ROAD << 4), 0, DC_NONE, CMD_BUILD_ROAD);
			if (road.Failed() && !OldAICommandAlreadySatisfied(
					CMD_BUILD_ROAD, road.GetErrorMessage())) {
				_oldai_road_plan_count[cid] = 0;
				continue;
			}
		}
		a->stopA_road = GetRoadBits(a->stopA, ROADTYPE_ROAD);
		a->stopB_road = GetRoadBits(a->stopB, ROADTYPE_ROAD);
		a->route_kind = OARK_TOWN_BUS;
		a->buses_on_route = 0;
		int road_tiles = RoadPlanTileLength(_oldai_plan[cid].road,
				_oldai_road_plan_count[cid]);
		a->bus_target_count = BusCountForRoadLength(road_tiles);
		a->tries = 0;
		BeginCostedAttempt(cid, a);
		a->state = OAS_BUILD_ROAD;
		OLn("tplan: free town-road plan steps = ", (uint)_oldai_road_plan_count[cid]);
		OLn("tplan: planned road tiles = ", (uint)road_tiles);
		OLn("tplan: buses required = ", (uint)a->bus_target_count);
		OLn("tplan: inter-town bus distance = ", DistanceManhattan(ta->xy, tb->xy));
		return true;
	}
	return false;
}

static OldAIWorkResult RemoveAttemptRoadBit(OldAICompany *a, TileIndex tile, RoadBits bit)
{
	if (a->pending_op == OAOP_REMOVE_BUS_CONNECTOR) {
		Axis axis = (bit & (ROAD_NE | ROAD_SW)) ? AXIS_X : AXIS_Y;
		uint32 p2 = 1u | ((uint32)axis << 2) | ((uint32)ROADTYPE_ROAD << 3);
		return OldAICommand(a, OAOP_REMOVE_BUS_CONNECTOR,
				tile, tile, p2, CMD_REMOVE_LONG_ROAD);
	}
	if (!IsNormalRoadTile(tile) || (GetRoadBits(tile, ROADTYPE_ROAD) & bit) == 0) {
		return OAI_WORK_DONE;
	}
	/* 1.0.5 has no single-bit road-remove command; CMD_REMOVE_LONG_ROAD removes one
	 * axis over a drag. Our connector is a SPUR perpendicular to the town road, so a
	 * 1-tile full drag along the spur's axis removes that axis, then cleanup restores
	 * the exact pre-connector bits. p1 = end tile (= start), p2 bit0 selects a
	 * full single-tile drag, bit2 = axis, bits3-4 = roadtype. */
	Axis axis = (bit & (ROAD_NE | ROAD_SW)) ? AXIS_X : AXIS_Y;
	uint32 p2 = 1u | ((uint32)axis << 2) | ((uint32)ROADTYPE_ROAD << 3);
	/* 1.0.5 marks CMD_REMOVE_LONG_ROAD CMD_NO_TEST: a connected town-road bit
	 * can be refused in test mode although its execute-mode removal is legal. */
	return OldAICommand(a, OAOP_REMOVE_BUS_CONNECTOR,
			tile, tile, p2, CMD_REMOVE_LONG_ROAD);
}

static OldAIWorkResult RestoreAttemptRoad(OldAICompany *a, TileIndex tile, RoadBits original)
{
	if (a->pending_op == OAOP_RESTORE_ROAD) {
		RoadBits present = IsNormalRoadTile(tile) ? GetRoadBits(tile, ROADTYPE_ROAD) : ROAD_NONE;
		RoadBits missing = (RoadBits)(original & ~present);
		return OldAICommand(a, OAOP_RESTORE_ROAD, tile,
				missing | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD);
	}
	RoadBits present = IsNormalRoadTile(tile) ? GetRoadBits(tile, ROADTYPE_ROAD) : ROAD_NONE;
	RoadBits missing = (RoadBits)(original & ~present);
	if (missing == ROAD_NONE) return OAI_WORK_DONE;
	CommandCost test = DoCommand(tile, missing | (ROADTYPE_ROAD << 4), 0,
			DC_NONE, CMD_BUILD_ROAD);
	if (test.Failed()) {
		if (OldAICommandAlreadySatisfied(CMD_BUILD_ROAD,
				test.GetErrorMessage())) return OAI_WORK_DONE;
		OLn("cleanup restore town road err ", (uint32)test.GetErrorMessage());
		return OAI_WORK_FAILED;
	}
	return OldAICommand(a, OAOP_RESTORE_ROAD, tile,
			missing | (ROADTYPE_ROAD << 4), 0, CMD_BUILD_ROAD);
}

static OldAIWorkResult CleanupBusStop(OldAICompany *a, TileIndex tile, RoadBits original)
{
	if (a->pending_op == OAOP_CLEAR_TILE) {
		OldAIWorkResult wr = ClearAttemptTile(a, tile, "cleanup bus stop err ");
		if (wr != OAI_WORK_DONE) return wr;
	}
	if (a->pending_op == OAOP_RESTORE_ROAD) {
		return RestoreAttemptRoad(a, tile, original);
	}
	if (IsTileType(tile, MP_STATION)) {
		OldAIWorkResult wr = ClearAttemptTile(a, tile, "cleanup bus stop err ");
		if (wr != OAI_WORK_DONE) return wr;
	}
	return RestoreAttemptRoad(a, tile, original);
}

static OldAIWorkResult CleanupBusAttempt(CompanyID cid, OldAICompany *a)
{
	/* A failed fleet build can leave several buses stopped in the depot. Sell
	 * every one before removing it; CmdSellRoadVeh requires this exact state. */
	while (a->buses_on_route != 0) {
		VehicleID id = a->dispatch_bus[0];
		Vehicle *v = Vehicle::GetIfValid(id);
		if (a->pending_op == OAOP_SELL_BUS ||
				(v != NULL && v->type == VEH_ROAD && v->owner == cid)) {
			if (a->pending_op != OAOP_SELL_BUS) {
			CommandCost test = DoCommand(0, id, 0, DC_NONE, GetCmdSellVeh(VEH_ROAD));
				if (test.Failed()) {
					OLn("cleanup bus vehicle err ", (uint32)test.GetErrorMessage());
					return OAI_WORK_FAILED;
				}
			}
			OldAIWorkResult wr = OldAICommand(a, OAOP_SELL_BUS,
					0, id, 0, GetCmdSellVeh(VEH_ROAD));
			if (wr != OAI_WORK_DONE) return wr;
		}
		for (int i = 1; i < a->buses_on_route; i++) {
			a->dispatch_bus[i - 1] = a->dispatch_bus[i];
		}
		a->buses_on_route--;
	}
	if (a->attempt_bus_depot) {
		if (!IsTileType(a->depot, MP_ROAD) && a->pending_op != OAOP_CLEAR_TILE) {
			a->attempt_bus_depot = false;
		} else {
			OldAIWorkResult wr = ClearAttemptTile(a, a->depot, "cleanup bus depot err ");
			if (wr != OAI_WORK_DONE) return wr;
			a->attempt_bus_depot = false;
		}
	}
	if (a->attempt_bus_road) {
		if (a->cleanup_phase == 0) {
			OldAIWorkResult wr = RemoveAttemptRoadBit(a, a->depot_front,
					DiagDirToRoadBits(a->depot_dir));
			if (wr != OAI_WORK_DONE) return wr;
			a->cleanup_phase = 1;
		}
		OldAIWorkResult wr = RestoreAttemptRoad(a, a->depot_front,
				(RoadBits)a->depot_front_road);
		if (wr != OAI_WORK_DONE) return wr;
		a->cleanup_phase = 0;
		a->attempt_bus_road = false;
	}
	if (a->attempt_bus_stop_b) {
		OldAIWorkResult wr = CleanupBusStop(a, a->stopB, a->stopB_road);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_bus_stop_b = false;
	}
	if (a->attempt_bus_stop_a) {
		OldAIWorkResult wr = CleanupBusStop(a, a->stopA, a->stopA_road);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_bus_stop_a = false;
	}
	if (a->attempt_bus_line) {
		if (cid >= MAX_COMPANIES) return OAI_WORK_FAILED;
		OldAIWorkResult wr = RemoveRoadPlan(a, _oldai_plan[cid].road,
				_oldai_road_plan_count[cid]);
		if (wr != OAI_WORK_DONE) return wr;
		a->attempt_bus_line = false;
		_oldai_road_plan_count[cid] = 0;
	}
	return OAI_WORK_DONE;
}

static void AbandonBusAttempt(CompanyID cid, OldAICompany *a)
{
	a->tries = 0;
	a->town_skip++;
	a->op_step = 0;
	a->state = OAS_BCLEANUP;
	OldAIWorkResult wr = CleanupBusAttempt(cid, a);
	if (wr == OAI_WORK_DONE) {
		a->op_step = 1;
		wr = RefundFailedAttempt(cid, a);
		if (wr == OAI_WORK_DONE) {
			a->op_step = 0;
			a->state = OAS_TPLAN;
		}
	}
}

/* This is independent of the route-building state machine: completed routes
 * drain one stopped bus at a time while the company plans and builds elsewhere. */
static void DispatchQueuedBus(CompanyID cid, OldAICompany *a)
{
	if (a->buses_waiting == 0 || _oldai_tick < a->next_bus_release_tick) return;
	if (a->pending_op != OAOP_NONE && a->pending_op != OAOP_BUS_DISPATCH) return;
	int index = (int)a->bus_target_count - (int)a->buses_waiting;
	if (index < 0 || index >= a->bus_target_count ||
			index >= OLDAI_BUS_MAX_COUNT) {
		OL("bus dispatch queue corrupt; dropping queue");
		a->buses_waiting = 0;
		return;
	}

	VehicleID id = a->dispatch_bus[index];
	if (a->pending_op == OAOP_BUS_DISPATCH) {
		OldAIWorkResult wr = OldAICommand(a, OAOP_BUS_DISPATCH,
				a->pending_tile, a->pending_p1, a->pending_p2, a->pending_cmd);
		if (wr == OAI_WORK_WAIT) return;
		if (wr == OAI_WORK_FAILED) {
			if ((_oldai_tick & 255) == 0) OL("queued bus start failed; will retry");
			return;
		}
	}
	Vehicle *v = Vehicle::GetIfValid(id);
	if (v == NULL || v->type != VEH_ROAD || v->owner != cid) {
		OL("queued bus disappeared; advancing dispatch queue");
		a->buses_waiting--;
		a->next_bus_release_tick = _oldai_tick + OLDAI_BUS_DISPATCH_INTERVAL;
		return;
	}
	/* A prior command may have succeeded despite a lost return path. Never
	 * toggle an already-running vehicle back to stopped on retry. */
	if ((v->vehstatus & VS_STOPPED) != 0) {
		OldAIWorkResult wr = OldAICommand(a, OAOP_BUS_DISPATCH,
				0, id, 0, CMD_START_STOP_VEHICLE);
		if (wr == OAI_WORK_WAIT) return;
		if (wr == OAI_WORK_FAILED) {
			if ((_oldai_tick & 255) == 0) OL("queued bus start failed; will retry");
			return;
		}
	}
	a->buses_waiting--;
	a->next_bus_release_tick = _oldai_tick + OLDAI_BUS_DISPATCH_INTERVAL;
	OLn("bus dispatched; still waiting = ", (uint)a->buses_waiting);
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
			OL("laying saved free-trial town-to-town road");
			/* Mark ownership before execution.  The committed-step flags and
			 * saved cursor let OAS_BCLEANUP remove any interrupted prefix. */
			a->attempt_bus_line = true;
			if (cid >= MAX_COMPANIES || _oldai_road_plan_count[cid] <= 0) {
				AbandonBusAttempt(cid, a);
				break;
			}
			{
				OldAIWorkResult wr = ExecuteRoadPlan(a, _oldai_plan[cid].road,
						_oldai_road_plan_count[cid]);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_DONE) {
				OL("town-to-town road built");
				a->tries = 0;
				a->state = OAS_BUILD_STOP_A;
				} else {
				OL("town-to-town road failed; cleaning attempt");
				AbandonBusAttempt(cid, a);
				}
			}
			break;

		case OAS_BUILD_STOP_A:
			OL("building bus stop A");
			{
				OldAIWorkResult wr = BuildBusStop(a, OAOP_BUILD_BUS_STOP_A,
						a->stopA, a->frontA);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_DONE) {
				a->staA = GetStationIndex(a->stopA);
				a->attempt_bus_stop_a = true;
				OL("stop A built");
				a->state = OAS_BUILD_STOP_B;
				} else if (++a->tries > 1) {
					OL("stop A failed; cleaning attempt");
					AbandonBusAttempt(cid, a);
				}
			}
			break;

		case OAS_BUILD_STOP_B:
			OL("building bus stop B");
			{
				OldAIWorkResult wr = BuildBusStop(a, OAOP_BUILD_BUS_STOP_B,
						a->stopB, a->frontB);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_DONE) {
				a->staB = GetStationIndex(a->stopB);
				a->attempt_bus_stop_b = true;
				OL("stop B built");
				a->state = OAS_BUILD_DEPOT;
				} else if (++a->tries > 1) {
					OL("stop B failed; cleaning attempt");
					AbandonBusAttempt(cid, a);
				}
			}
			break;

		case OAS_BUILD_DEPOT: {
			OL("building road depot");
			uint32 entrance = EntranceDir(a->depot, a->depot_front) | (0 << 2);
			if (!a->attempt_bus_depot) {
				if (a->pending_op != OAOP_BUILD_BUS_DEPOT) {
					CommandCost test = DoCommand(a->depot, entrance, 0,
							DC_NONE, CMD_BUILD_ROAD_DEPOT);
					if (test.Failed()) {
						OLn("depot err", (uint32)test.GetErrorMessage());
						if (++a->tries > 1) {
							OL("depot failed; cleaning attempt");
							AbandonBusAttempt(cid, a);
						}
						break;
					}
				}
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_BUS_DEPOT,
						a->depot, entrance, 0, CMD_BUILD_ROAD_DEPOT);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if (++a->tries > 1) {
						OL("depot failed; cleaning attempt");
						AbandonBusAttempt(cid, a);
					}
					break;
				}
				a->attempt_bus_depot = true;
				OL("depot built");
				a->depot_front_road = (byte)GetRoadBits(a->depot_front, ROADTYPE_ROAD);
				RoadBits connector = DiagDirToRoadBits(a->depot_dir);
				a->depot_connector_was_missing =
						(GetRoadBits(a->depot_front, ROADTYPE_ROAD) & connector) == 0;
			}

			RoadBits connector = DiagDirToRoadBits(a->depot_dir);
			if (a->depot_connector_was_missing && !a->attempt_bus_road) {
				if (a->pending_op != OAOP_BUILD_BUS_CONNECTOR) {
					CommandCost test = DoCommand(a->depot_front,
							connector | (ROADTYPE_ROAD << 4), 0,
							DC_NONE, CMD_BUILD_ROAD);
					if (test.Failed() && !OldAICommandAlreadySatisfied(
							CMD_BUILD_ROAD, test.GetErrorMessage())) {
						OLn("connect err", (uint32)test.GetErrorMessage());
						OL("depot connector failed; cleaning attempt");
						AbandonBusAttempt(cid, a);
						break;
					}
					if (test.Failed()) a->depot_connector_was_missing = false;
				}
				if (a->depot_connector_was_missing) {
					OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_BUS_CONNECTOR,
							a->depot_front, connector | (ROADTYPE_ROAD << 4),
							0, CMD_BUILD_ROAD);
					if (wr == OAI_WORK_WAIT) break;
					if (wr == OAI_WORK_FAILED) {
						OL("depot connector failed; cleaning attempt");
						AbandonBusAttempt(cid, a);
						break;
					}
					a->attempt_bus_road = true;
				}
			}
			OL("depot connected to road");
			a->state = OAS_BUILD_BUS;
			break;
		}

		case OAS_BUILD_BUS: {
			bool built_this_call = false;
bus_orders:
			/* Finish the orders for a bus whose build callback has already
			 * supplied its exact id before attempting another vehicle. */
			if (a->op_step == 1) {
				VehicleID bus = a->dispatch_bus[a->buses_on_route - 1];
				Order oa; oa.MakeGoToStation(a->staA); oa.SetStopLocation(OSL_PLATFORM_FAR_END); oa.SetNonStopType(ONSF_STOP_EVERYWHERE);
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUS_ORDER_A, 0,
						bus | (0 << 16), oa.Pack(), CMD_INSERT_ORDER);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if (++a->tries > 8) AbandonBusAttempt(cid, a);
					break;
				}
				a->op_step = 2;
			}
			if (a->op_step == 2) {
				VehicleID bus = a->dispatch_bus[a->buses_on_route - 1];
				Order ob; ob.MakeGoToStation(a->staB); ob.SetStopLocation(OSL_PLATFORM_FAR_END); ob.SetNonStopType(ONSF_STOP_EVERYWHERE);
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUS_ORDER_B, 0,
						bus | (1 << 16), ob.Pack(), CMD_INSERT_ORDER);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if (++a->tries > 8) AbandonBusAttempt(cid, a);
					break;
				}
				a->op_step = 0;
				a->tries = 0;
			}
			if (built_this_call && a->buses_on_route < a->bus_target_count) break;
			if (a->op_step == 0 && a->buses_on_route < a->bus_target_count) {
				EngineID e = (a->pending_op == OAOP_BUILD_BUS)
						? (EngineID)a->pending_p1 : FindBusEngine(cid);
				if (e == INVALID_ENGINE) {
					/* Before a vehicle exists this is a normal refundable failure.
					 * Once a bus exists, keep its depot and wait rather than orphaning
					 * a vehicle by tearing the route out from under it. */
					if (a->buses_on_route == 0) {
						OL("bus engine disappeared; cleaning attempt");
						AbandonBusAttempt(cid, a);
					} else if ((++a->tries & 7) == 1) {
						OL("fleet bus engine unavailable; waiting");
					}
					if (a->buses_on_route != 0 && a->tries > 8) {
						OL("fleet bus engine stayed unavailable; cleaning attempt");
						AbandonBusAttempt(cid, a);
					}
					break;
				}
				VehicleID new_bus = INVALID_VEHICLE;
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_BUS,
						a->depot, e, 0, GetCmdBuildVeh(VEH_ROAD), &new_bus);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if ((++a->tries & 7) == 1) OL("fleet bus build failed; retrying without duplication");
					if (a->tries > 8) {
						OL("fleet bus build stayed failed; cleaning attempt");
						AbandonBusAttempt(cid, a);
					}
					break;
				}
				a->dispatch_bus[a->buses_on_route++] = new_bus;
				a->op_step = 1;
				built_this_call = true;
				OLn("fleet buses built = ", (uint)a->buses_on_route);
				goto bus_orders;
			}
			if (a->op_step != 0 || a->buses_on_route < a->bus_target_count) break;

			/* The fleet is complete and still stopped. Queue it, release the first
			 * bus now, and let later ticks drain the rest without blocking work. */
			a->buses_waiting = a->bus_target_count;
			a->next_bus_release_tick = _oldai_tick;
			a->routes_done++;
			a->attempt_costing = false;
			a->plan_fail_streak = 0;
			a->next_plan_tick = _oldai_tick + OLDAI_PLAN_MIN_GAP;
			ResetBusAttempt(cid, a); /* completed objects are no longer attempt-owned */
			DispatchQueuedBus(cid, a);
			OLn("BUS ROUTE COMPLETE, total routes = ", a->routes_done);
			uint8 speed = OldAICompetitorSpeed();
			a->cooldown_until = _oldai_tick + ((uint)8192 << (4 - speed));
			a->state = OAS_TPLAN;
			break;
		}

		case OAS_BCLEANUP:
			{
				OldAIWorkResult wr = OAI_WORK_DONE;
				if (a->op_step == 0) {
					wr = CleanupBusAttempt(cid, a);
					if (wr == OAI_WORK_DONE) a->op_step = 1;
				}
				if (wr == OAI_WORK_DONE && a->op_step == 1) {
					wr = RefundFailedAttempt(cid, a);
				}
				if (wr == OAI_WORK_DONE) {
					a->tries = 0;
					a->op_step = 0;
					a->state = OAS_TPLAN;
				} else if (wr == OAI_WORK_FAILED) {
					a->tries++;
					if ((a->tries & 7) == 1) OL("bus cleanup incomplete; will retry");
				}
			}
			break;

		/* ----------------------------------------------------------------- *
		 *  TRAIN route state machine (one action per tick).                  *
		 * ----------------------------------------------------------------- */
		case OAS_TPLAN: {
			const Company *co = Company::GetIfValid(cid);
			bool loan_action_done = false;
			if (a->pending_op == OAOP_DECREASE_LOAN ||
					a->pending_op == OAOP_INCREASE_LOAN) {
				OldAIPendingOp op = a->pending_op;
				OldAIWorkResult wr = OldAICommand(a, op, a->pending_tile,
						a->pending_p1, a->pending_p2, a->pending_cmd);
				if (wr == OAI_WORK_WAIT) break;
				loan_action_done = true;
				co = Company::GetIfValid(cid);
			}
			/* Manage the loan by net cash position (money - current_loan):
			 *  - money > 1.5x loan  -> repay the loan in full; a low/zero loan lifts
			 *    the company performance rating a lot.
			 *  - money <  loan ("na minusie", net negative) -> draw the loan to the
			 *    ceiling so there is working capital to build with.
			 *  - in between (solvent but not flush) -> leave the loan as is; do not
			 *    borrow more when already in the black.
			 * Re-fetch the company after any change - money just moved. */
			if (co != NULL && !loan_action_done) {
				Money money = co->money;
				Money loan  = co->current_loan;
				if (loan > 0 && money > loan + loan / 2) {
					OldAIWorkResult wr = OldAICommand(a, OAOP_DECREASE_LOAN,
							0, 0, 1, CMD_DECREASE_LOAN);
					if (wr == OAI_WORK_WAIT) break;
					co = Company::GetIfValid(cid);
				} else if (money < loan && loan < _economy.max_loan) {
					Money delta = _economy.max_loan - loan;
					delta -= delta % LOAN_INTERVAL;
					if (delta > 0) {
						OldAIWorkResult wr = OldAICommand(a, OAOP_INCREASE_LOAN,
								0, (uint32)delta, 2, CMD_INCREASE_LOAN);
						if (wr == OAI_WORK_WAIT) break;
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
			/* Keep the full A* node budgets, but space server-side planning attempts
			 * by 128,256,...4096 ticks after consecutive abandoned train routes.
			 * A completed route resets this to 128. */
			if (_oldai_tick < a->next_plan_tick) break;
			/* SPEED FIX: one random route-type per attempt (not a loop over all
			 * types), so a failed attempt costs ONE heavy A* search, not ~6. That
			 * makes each attempt ~6x cheaper, so we can retry ~4x more often: 128
			 * ticks instead of 512. Net: first route in ~1 game-month (was ~2.5
			 * years when this looped all types at 512). */
			a->next_plan_tick = _oldai_tick + OldAIPlanningGap(a);
			/* Short cargo is always candidate zero. Richer tiers append long cargo,
			 * three passenger-train bands, then free-planned town-to-town buses.
			 * Start at a random candidate and wrap through the others, so unavailable
			 * or unbuildable choices fall through without grinding one pair forever. */
			enum PlanChoice { PC_CARGO_SHORT, PC_CARGO_LONG, PC_PASS_SHORT, PC_PASS_2X, PC_PASS_3X, PC_TOWN_BUS };
			PlanChoice choices[6];
			int choice_count = 0;
			choices[choice_count++] = PC_CARGO_SHORT;
			if (co->money >= 100000) choices[choice_count++] = PC_CARGO_LONG;
			if (co->money >= 120000) choices[choice_count++] = PC_PASS_SHORT;
			if (co->money >= 150000) choices[choice_count++] = PC_PASS_2X;
			if (co->money >= 200000) choices[choice_count++] = PC_PASS_3X;
			if (co->money >= 300000) choices[choice_count++] = PC_TOWN_BUS;

			/* SPEED FIX: pick ONE random type this attempt, do not loop. If it is
			 * not buildable we retry after the current adaptive gap with a fresh
			 * random pick, which is far cheaper than grinding all types. */
			int first = (int)OldAIRandomRange(a, choice_count);
			bool prepared = false;
			switch (choices[first]) {
				case PC_CARGO_SHORT: prepared = PrepareCargoTrain(cid, a, 24, 64, 40); break;
				case PC_CARGO_LONG:  prepared = PrepareCargoTrain(cid, a, 48, 128, 80); break;
				case PC_PASS_SHORT:  prepared = PreparePassengerTrain(cid, a, 20, 60, 40); break;
				case PC_PASS_2X:     prepared = PreparePassengerTrain(cid, a, 40, 120, 80); break;
				case PC_PASS_3X:     prepared = PreparePassengerTrain(cid, a, 60, 180, 120); break;
				case PC_TOWN_BUS:    a->state = OAS_PLAN; prepared = true; break;
			}
			if (!prepared) {
				a->town_skip++;
				if ((_oldai_tick & 1023) == 0) OL("tplan: no buildable unlocked choice this pass");
			}
			break;
		}

		case OAS_TBUILD_STA_A: {
			OL(a->route_kind == OARK_PASSENGER_TRAIN ? "building first town rail station" : "building producer rail station");
			if (a->op_step == 0) {
				OldAIWorkResult level = LevelStationFootprint(a, OAOP_LEVEL_STATION_P,
						a->staP_tile, a->staP_axis, a->route_p_h);
				if (level == OAI_WORK_WAIT) break;
				if (level == OAI_WORK_FAILED) {
					OL("producer station terrain changed; next pair");
					AbandonTrainAttempt(cid, a); break;
				}
				/* Do not preflight/execute the area-level operation again while
				 * station construction is pending or being retried. */
				a->op_step = 1;
			}
			/* railtype0 | axis(bit4) | numtracks 1 (bits8..) | plat_len 5 (bits16..) | adjacent (bit24) */
			uint32 p1 = 0u | ((a->staP_axis == 1) ? (1u << 4) : 0u) | (1u << 8) | (5u << 16) | (1u << 24);
			uint32 p2 = ((uint32)INVALID_STATION) << 16;
			if (a->pending_op != OAOP_BUILD_STATION_P) {
				CommandCost test = DoCommand(a->staP_tile, p1, p2,
						DC_NONE, CMD_BUILD_RAIL_STATION);
				if (test.Failed()) {
					OLn("producer sta err", (uint32)test.GetErrorMessage());
					if (OldAIStationTerrainError(test.GetErrorMessage())) {
						/* v0.9.4 re-entered footprint levelling on every station
						 * retry.  Preserve that recovery without duplicating a
						 * pending command. */
						a->op_step = 0;
					}
					if (++a->tries > 1) {
						OL("producer sta failed; cleaning attempt");
						AbandonTrainAttempt(cid, a);
					}
					break;
				}
			}
			StringID error;
			OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_STATION_P,
					a->staP_tile, p1, p2, CMD_BUILD_RAIL_STATION,
					NULL, &error);
			if (wr == OAI_WORK_WAIT) break;
			if (wr == OAI_WORK_DONE) {
				a->trStaP = GetStationIndex(a->staP_tile);
				a->attempt_sta_p = true;
				OL("producer station built");
				a->op_step = 0;
				a->state = OAS_TBUILD_STA_B;
			} else {
				if (OldAIStationTerrainError(error)) a->op_step = 0;
				if (++a->tries > 1) {
					OL("producer sta failed; cleaning attempt");
					AbandonTrainAttempt(cid, a);
				}
			}
			break;
		}

		case OAS_TBUILD_STA_B: {
			OL(a->route_kind == OARK_PASSENGER_TRAIN ? "building second town rail station" : "building accepter rail station");
			if (a->op_step == 0) {
				OldAIWorkResult level = LevelStationFootprint(a, OAOP_LEVEL_STATION_A,
						a->staA_tile, a->staA_axis, a->route_a_h);
				if (level == OAI_WORK_WAIT) break;
				if (level == OAI_WORK_FAILED) {
					OL("accepter station terrain changed; next pair");
					AbandonTrainAttempt(cid, a); break;
				}
				a->op_step = 1;
			}
			uint32 p1 = 0u | ((a->staA_axis == 1) ? (1u << 4) : 0u) | (1u << 8) | (5u << 16) | (1u << 24);
			uint32 p2 = ((uint32)INVALID_STATION) << 16;
			if (a->pending_op != OAOP_BUILD_STATION_A) {
				CommandCost test = DoCommand(a->staA_tile, p1, p2,
						DC_NONE, CMD_BUILD_RAIL_STATION);
				if (test.Failed()) {
					OLn("accepter sta err", (uint32)test.GetErrorMessage());
					if (OldAIStationTerrainError(test.GetErrorMessage())) {
						a->op_step = 0;
					}
					if (++a->tries > 1) {
						OL("accepter sta failed; cleaning attempt");
						AbandonTrainAttempt(cid, a);
					}
					break;
				}
			}
			StringID error;
			OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_STATION_A,
					a->staA_tile, p1, p2, CMD_BUILD_RAIL_STATION,
					NULL, &error);
			if (wr == OAI_WORK_WAIT) break;
			if (wr == OAI_WORK_DONE) {
				a->trStaA = GetStationIndex(a->staA_tile);
				a->attempt_sta_a = true;
				OL("accepter station built");
				a->op_step = 0;
				a->state = OAS_TBUILD_RAIL;
			} else {
				if (OldAIStationTerrainError(error)) a->op_step = 0;
				if (++a->tries > 1) {
					OL("accepter sta failed; cleaning attempt");
					AbandonTrainAttempt(cid, a);
				}
			}
			break;
		}

		case OAS_TBUILD_RAIL:
			OL(a->route_kind == OARK_PASSENGER_TRAIN ? "laying saved free-trial passenger line" : "laying saved free-trial cargo main line");
			{
				OldAIWorkResult wr = BuildRailLine(cid, a);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_DONE) {
				OL("main line laid");
				a->tries = 0;
				a->state = OAS_TBUILD_DEPOT;
				} else {
				OL("main line failed; cleaning attempt");
				AbandonTrainAttempt(cid, a);
				}
			}
			break;

		case OAS_TBUILD_DEPOT: {
			OL("building in-line depot at producer outer end");
			OldAIWorkResult wr = BuildProducerTrainDepot(a);
			if (wr == OAI_WORK_WAIT) break;
			if (wr == OAI_WORK_DONE) {
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
			VehicleID locoid = a->train;

			/* Route completion is also command-confirmed: both orders and the
			 * start command must execute before attempt ownership is released. */
train_orders:
			locoid = a->train;
			if (a->op_step >= 10) {
				if (a->op_step == 10) {
					Order op; op.MakeGoToStation(a->trStaP); op.SetLoadType(OLFB_FULL_LOAD); op.SetNonStopType(ONSF_STOP_EVERYWHERE);
					OldAIWorkResult wr = OldAICommand(a, OAOP_TRAIN_ORDER_P, 0,
							locoid | (0 << 16), op.Pack(), CMD_INSERT_ORDER);
					if (wr == OAI_WORK_WAIT) break;
					if (wr == OAI_WORK_FAILED) {
						if ((++a->tries & 7) == 1) OL("producer order failed; retrying");
						break;
					}
					a->op_step = 11;
				}
				if (a->op_step == 11) {
					Order od; od.MakeGoToStation(a->trStaA); od.SetNonStopType(ONSF_STOP_EVERYWHERE);
					OldAIWorkResult wr = OldAICommand(a, OAOP_TRAIN_ORDER_A, 0,
							locoid | (1 << 16), od.Pack(), CMD_INSERT_ORDER);
					if (wr == OAI_WORK_WAIT) break;
					if (wr == OAI_WORK_FAILED) {
						if ((++a->tries & 7) == 1) OL("delivery order failed; retrying");
						break;
					}
					a->op_step = 12;
				}
				OldAIWorkResult wr = OldAICommand(a, OAOP_TRAIN_START, 0,
						locoid, 0, CMD_START_STOP_VEHICLE);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if ((++a->tries & 7) == 1) OL("train start failed; retrying");
					break;
				}

				a->routes_done++;
				a->tries = 0;
				a->attempt_costing = false;
				a->plan_fail_streak = 0;
				a->next_plan_tick = _oldai_tick + OLDAI_PLAN_MIN_GAP;
				OLn(a->route_kind == OARK_PASSENGER_TRAIN ? "PASSENGER TRAIN ROUTE COMPLETE, total = " : "CARGO TRAIN ROUTE COMPLETE, total = ", a->routes_done);
				uint8 speed = OldAICompetitorSpeed();
				a->cooldown_until = _oldai_tick + ((uint)8192 << (4 - speed));
				ResetTrainAttempt(cid, a);
				a->state = OAS_TPLAN;
				break;
			}

			/* Build the loco on one tick and capture its id only in CcOldAI. */
			if (!a->attempt_train_vehicle) {
				EngineID loco = (a->pending_op == OAOP_BUILD_LOCO)
						? (EngineID)a->pending_p1 : FindTrainLoco(cid);
				if (loco == INVALID_ENGINE) { OL("no buildable loco yet; waiting"); break; }
				VehicleID new_loco = INVALID_VEHICLE;
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_LOCO,
						a->tdepot, loco, 0, GetCmdBuildVeh(VEH_TRAIN), &new_loco);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if (++a->tries > 8) {
						OL("loco build failed; cleaning attempt");
						AbandonTrainAttempt(cid, a);
					}
					break;
				}
				a->train = new_loco;
				a->attempt_train_vehicle = true;
				a->tries = 0;
				OL(a->route_kind == OARK_PASSENGER_TRAIN ? "passenger loco built" : "cargo loco built");
				break;
			}
			locoid = a->train;

			EngineID wag = (a->pending_op == OAOP_BUILD_WAGON)
					? (EngineID)a->pending_p1 : FindCargoWagon(cid, a->tr_cargo);
			if (wag == INVALID_ENGINE && a->pending_op != OAOP_MOVE_WAGON) {
				OL("required carriage unavailable; loco held in depot, waiting");
				break;
			}

			if (a->attempt_loose_wagon) {
				OldAIWorkResult wr = OldAICommand(a, OAOP_MOVE_WAGON, 0,
						a->loose_wagon | (locoid << 16), 0, CMD_MOVE_RAIL_VEHICLE);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if ((++a->tries & 7) == 1) OL("carriage move failed; retrying without duplication");
					break;
				}
				a->attempt_loose_wagon = false;
				a->attempt_carriages++;
				a->tries = 0;
				OLn("carriage attached, count = ", (uint)a->attempt_carriages);
			}
			if (a->attempt_carriages == 0) {
				VehicleID new_wagon = INVALID_VEHICLE;
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_WAGON,
						a->tdepot, wag, 0, GetCmdBuildVeh(VEH_TRAIN), &new_wagon);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					if ((++a->tries & 7) == 1) OL("required first carriage build failed; waiting");
					break;
				}
				a->loose_wagon = new_wagon;
				a->attempt_loose_wagon = true;
				break; /* attach by stored id next tick */
			}
			while (a->attempt_carriages < 5) {
				VehicleID new_wagon = INVALID_VEHICLE;
				OldAIWorkResult wr = OldAICommand(a, OAOP_BUILD_WAGON,
						a->tdepot, wag, 0, GetCmdBuildVeh(VEH_TRAIN), &new_wagon);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					OL("optional carriage build failed; consist is sufficient");
					break;
				}
				a->loose_wagon = new_wagon;
				a->attempt_loose_wagon = true;
				wr = OldAICommand(a, OAOP_MOVE_WAGON, 0,
						a->loose_wagon | (locoid << 16), 0, CMD_MOVE_RAIL_VEHICLE);
				if (wr == OAI_WORK_WAIT) break;
				if (wr == OAI_WORK_FAILED) {
					OL("optional carriage move delayed; retrying next tick");
					break;
				}
				a->attempt_loose_wagon = false;
				a->attempt_carriages++;
				OLn("carriage attached, count = ", (uint)a->attempt_carriages);
			}
			if (a->pending_op == OAOP_BUILD_WAGON ||
					a->pending_op == OAOP_MOVE_WAGON) break;
			if (a->attempt_loose_wagon) break;

			/* FULL LOAD at producer; default unload at accepter so delivery pays. */
			a->op_step = 10;
			goto train_orders;
		}

		case OAS_TCLEANUP:
			{
				OldAIWorkResult wr = OAI_WORK_DONE;
				if (a->op_step == 0) {
					wr = CleanupTrainAttempt(cid, a);
					if (wr == OAI_WORK_DONE) a->op_step = 1;
				}
				if (wr == OAI_WORK_DONE && a->op_step == 1) {
					wr = RefundFailedAttempt(cid, a);
				}
				if (wr == OAI_WORK_DONE) {
					a->tries = 0;
					a->op_step = 0;
					a->state = OAS_TPLAN;
				} else if (wr == OAI_WORK_FAILED) {
					a->tries++;
					if ((a->tries & 7) == 1) OL("train cleanup incomplete; will retry");
				}
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
	/* Mirror stock AI::GameLoop (ai_core.cpp): in networking only the server
	 * ticks the AI, and only when AIs are allowed in multiplayer. Without this
	 * every client would lazily adopt the AI company (OldAI_Start below) and
	 * post its own DoCommandP packets -> duplicate/rejected commands = desync. */
#ifdef ENABLE_NETWORK
	if (_networking && (!_network_server || !_settings_game.ai.ai_in_multiplayer)) return;
#endif

	_oldai_tick++;
	uint8 speed = OldAICompetitorSpeed();
	bool run_company = (_oldai_tick & ((1u << (4 - speed)) - 1)) == 0;

	CompanyByte old_company = _current_company;
	const Company *c;
	FOR_ALL_COMPANIES(c) {
		if (!c->is_ai) continue;
		CompanyID cid = c->index;
		/* Saves made before the OLAI chunk have no native state.  The stock AI
		 * loader leaves a native company's ai_instance NULL; lazily seed a
		 * clean deterministic state instead of leaving that company inert. */
		if (!_oldai[cid].active) {
			if (c->ai_instance != NULL) continue;
			OldAI_Start(cid);
		}
		_current_company = cid;
		DispatchQueuedBus(cid, &_oldai[cid]);
		if (run_company) {
			_oldai[cid].age++;
			RunCompany(cid);
		}
	}
	_current_company = old_company;
}
