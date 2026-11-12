#ifndef TACTICS_H
#define TACTICS_H
#include "game.h"
#include "strategy.h"

/* Evaluate positions with a wall-aware distance field. Occupied distant tiles
   remain reachable in the estimate; the resolver rejects actual collisions. */
static int tactics_value(const Game *g, int self, int camper)
{
    const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    Position queue[MAP_MAX * MAP_MAX + 1], goal = g->exit, me = g->players[self].pos;
    int distance[MAP_MAX][MAP_MAX], occupied[MAP_MAX][MAP_MAX] = {0};
    int queued[MAP_MAX][MAP_MAX] = {0}, head = 0, tail = 0, count = 0;
    int x, y, d, i, steps, score, capacity = MAP_MAX * MAP_MAX + 1;
    int owns = g->key_owner == self, guarding = !owns;
    if (g->winner >= 0) return g->winner == self ? 1000000 : -1000000;
    score = 900 * g->players[self].hp - 6000 * (int)(g->players[self].deaths > 100 ? 100 : g->players[self].deaths);
    if (g->players[self].hp <= 0) score -= 6000;
    if (owns) score += 2200;
    else if (g->key_owner < 0) {
        int key_exit = abs(g->key.x - g->exit.x) + abs(g->key.y - g->exit.y);
        int self_exit = abs(me.x - g->exit.x) + abs(me.y - g->exit.y);
        if (!camper || key_exit <= 1 || (self_exit == 1 && key_exit <= 2)) {
            goal = g->key; guarding = 0;
        }
    }
    else {
        Position carrier = g->players[g->key_owner].pos;
        int approach = abs(carrier.x - g->exit.x) + abs(carrier.y - g->exit.y);
        score -= 1800;
        if (approach <= (camper ? 4 : 6) ||
            (!camper && abs(carrier.x - me.x) + abs(carrier.y - me.y) <= 4)) {
            goal = carrier; guarding = 0;
        }
    }
    for (y = 0; y < g->size; ++y)
        for (x = 0; x < g->size; ++x) distance[y][x] = -1;
    for (i = 0; i < PLAYER_COUNT; ++i)
        if (i != self && g->players[i].hp > 0)
            occupied[g->players[i].pos.y][g->players[i].pos.x] = 1;
    for (d = 0; d < 4; ++d) {
        Position p = {goal.x + dx[d], goal.y + dy[d]};
        if (owns) p = goal;
        if (p.x < 0 || p.y < 0 || p.x >= g->size || p.y >= g->size ||
            g->terrain[p.y][p.x] == '#' || (g->terrain[p.y][p.x] == 'E' && !owns) ||
            distance[p.y][p.x] >= 0) continue;
        distance[p.y][p.x] = occupied[p.y][p.x] ? 4 : 0;
        queue[tail] = p; tail = (tail + 1) % capacity; ++count;
        queued[p.y][p.x] = 1;
    }
    /* Soft occupancy costs choose a detour when available, but still approach
       a blocked choke point when fighting through it is the only route. */
    while (count) {
        Position p = queue[head];
        head = (head + 1) % capacity; --count; queued[p.y][p.x] = 0;
        for (d = 0; d < 4; ++d) {
            Position next = {p.x + dx[d], p.y + dy[d]};
            int cost;
            if (next.x < 0 || next.y < 0 || next.x >= g->size || next.y >= g->size ||
                g->terrain[next.y][next.x] == '#' ||
                (g->terrain[next.y][next.x] == 'E' && !owns)) continue;
            cost = distance[p.y][p.x] + 1 + (occupied[next.y][next.x] ? 4 : 0);
            if (distance[next.y][next.x] >= 0 && distance[next.y][next.x] <= cost) continue;
            distance[next.y][next.x] = cost;
            if (!queued[next.y][next.x]) {
                queue[tail] = next; tail = (tail + 1) % capacity; ++count;
                queued[next.y][next.x] = 1;
            }
        }
    }
    steps = distance[me.y][me.x];
    if (steps < 0) steps = 1000;
    score -= (owns ? 140 : 100) * steps;
    if (guarding && steps == 0) score += camper ? 450 : 200;
    for (i = 0; i < PLAYER_COUNT; ++i) {
        if (i == self) continue;
        score -= 160 * g->players[i].hp;
        score += 1200 * (int)(g->players[i].deaths > 100 ? 100 : g->players[i].deaths);
    }
    return score;
}

