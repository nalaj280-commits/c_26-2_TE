#include "tactics.h"

/* Establish an exit ambush first. Claim a nearby key from the post, then escape. */
static Command player2_preferred(const Snapshot *state)
{
    Snapshot s = *state;
    const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    int distance[MAP_MAX][MAP_MAX], occupied[MAP_MAX][MAP_MAX] = {0};
    Position queue[MAP_MAX * MAP_MAX], me;
    int d, j, x, y, head = 0, tail = 0;
    int guard_direction = -1, best_direction = 0, best_score = -1;
    Position goal;
    me = s.players[s.self].pos;
    /* Take an immediate escape instead of prolonging a fight. */
    if (s.key_owner == s.self) {
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == s.exit.x && me.y + dy[d] == s.exit.y)
                return (Command)d;
    }
    for (d = 0; d < 4; ++d) {
        int score = 0;
        for (j = 0; j < PLAYER_COUNT; ++j) {
            int px, py;
            if (j == s.self || s.players[j].hp <= 0) continue;
            px = s.players[j].pos.x - me.x;
            py = s.players[j].pos.y - me.y;
            if ((d < 2 && py == dy[d] && abs(px) <= 1) ||
                (d >= 2 && px == dx[d] && abs(py) <= 1))
                score += j == s.key_owner ? 100 : s.players[j].hp == 1 ? 20 : 10;
        }
        if (score > best_score) { best_score = score; best_direction = d; }
    }
    if (best_score > 0) return (Command)(COMMAND_ATTACK_UP + best_direction);
    if (s.key_owner < 0 && (abs(me.x - s.exit.x) + abs(me.y - s.exit.y) == 1 ||
        abs(s.key.x - s.exit.x) + abs(s.key.y - s.exit.y) <= 1)) {
        for (d = 0; d < 4; ++d)
            if (me.x + dx[d] == s.key.x && me.y + dy[d] == s.key.y)
                return (Command)(COMMAND_PICK_UP + d);
    }
    goal = s.exit;
    if (s.key_owner >= 0 && s.key_owner != s.self) {
        Position carrier = s.players[s.key_owner].pos;
        if (abs(carrier.x - s.exit.x) + abs(carrier.y - s.exit.y) <= 4 &&
            abs(me.x - s.exit.x) + abs(me.y - s.exit.y) <= 4) goal = carrier;
    }
    /* A key dropped on our own post needs a one-tile sidestep before pickup. */
    if (s.key_owner < 0 && abs(s.key.x - s.exit.x) + abs(s.key.y - s.exit.y) <= 1 &&
        abs(me.x - s.exit.x) + abs(me.y - s.exit.y) <= 2) goal = s.key;
    for (j = 0; j < PLAYER_COUNT; ++j)
        if (j != s.self && s.players[j].hp > 0)
            occupied[s.players[j].pos.y][s.players[j].pos.x] = 1;
    for (y = 0; y < s.size; ++y)
        for (x = 0; x < s.size; ++x) distance[y][x] = -1;
    /* Always establish the post, even when the loose key is on another route.
       Possession changes the goal from an ambush tile to the exit itself. */
    for (d = 0; d < 4; ++d) {
        Position p = {goal.x + dx[d], goal.y + dy[d]};
        if (s.key_owner == s.self) p = s.exit;
        if (p.x < 0 || p.y < 0 || p.x >= s.size || p.y >= s.size ||
            s.terrain[p.y][p.x] == '#' ||
            distance[p.y][p.x] >= 0 ||
            (s.terrain[p.y][p.x] == 'E' && s.key_owner != s.self)) continue;
        distance[p.y][p.x] = 0;
        queue[tail++] = p;
        if (s.key_owner != s.self && goal.x == s.exit.x && goal.y == s.exit.y &&
            p.x == me.x && p.y == me.y) guard_direction = d ^ 1;
    }
    while (head < tail) {
        Position p = queue[head++];
        for (d = 0; d < 4; ++d) {
            Position next = {p.x + dx[d], p.y + dy[d]};
            if (next.x < 0 || next.y < 0 || next.x >= s.size || next.y >= s.size ||
                s.terrain[next.y][next.x] == '#' ||
                (s.terrain[next.y][next.x] == 'E' && s.key_owner != s.self) ||
                distance[next.y][next.x] >= 0) continue;
            distance[next.y][next.x] = distance[p.y][p.x] + 1;
            queue[tail++] = next;
        }
    }
    if (guard_direction < 0 && distance[me.y][me.x] > 0) {
        for (d = 0; d < 4; ++d) {
            x = me.x + dx[d]; y = me.y + dy[d];
            if (x >= 0 && y >= 0 && x < s.size && y < s.size &&
                !occupied[y][x] && distance[y][x] == distance[me.y][me.x] - 1) return (Command)d;
        }
    }
    if (guard_direction >= 0) best_direction = guard_direction;
    return (Command)(COMMAND_ATTACK_UP + best_direction);
}

Command player2_choose_command(const char *state_path)
{
    Snapshot s;
    FILE *file = fopen(state_path, "r");
    int ok;
    if (!file) return COMMAND_ATTACK_UP;
    ok = snapshot_read(file, &s);
    fclose(file);
    if (!ok) return COMMAND_ATTACK_UP;
    return tactics_choose(&s, player2_preferred(&s), 1);
}
