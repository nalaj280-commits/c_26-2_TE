#include "game.h"
#include "tactics.h"
#include <windows.h>

/* Public-state model of an exit camper: attack opportunities, establish the
   post, claim a nearby loose key and finally escape with it. */
static Command p5_predict_attacker(const Snapshot *s)
{
    const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    Position me = s->players[s->self].pos, goal = s->exit;
    Position queue[MAP_MAX * MAP_MAX];
    int first[MAP_MAX][MAP_MAX], blocked[MAP_MAX][MAP_MAX] = {0};
    int d, i, x, y, head = 0, tail = 0, best = 0, best_score = 0;
    if (s->key_owner == s->self)
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == s->exit.x && me.y + dy[d] == s->exit.y) return (Command)d;
    for (d = 0; d < 4; ++d) {
        int score = 0;
        for (i = 0; i < PLAYER_COUNT; ++i) {
            int px = s->players[i].pos.x - me.x, py = s->players[i].pos.y - me.y;
            if (i == s->self || s->players[i].hp <= 0) continue;
            if ((d < 2 && py == dy[d] && abs(px) <= 1) ||
                (d >= 2 && px == dx[d] && abs(py) <= 1))
                score += i == s->key_owner ? 100 : s->players[i].hp == 1 ? 20 : 10;
        }
        if (score > best_score) { best_score = score; best = d; }
    }
    if (best_score) return (Command)(COMMAND_ATTACK_UP + best);
    if (s->key_owner < 0 && (abs(me.x - goal.x) + abs(me.y - goal.y) == 1 ||
        abs(s->key.x - goal.x) + abs(s->key.y - goal.y) <= 1)) {
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == s->key.x && me.y + dy[d] == s->key.y)
                return (Command)(COMMAND_PICK_UP + d);
    }
    else if (s->key_owner != s->self) {
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == goal.x && me.y + dy[d] == goal.y)
                return (Command)(COMMAND_ATTACK_UP + d);
    }
    if (s->key_owner < 0 && abs(s->key.x - s->exit.x) + abs(s->key.y - s->exit.y) <= 1 &&
        abs(me.x - s->exit.x) + abs(me.y - s->exit.y) <= 2) goal = s->key;
    for (y = 0; y < s->size; ++y)
        for (x = 0; x < s->size; ++x) first[y][x] = -1;
    for (i = 0; i < PLAYER_COUNT; ++i)
        if (i != s->self && s->players[i].hp > 0)
            blocked[s->players[i].pos.y][s->players[i].pos.x] = 1;
    queue[tail++] = me;
    first[me.y][me.x] = COMMAND_ATTACK_UP;
    while (head < tail) {
        Position p = queue[head++];
        int distance = abs(p.x - goal.x) + abs(p.y - goal.y);
        if ((s->key_owner == s->self && distance == 0) ||
            (s->key_owner != s->self && distance == 1)) return (Command)first[p.y][p.x];
        for (d = 0; d < 4; ++d) {
            Position next = {p.x + dx[d], p.y + dy[d]};
            if (next.x < 0 || next.y < 0 || next.x >= s->size || next.y >= s->size ||
                s->terrain[next.y][next.x] == '#' || blocked[next.y][next.x] ||
                (s->terrain[next.y][next.x] == 'E' && s->key_owner != s->self) ||
                first[next.y][next.x] >= 0) continue;
            first[next.y][next.x] = head == 1 ? d : first[p.y][p.x];
            queue[tail++] = next;
        }
    }
    return COMMAND_ATTACK_UP;
}

/* Test multiple plausible policies using only public state. No opponent's
   worker, private memory or actual command is queried by this prediction. */
static Command p5_predict_rival(const Snapshot *s, int model)
{
    int style;
    if (model == 0 && (s->self == 2 || s->self == 4)) {
        Position p = s->players[s->self].pos, target = s->players[5].pos;
        Command preferred = s->self == 2 ? p5_predict_attacker(s) :
            strategy_choose(s, STRATEGY_INTERCEPTOR);
        int near_fight = abs(p.x - target.x) + abs(p.y - target.y) <= 6;
        if (s->key_owner >= 0 && s->key_owner != s->self) {
            target = s->players[s->key_owner].pos;
            if (abs(p.x - target.x) + abs(p.y - target.y) <= 3) near_fight = 1;
        }
        if (near_fight || s->key_owner == s->self)
            return tactics_choose(s, preferred, s->self == 2);
        return preferred;
    }
    if (model == 1) style = STRATEGY_RUNNER;
    else if (model == 2) style = STRATEGY_HUNTER;
    else if (model == 3) style = STRATEGY_INTERCEPTOR;
    else style = s->self == 0 ? STRATEGY_RUNNER :
        s->self == 1 ? STRATEGY_CAUTIOUS :
        s->self == 3 ? STRATEGY_HUNTER : STRATEGY_INTERCEPTOR;
    return strategy_choose(s, style);
}


