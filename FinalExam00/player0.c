#include "player_api.h"

/* All styles share the same victory objective. Styles change risk tolerance
   and interception, never HP, turn order, state visibility or game rules. */
enum { STRATEGY_RUNNER, STRATEGY_CAUTIOUS, STRATEGY_HUNTER, STRATEGY_INTERCEPTOR };

static int strategy_danger(const Snapshot *s, Position p)
{
    int i, danger = 0;
    for (i = 0; i < PLAYER_COUNT; ++i)
        if (i != s->self && s->players[i].hp > 0 &&
            abs(s->players[i].pos.x - p.x) <= 1 &&
            abs(s->players[i].pos.y - p.y) <= 1) ++danger;
    return danger;
}

static Command strategy_choose(const Snapshot *s, int style)
{
    const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    Position me = s->players[s->self].pos, goal;
    Position queue[MAP_MAX * MAP_MAX];
    int distance[MAP_MAX][MAP_MAX], occupied[MAP_MAX][MAP_MAX] = {0};
    int head = 0, tail = 0, x, y, d, i, best_attack = 0, attack_score = 0;
    int owns = s->key_owner == s->self, best_move = -1, move_score = -100000;
    if (owns) {
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == s->exit.x && me.y + dy[d] == s->exit.y &&
                (s->players[s->self].hp > 1 || !strategy_danger(s, s->exit))) return (Command)d;
    }
    for (d = 0; d < 4; ++d) {
        int score = 0;
        for (i = 0; i < PLAYER_COUNT; ++i) {
            int px = s->players[i].pos.x - me.x, py = s->players[i].pos.y - me.y;
            if (i == s->self || s->players[i].hp <= 0) continue;
            if ((d < 2 && py == dy[d] && abs(px) <= 1) ||
                (d >= 2 && px == dx[d] && abs(py) <= 1))
                score += i == s->key_owner ? 100 : s->players[i].hp == 1 ? 45 : 15;
        }
        if (score > attack_score) { attack_score = score; best_attack = d; }
    }
    if ((style == STRATEGY_HUNTER || style == STRATEGY_INTERCEPTOR) && attack_score > 0)
        return (Command)(COMMAND_ATTACK_UP + best_attack);
    if (s->key_owner < 0) {
        for (d = 0; d < 4; ++d) {
            Position p = {me.x + dx[d], me.y + dy[d]};
            if (p.x == s->key.x && p.y == s->key.y &&
                (style != STRATEGY_CAUTIOUS || s->players[s->self].hp > 1 ||
                    !strategy_danger(s, me))) return (Command)(COMMAND_PICK_UP + d);
        }
    }
    goal = owns ? s->exit : s->key_owner < 0 ? s->key : s->players[s->key_owner].pos;
    if (!owns && s->key_owner >= 0 && style == STRATEGY_INTERCEPTOR) goal = s->exit;
    for (i = 0; i < PLAYER_COUNT; ++i)
        if (i != s->self && s->players[i].hp > 0)
            occupied[s->players[i].pos.y][s->players[i].pos.x] = 1;
    for (y = 0; y < s->size; ++y)
        for (x = 0; x < s->size; ++x) distance[y][x] = -1;
    for (d = 0; d < 4; ++d) {
        Position p = {goal.x + dx[d], goal.y + dy[d]};
        if (owns) p = goal;
        if (p.x < 0 || p.y < 0 || p.x >= s->size || p.y >= s->size ||
            s->terrain[p.y][p.x] == '#' || occupied[p.y][p.x] ||
            (s->terrain[p.y][p.x] == 'E' && !owns) || distance[p.y][p.x] >= 0) continue;
        distance[p.y][p.x] = 0;
        queue[tail++] = p;
    }
    while (head < tail) {
        Position p = queue[head++];
        for (d = 0; d < 4; ++d) {
            Position next = {p.x + dx[d], p.y + dy[d]};
            if (next.x < 0 || next.y < 0 || next.x >= s->size || next.y >= s->size ||
                s->terrain[next.y][next.x] == '#' || occupied[next.y][next.x] ||
                (s->terrain[next.y][next.x] == 'E' && !owns) ||
                distance[next.y][next.x] >= 0) continue;
            distance[next.y][next.x] = distance[p.y][p.x] + 1;
            queue[tail++] = next;
        }
    }
    for (i = 0; i < 4; ++i) {
        int danger, score;
        Position p;
        d = (s->self + s->turn + i) % 4;
        p.x = me.x + dx[d]; p.y = me.y + dy[d];
        if (p.x < 0 || p.y < 0 || p.x >= s->size || p.y >= s->size ||
            s->terrain[p.y][p.x] == '#' || occupied[p.y][p.x] ||
            (s->terrain[p.y][p.x] == 'E' && !owns) || distance[p.y][p.x] < 0) continue;
        danger = strategy_danger(s, p);
        score = -100 * distance[p.y][p.x] - danger *
            (s->players[s->self].hp == 1 ? 180 : style == STRATEGY_CAUTIOUS ? 90 : 25);
        if (score > move_score) { move_score = score; best_move = d; }
    }
    if (attack_score >= 100 || (attack_score >= 45 && !owns) ||
        (attack_score > 0 && (best_move < 0 || distance[me.y][me.x] == 0)))
        return (Command)(COMMAND_ATTACK_UP + best_attack);
    if (best_move >= 0 && !(style == STRATEGY_INTERCEPTOR && s->key_owner >= 0 &&
        !owns && distance[me.y][me.x] == 0)) return (Command)best_move;
    /* An interceptor keeps the exit covered until the carrier approaches. */
    if (style == STRATEGY_INTERCEPTOR && s->key_owner >= 0 && !owns)
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == s->exit.x && me.y + dy[d] == s->exit.y)
                return (Command)(COMMAND_ATTACK_UP + d);
    return (Command)(COMMAND_ATTACK_UP + best_attack);
}

/* Compete for the key and escape using the runner policy. */
Command player0_choose_command(const char *state_path)
{
    Snapshot s;
    FILE *file = fopen(state_path, "r");
    int ok;
    if (!file) return COMMAND_ATTACK_UP;
    ok = snapshot_read(file, &s);
    fclose(file);
    if (!ok) return COMMAND_ATTACK_UP;
    return strategy_choose(&s, STRATEGY_RUNNER);
}