/* Compare legal actions against both a goal-seeking reply and a nearby
   opponent's retaliation. Only private copies of the public snapshot are used. */
static Command tactics_choose(const Snapshot *s, Command preferred, int camper)
{
    Game start = {0};
    Command normal[PLAYER_COUNT], hostile[PLAYER_COUNT], result = preferred;
    int i, action, best = -2000000000;
    start.size = s->size; start.turn = s->turn; start.key_owner = s->key_owner; start.winner = -1;
    start.key = s->key; start.key_start = s->key_start; start.exit = s->exit;
    memcpy(start.players, s->players, sizeof(s->players));
    memcpy(start.terrain, s->terrain, sizeof(s->terrain));
    for (i = 0; i < PLAYER_COUNT; ++i) {
        Snapshot rival = *s;
        Position p = s->players[i].pos, target = s->players[s->self].pos;
        int px = target.x - p.x, py = target.y - p.y;
        rival.self = i;
        normal[i] = s->players[i].hp > 0 && i != s->self ?
            strategy_choose(&rival, i == 3 ? STRATEGY_HUNTER : STRATEGY_CAUTIOUS) : COMMAND_ATTACK_UP;
        hostile[i] = normal[i];
        if (i != s->self && s->players[i].hp > 0 && abs(px) <= 1 && abs(py) <= 1 && (px || py))
            hostile[i] = py ? (py < 0 ? COMMAND_ATTACK_UP : COMMAND_ATTACK_DOWN) :
                (px < 0 ? COMMAND_ATTACK_LEFT : COMMAND_ATTACK_RIGHT);
    }
    for (action = 0; action < COMMAND_COUNT; ++action) {
        Game expected = start, attacked = start;
        int score, risk, j, gives_up_post = 0;
        if (camper && action >= COMMAND_PICK_UP && action <= COMMAND_PICK_RIGHT &&
            abs(s->players[s->self].pos.x - s->exit.x) +
                abs(s->players[s->self].pos.y - s->exit.y) != 1 &&
            abs(s->key.x - s->exit.x) + abs(s->key.y - s->exit.y) > 1) continue;
        normal[s->self] = hostile[s->self] = (Command)action;
        game_resolve(&expected, normal);
        game_resolve(&attacked, hostile);
        if (expected.cancelled[s->self] || attacked.cancelled[s->self]) continue;
        score = tactics_value(&expected, s->self, camper);
        risk = tactics_value(&attacked, s->self, camper);
        if (risk < score) score = risk;
        if (expected.players[s->self].hp <= 0 || attacked.players[s->self].hp <= 0) score -= 10000;
        if (action == (int)preferred) score += 35;
        /* Do not chase a far loose key away from the exit as a camping player. */
        if (camper && s->key_owner < 0 &&
            abs(s->key.x - s->exit.x) + abs(s->key.y - s->exit.y) > 2 && action < 4) {
            Position p = expected.players[s->self].pos, me = s->players[s->self].pos;
            gives_up_post = abs(me.x - s->exit.x) + abs(me.y - s->exit.y) == 1 &&
                abs(p.x - s->exit.x) + abs(p.y - s->exit.y) > 1;
            if (gives_up_post && attacked.players[s->self].hp == s->players[s->self].hp) score -= 500;
        }
        /* Prefer a productive attack over an equally rated empty attack. */
        for (j = 0; j < PLAYER_COUNT; ++j)
            if (j != s->self && expected.players[j].hp < s->players[j].hp) {
                score += 20;
                /* An exit defender should trade a nonfatal hit for real damage,
                   rather than retreat forever to preserve a perfect HP bar. */
                if (camper && action >= COMMAND_ATTACK_UP && action <= COMMAND_ATTACK_RIGHT &&
                    expected.players[s->self].hp > 0 && attacked.players[s->self].hp > 0 &&
                    attacked.players[j].hp < s->players[j].hp) score += 1000;
            }
        if (score > best) { best = score; result = (Command)action; }
    }
    return result;
}
#endif