#define P5_BEAM 40
#define P5_HORIZON 24
#define P5_FAR 10000

/* Terrain is immutable during a decision. Reuse BFS results for repeated goals
   so the time budget can be spent comparing more tactical futures. */
static int p5_cached[2][MAP_MAX * MAP_MAX];
static int p5_distance_cache[2][MAP_MAX * MAP_MAX][MAP_MAX][MAP_MAX];

typedef struct P5Plan {
    Game world;
    Game reclaim_world;
    Command first;
    int score, cost, regained, length, uncertain, engaged;
    int reclaim_cost, reclaim_length;
    unsigned char actions[P5_HORIZON];
} P5Plan;

/* Breadth-first distances on the public map; the locked exit is not a bridge. */
static void p5_distances(const Game* g, Position goal, int unlocked,
    int distance[MAP_MAX][MAP_MAX])
{
    const int dx[4] = { 0, 0, -1, 1 }, dy[4] = { -1, 1, 0, 0 };
    Position queue[MAP_MAX * MAP_MAX];
    int x, y, head = 0, tail = 0, d;
    int index;
    if (goal.x < 0 || goal.y < 0 || goal.x >= g->size || goal.y >= g->size) {
        for (y = 0; y < MAP_MAX; ++y)
            for (x = 0; x < MAP_MAX; ++x) distance[y][x] = P5_FAR;
        return;
    }
    unlocked = !!unlocked;
    index = goal.y * MAP_MAX + goal.x;
    if (p5_cached[unlocked][index]) {
        memcpy(distance, p5_distance_cache[unlocked][index], sizeof(int) * MAP_MAX * MAP_MAX);
        return;
    }
    for (y = 0; y < MAP_MAX; ++y)
        for (x = 0; x < MAP_MAX; ++x) distance[y][x] = P5_FAR;
    distance[goal.y][goal.x] = 0;
    queue[tail++] = goal;
    while (head < tail) {
        Position p = queue[head++];
        for (d = 0; d < 4; ++d) {
            x = p.x + dx[d]; y = p.y + dy[d];
            if (x < 0 || y < 0 || x >= g->size || y >= g->size ||
                g->terrain[y][x] == '#' || (!unlocked && g->terrain[y][x] == 'E') ||
                distance[y][x] != P5_FAR) continue;
            distance[y][x] = distance[p.y][p.x] + 1;
            queue[tail].x = x; queue[tail++].y = y;
        }
    }
    memcpy(p5_distance_cache[unlocked][index], distance, sizeof(int) * MAP_MAX * MAP_MAX);
    p5_cached[unlocked][index] = 1;
}

static int p5_clearance(const Game *g)
{
    int distance[MAP_MAX][MAP_MAX], i, nearest = 4;
    p5_distances(g, g->players[5].pos, 0, distance);
    for (i = 0; i < PLAYER_COUNT; ++i) {
        int steps;
        if (i == 5 || g->players[i].hp <= 0) continue;
        steps = distance[g->players[i].pos.y][g->players[i].pos.x];
        if (steps < nearest) nearest = steps;
    }
    return nearest;
}

/* Score real local outcomes: escape first, survival and key ownership next.
   Pursuing a carrier's future position lets P5 intercept instead of trailing it. */
static int p5_value(const Game* g)
{
    const PlayerState* me = &g->players[5];
    int distance[MAP_MAX][MAP_MAX], score, i, steps, routes = 0;
    Position goal = g->key_owner == 5 ? g->exit : g->key_owner >= 0 ?
        g->players[g->key_owner].pos : g->key;
    if (g->winner >= 0) return g->winner == 5 ? 1000000 : -1000000;
    score = 450 * me->hp - 5000 * (int)(me->deaths > 100 ? 100 : me->deaths);
    if (me->hp <= 0) score -= 4500;
    if (me->hp > 0) score += (me->hp == 1 ? 120 : 15) * p5_clearance(g);
    if (me->hp > 0) {
        const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
        for (i = 0; i < 4; ++i) {
            int x = me->pos.x + dx[i], y = me->pos.y + dy[i];
            if (x >= 0 && y >= 0 && x < g->size && y < g->size &&
                g->terrain[y][x] != '#' && (g->terrain[y][x] != 'E' || g->key_owner == 5)) ++routes;
        }
        score += (me->hp == 1 ? 25 : 10) * routes;
    }
    p5_distances(g, goal, g->key_owner == 5, distance);
    steps = distance[me->pos.y][me->pos.x];
    if (steps > 1000) steps = 1000;
    score -= 25 * steps;
    if (g->key_owner == 5) score += 1800;
    else if (g->key_owner < 0 && me->hp > 0 && steps <= 2) {
        int closest_rival = P5_FAR;
        /* A nearby key is still under our control if we can reclaim it first.
           Do not reward a bait that an opponent can immediately steal. */
        for (i = 0; i < PLAYER_COUNT; ++i) {
            int rival_steps;
            if (i == 5 || g->players[i].hp <= 0) continue;
            rival_steps = distance[g->players[i].pos.y][g->players[i].pos.x];
            if (rival_steps < closest_rival) closest_rival = rival_steps;
        }
        if (steps > 0 && steps < closest_rival) score += 1450;
        else if (steps > 0 && steps == closest_rival) score += 500;
        /* Keep progress toward the exit while temporarily putting the key down. */
        p5_distances(g, g->exit, 1, distance);
        score -= 15 * (distance[g->key.y][g->key.x] > 1000 ?
            1000 : distance[g->key.y][g->key.x]);
    }
    else if (g->key_owner >= 0) {
        Position carrier = g->players[g->key_owner].pos;
        score -= 700;
        p5_distances(g, g->exit, 1, distance);
        score += 35 * distance[carrier.y][carrier.x];
        score -= 90 * g->players[g->key_owner].hp;
    }
    /* Penalize exposed low-health positions without forbidding useful trades. */
    for (i = 0; i < PLAYER_COUNT; ++i) {
        if (i == 5 || g->players[i].hp <= 0) continue;
        /* Every living opponent is a rival, including healthy non-carriers.
           Lowering their HP opens routes and makes later key contests safer. */
        score -= 180 * g->players[i].hp;
        if (abs(g->players[i].pos.x - g->exit.x) +
            abs(g->players[i].pos.y - g->exit.y) <= 2)
            score -= 120 * g->players[i].hp;
        if (abs(g->players[i].pos.x - me->pos.x) <= 1 &&
            abs(g->players[i].pos.y - me->pos.y) <= 1)
            score -= me->hp == 1 ? 300 : 40;
    }
    return score;
}

/* A temporary release must beat an equal-length keep-key plan with a concrete
   gain. Merely putting the same key down and picking it up earns nothing. */
static int p5_drop_is_better(const P5Plan *drop, const P5Plan *keep)
{
    int i, drop_hp = 0, keep_hp = 0, drop_guards = 0, keep_guards = 0;
    unsigned int drop_deaths = 0, keep_deaths = 0;
    if (!drop->regained || !drop->engaged || drop->world.key_owner != 5 || drop->world.players[5].hp <= 0) return 0;
    if (drop->world.winner == 5) return keep->world.winner != 5 || drop->world.turn < keep->world.turn;
    if (keep->world.winner == 5 || drop->score < keep->score + 80) return 0;
    if (drop->world.players[5].hp > keep->world.players[5].hp) return 1;
    for (i = 0; i < PLAYER_COUNT; ++i) {
        const PlayerState *a = &drop->world.players[i], *b = &keep->world.players[i];
        if (i == 5) continue;
        drop_hp += a->hp; keep_hp += b->hp;
        drop_deaths += a->deaths; keep_deaths += b->deaths;
        if (a->hp > 0 && abs(a->pos.x - drop->world.exit.x) + abs(a->pos.y - drop->world.exit.y) <= 2) ++drop_guards;
        if (b->hp > 0 && abs(b->pos.x - keep->world.exit.x) + abs(b->pos.y - keep->world.exit.y) <= 2) ++keep_guards;
    }
    if (drop_deaths > keep_deaths) return 1;
    return drop->world.players[5].hp >= keep->world.players[5].hp &&
        (drop_hp < keep_hp || drop_guards < keep_guards ||
        p5_clearance(&drop->world) >= p5_clearance(&keep->world) + 2);
}

/* Forecast each rival from the same public state. No live core state, hidden
   commands, other snapshot files or winner overrides are accessed. */
static void p5_forecast(const Game* g, Command commands[PLAYER_COUNT], int model)
{
    Snapshot s;
    int i;
    memset(&s, 0, sizeof(s));
    s.size = g->size; s.turn = g->turn; s.key_owner = g->key_owner;
    s.key = g->key; s.key_start = g->key_start; s.exit = g->exit;
    memcpy(s.terrain, g->terrain, sizeof(s.terrain));
    memcpy(s.players, g->players, sizeof(s.players));
    for (i = 0; i < PLAYER_COUNT; ++i) {
        s.self = i;
        commands[i] = i != 5 && s.players[i].hp > 0 ? p5_predict_rival(&s, model) : COMMAND_ATTACK_UP;
    }
}

/* Replay the same continuation while retaining the key. Replace release/pickup
   with a legal stationary attack, so incidental gains from waiting or another
   player's fight cannot be mistaken for a benefit caused by releasing the key. */
static int p5_confirm_release(const Game *initial, const P5Plan *drop, ULONGLONG started)
{
    const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    P5Plan control = {0}, secured = *drop;
    int turn;
    control.world = *initial;
    for (turn = 0; turn < drop->reclaim_length && control.world.winner < 0; ++turn) {
        Command commands[PLAYER_COUNT], action = (Command)drop->actions[turn];
        int i, hits = 0;
        if (GetTickCount64() - started >= 1100) return 0;
        if (turn) game_begin_turn(&control.world);
        if (control.world.players[5].hp <= 0) break;
        p5_forecast(&control.world, commands, 0);
        if (action >= COMMAND_PICK_UP) {
            int d, best = -1;
            Position me = control.world.players[5].pos;
            for (d = 0; d < 4; ++d) {
                int count = 0;
                for (i = 0; i < PLAYER_COUNT; ++i) {
                    int px = control.world.players[i].pos.x - me.x;
                    int py = control.world.players[i].pos.y - me.y;
                    if (i == 5 || control.world.players[i].hp <= 0) continue;
                    if ((d < 2 && py == dy[d] && abs(px) <= 1) ||
                        (d >= 2 && px == dx[d] && abs(py) <= 1)) ++count;
                }
                if (count > best) { best = count; action = (Command)(COMMAND_ATTACK_UP + d); }
            }
        }
        {
            int hp[PLAYER_COUNT];
            for (i = 0; i < PLAYER_COUNT; ++i) hp[i] = control.world.players[i].hp;
            commands[5] = action;
            game_resolve(&control.world, commands);
            for (i = 0; i < PLAYER_COUNT; ++i)
                if (i != 5 && control.world.players[i].hp < hp[i]) hits = 1;
        }
        control.cost += 5 + (action >= COMMAND_ATTACK_UP && !hits ? 20 : 0);
    }
    control.score = p5_value(&control.world) - control.cost;
    /* Demand the advantage by the actual reclaim, not in a distant future
       which the next decision could abandon after an otherwise useless cycle. */
    secured.world = drop->reclaim_world;
    secured.cost = drop->reclaim_cost;
    secured.score = p5_value(&secured.world) - secured.cost;
    return p5_drop_is_better(&secured, &control);
}

/* A defender may keep attacking instead of chasing a dropped bait. Evaluate
   that reply too, rather than depending on a single prediction being correct. */
static Command p5_guard_reply(const Game *g, Command predicted)
{
    Position p = g->players[2].pos, target = g->players[5].pos;
    int px, py;
    if (g->players[2].hp <= 0 || g->key_owner == 2 ||
        abs(p.x - g->exit.x) + abs(p.y - g->exit.y) > 2) return predicted;
    px = target.x - p.x; py = target.y - p.y;
    if (abs(px) > 1 || abs(py) > 1 || (!px && !py)) {
        px = g->exit.x - p.x; py = g->exit.y - p.y;
        if (abs(px) > 1 || abs(py) > 1) return predicted;
    }
    if (py) return py < 0 ? COMMAND_ATTACK_UP : COMMAND_ATTACK_DOWN;
    return px < 0 ? COMMAND_ATTACK_LEFT : COMMAND_ATTACK_RIGHT;
}

/* Beam search keeps a bounded set of promising futures. The official resolver
   runs only on PRIVATE copies reconstructed from the file, so collisions,
   attack fans, death-before-victory and respawns have the same rules for all. */
Command player5_choose_command(const char* state_path)
{
    Snapshot s;
    FILE* file = fopen(state_path, "r");
    P5Plan* current, * next, * swap;
    Game initial;
    int ok, depth, count[2] = {1, 0}, i;
    int emergency_score = -2000000000;
    ULONGLONG started = GetTickCount64();
    Command result = COMMAND_ATTACK_UP;
    if (!file) return result;
    ok = snapshot_read(file, &s);
    fclose(file);
    if (!ok || s.self != 5) return result;
    memset(p5_cached, 0, sizeof(p5_cached));
    current = (P5Plan*)calloc(P5_BEAM * 2, sizeof(*current));
    next = (P5Plan*)calloc(P5_BEAM * 2, sizeof(*next));
    if (!current || !next) { free(current); free(next); return result; }
    current[0].world.size = s.size; current[0].world.turn = s.turn;
    current[0].world.key_owner = s.key_owner; current[0].world.winner = -1;
    current[0].world.key = s.key; current[0].world.key_start = s.key_start;
    current[0].world.exit = s.exit;
    memcpy(current[0].world.players, s.players, sizeof(s.players));
    memcpy(current[0].world.terrain, s.terrain, sizeof(s.terrain));
    current[0].first = result;
    initial = current[0].world;
    /* Keep-key and release-key futures have separate beams. Compare them only
       after completing the same depth, so a timeout cannot favor a partial plan. */
    for (depth = 0; depth < P5_HORIZON && GetTickCount64() - started < 1000; ++depth) {
        int used[2] = {0, 0}, group, complete = 1, chosen = -1;
        for (group = 0; group < 2 && complete; ++group) {
            for (i = 0; i < count[group]; ++i) {
                int index = group * P5_BEAM + i;
                Game start = current[index].world;
                Command commands[PLAYER_COUNT], alternatives[3][PLAYER_COUNT], guard_reply;
                int action, model, models = depth < 3 ? 3 : 0;
                if (depth > 0 && GetTickCount64() - started >= 1000) { complete = 0; break; }
                if (depth && start.winner < 0) game_begin_turn(&start);
                p5_forecast(&start, commands, 0);
                for (model = 0; model < models; ++model)
                    p5_forecast(&start, alternatives[model], model + 1);
                guard_reply = p5_guard_reply(&start, commands[2]);
                for (action = 0; action < (start.winner < 0 ? COMMAND_COUNT : 1); ++action) {
                    P5Plan candidate;
                    int at, j, bucket, offset, limit, hits = 0, risk = 0, fatal_reply = 0;
                    int key_commit = 0;
                    /* The control beam retains the key; a release plan gets one
                       release/reclaim episode, rather than repeated item loops. */
                    if (action >= COMMAND_DROP_UP &&
                        ((depth && group == 0) || (group == 1 && current[index].regained))) continue;
                    if (depth == 0 && action >= COMMAND_DROP_UP && p5_clearance(&start) > 3) continue;
                    candidate.world = start;
                    commands[5] = (Command)action;
                    game_resolve(&candidate.world, commands);
                    candidate.score = p5_value(&candidate.world);
                    for (model = 0; model < models; ++model) {
                        Game reply = start;
                        int reply_score;
                        alternatives[model][5] = (Command)action;
                        game_resolve(&reply, alternatives[model]);
                        reply_score = p5_value(&reply);
                        if (start.players[5].hp > 0 && reply.players[5].hp <= 0) fatal_reply = 1;
                        if (candidate.score - reply_score > risk) risk = candidate.score - reply_score;
                    }
                    if (guard_reply != commands[2]) {
                        Game defended = start;
                        Command predicted = commands[2];
                        int defended_score;
                        commands[2] = guard_reply;
                        game_resolve(&defended, commands);
                        commands[2] = predicted;
                        defended_score = p5_value(&defended);
                        if (start.players[5].hp > 0 && defended.players[5].hp <= 0) fatal_reply = 1;
                        if (candidate.score - defended_score > risk) risk = candidate.score - defended_score;
                    }
                    /* An alternative policy is a risk estimate, not knowledge
                       of the opponent's command. Retain a penalized primary-safe
                       gamble when every guaranteed-safe continuation loses. */
                    for (j = 0; j <= 1; ++j)
                        if (start.players[j].hp > 0 &&
                            ((start.key_owner < 0 && commands[j] >= COMMAND_PICK_UP && commands[j] <= COMMAND_PICK_RIGHT) ||
                            (start.key_owner == j && commands[j] < COMMAND_ATTACK_UP))) key_commit = 1;
                    if (depth == 0 && action < COMMAND_DROP_UP && !candidate.world.cancelled[5] &&
                        candidate.world.players[5].hp > 0 && candidate.score > emergency_score) {
                        emergency_score = candidate.score;
                        result = (Command)action;
                    }
                    if (fatal_reply && !key_commit) continue;
                    candidate.uncertain = current[index].uncertain || fatal_reply;
                    if (depth == 0 && start.players[5].hp == 1) {
                        Position final = candidate.world.players[5].pos;
                        for (j = 0; j < PLAYER_COUNT; ++j) {
                            int committed;
                            if (j == 5 || start.players[j].hp <= 0 ||
                                abs(start.players[j].pos.x - final.x) > 1 ||
                                abs(start.players[j].pos.y - final.y) > 1) continue;
                            /* These public policies are modeled exactly; the
                               resolver already checks whether their chosen fan
                               hits the final tile. Allow a real dodge or pickup bait. */
                            committed = j == 0 || j == 1 || j == 3;
                            if (!committed) break;
                        }
                        if (j < PLAYER_COUNT) continue;
                    }
                    if (start.players[5].hp > 0 && start.winner < 0 && candidate.world.cancelled[5]) continue;
                    if (start.players[5].hp > 0 && candidate.world.players[5].hp <= 0) continue;
                    candidate.first = depth ? current[index].first : (Command)action;
                    bucket = candidate.first >= COMMAND_DROP_UP;
                    offset = bucket * P5_BEAM;
                    limit = bucket ? 8 : P5_BEAM;
                    candidate.regained = current[index].regained || (bucket && candidate.world.key_owner == 5);
                    candidate.engaged = current[index].engaged;
                    if (bucket && !current[index].regained) {
                        if (action < COMMAND_ATTACK_UP &&
                            (candidate.world.players[5].pos.x != start.players[5].pos.x ||
                            candidate.world.players[5].pos.y != start.players[5].pos.y)) candidate.engaged = 1;
                        if (candidate.world.key_owner >= 0 && candidate.world.key_owner != 5) candidate.engaged = 1;
                        for (j = 0; j < PLAYER_COUNT; ++j)
                            if (j != 5 && start.key_owner < 0 &&
                                commands[j] >= COMMAND_PICK_UP && commands[j] <= COMMAND_PICK_RIGHT) candidate.engaged = 1;
                    }
                    candidate.reclaim_world = current[index].reclaim_world;
                    candidate.reclaim_cost = current[index].reclaim_cost;
                    candidate.reclaim_length = current[index].reclaim_length;
                    candidate.length = current[index].length;
                    memcpy(candidate.actions, current[index].actions, sizeof(candidate.actions));
                    if (start.winner < 0 && candidate.length < P5_HORIZON)
                        candidate.actions[candidate.length++] = (unsigned char)action;
                    for (j = 0; j < PLAYER_COUNT; ++j)
                        if (j != 5 && candidate.world.players[j].hp < start.players[j].hp) hits = 1;
                    candidate.cost = current[index].cost;
                    if (start.winner < 0) candidate.cost += 5 +
                        (action >= COMMAND_DROP_UP ? 70 :
                        action >= COMMAND_PICK_UP ? 15 :
                        action >= COMMAND_ATTACK_UP && !hits ? 20 : 0);
                    if (candidate.regained && !current[index].regained) {
                        candidate.reclaim_world = candidate.world;
                        candidate.reclaim_cost = candidate.cost;
                        candidate.reclaim_length = candidate.length;
                    }
                    candidate.score -= candidate.cost + (risk > 1200 ? 1200 : risk) / 3 +
                        (candidate.uncertain ? 1800 : 0);
                    for (j = 0; j < used[bucket]; ++j) {
                        const Game *other = &next[offset + j].world;
                        if (other->winner == candidate.world.winner &&
                            other->key_owner == candidate.world.key_owner &&
                            other->key.x == candidate.world.key.x && other->key.y == candidate.world.key.y &&
                            !memcmp(other->players, candidate.world.players, sizeof(start.players))) break;
                    }
                    if (j < used[bucket]) {
                        if (candidate.score <= next[offset + j].score) continue;
                        for (at = j; at + 1 < used[bucket]; ++at) next[offset + at] = next[offset + at + 1];
                        --used[bucket];
                    }
                    at = used[bucket];
                    while (at > 0 && candidate.score > next[offset + at - 1].score) --at;
                    if (at >= limit) continue;
                    if (used[bucket] < limit) ++used[bucket];
                    for (j = used[bucket] - 1; j > at; --j) next[offset + j] = next[offset + j - 1];
                    next[offset + at] = candidate;
                }
            }
        }
        if (!complete || (!used[0] && !used[1])) break;
        swap = current; current = next; next = swap;
        count[0] = used[0]; count[1] = used[1];
        if (count[0]) {
            chosen = 0;
            for (i = 0; i < count[1]; ++i)
                if (p5_drop_is_better(&current[P5_BEAM + i], &current[0]) &&
                    (chosen == 0 || current[P5_BEAM + i].score > current[chosen].score)) chosen = P5_BEAM + i;
        }
        else {
            /* All keep-key continuations can be eliminated by fatal replies.
               A surviving release/reclaim still needs the concrete replay proof. */
            for (i = 0; i < count[1]; ++i)
                if (current[P5_BEAM + i].regained && current[P5_BEAM + i].world.key_owner == 5 &&
                    (chosen < 0 || current[P5_BEAM + i].score > current[chosen].score)) chosen = P5_BEAM + i;
        }
        if (chosen >= 0) {
            if (chosen > 0 && !p5_confirm_release(&initial, &current[chosen], started)) chosen = count[0] ? 0 : -1;
            if (chosen >= 0) {
                result = current[chosen].first;
                if (current[chosen].world.winner == 5) break;
            }
        }
    }
    free(current); free(next);
    /* Do not stall indefinitely with empty attacks when a goal-directed move is
       safe under every root model. This also finds detours around occupied tiles. */
    if (result >= COMMAND_ATTACK_UP && result <= COMMAND_ATTACK_RIGHT) {
        Game start = {0};
        Command commands[PLAYER_COUNT], alternative = strategy_choose(&s, STRATEGY_CAUTIOUS);
        int model, safe = 1, hits = 0, d = result - COMMAND_ATTACK_UP;
        Position me = s.players[5].pos;
        for (i = 0; i < PLAYER_COUNT; ++i) {
            int px = s.players[i].pos.x - me.x, py = s.players[i].pos.y - me.y;
            if (i == 5 || s.players[i].hp <= 0) continue;
            if ((d < 2 && py == (d == 0 ? -1 : 1) && abs(px) <= 1) ||
                (d >= 2 && px == (d == 2 ? -1 : 1) && abs(py) <= 1)) hits = 1;
        }
        start.size = s.size; start.turn = s.turn; start.key_owner = s.key_owner; start.winner = -1;
        start.key = s.key; start.key_start = s.key_start; start.exit = s.exit;
        memcpy(start.players, s.players, sizeof(s.players));
        memcpy(start.terrain, s.terrain, sizeof(s.terrain));
        if (!hits && (alternative < COMMAND_ATTACK_UP ||
            (alternative >= COMMAND_PICK_UP && alternative <= COMMAND_PICK_RIGHT))) {
            for (model = 0; model < 4; ++model) {
                Game reply = start;
                p5_forecast(&start, commands, model);
                commands[5] = alternative;
                game_resolve(&reply, commands);
                if (reply.cancelled[5] || reply.players[5].hp < s.players[5].hp ||
                    (reply.winner >= 0 && reply.winner != 5)) safe = 0;
                if (s.players[5].hp == 1 && strategy_danger(&s, reply.players[5].pos)) safe = 0;
            }
            if (safe) result = alternative;
        }
    }
    return result;
}
