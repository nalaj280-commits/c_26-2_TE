#include <windows.h>
#include "game.h"

static int failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test); ++failures; } } while (0)
static const char valid_map[] = "##########\n##P0@-()##\n##P1P2P3##\n  P4P5P6##\n##########\n";

/* Use a real temporary stream to cover the same parser as production map loading. */
static int load_text(Game *g, const char *text)
{
    char error[256];
    FILE *file = NULL;
    int ok;
    if (tmpfile_s(&file) != 0) { CHECK(0); return 0; }
    fputs(text, file); rewind(file);
    ok = game_load(g, file, error, sizeof(error));
    (fclose)(file); /* tmpfile is intentionally outside the fopen tracker. */
    return ok;
}

/* Create a deterministic empty arena; only explicitly placed actors are alive. */
static void arena(Game *g, Command commands[PLAYER_COUNT])
{
    int i;
    memset(g, 0, sizeof(*g));
    g->size = 7; g->key_owner = g->winner = -1;
    g->key.x = g->key_start.x = 5; g->key.y = g->key_start.y = 5;
    g->exit.x = 6; g->exit.y = 6;
    memset(g->terrain, ' ', sizeof(g->terrain));
    g->terrain[6][6] = 'E';
    for (i = 0; i < PLAYER_COUNT; ++i) {
        g->players[i].pos.x = g->players[i].spawn.x = i;
        g->players[i].pos.y = g->players[i].spawn.y = 0;
        g->players[i].rest = 100;
        commands[i] = COMMAND_ATTACK_UP;
    }
}

/* Activate a player and give it a separate initial spawn for lifecycle tests. */
static void place(Game *g, int id, int x, int y, int hp)
{
    g->players[id].pos.x = x; g->players[id].pos.y = y;
    g->players[id].hp = hp; g->players[id].rest = 0;
}

/* Exercise simultaneous conflicts, swaps, propagation and no-index-priority claims. */
static void test_collisions(void)
{
    Game g; Command c[PLAYER_COUNT];
    arena(&g, c);
    place(&g, 0, 2, 1, 2); place(&g, 1, 1, 2, 2); place(&g, 2, 3, 2, 2);
    c[0] = COMMAND_DOWN; c[1] = COMMAND_RIGHT; c[2] = COMMAND_LEFT;
    game_resolve(&g, c);
    CHECK(g.players[0].hp == 1 && g.players[1].hp == 1 && g.players[2].hp == 1);
    CHECK(g.players[0].pos.y == 1 && g.players[1].pos.x == 1 && g.players[2].pos.x == 3);
    arena(&g, c);
    place(&g, 0, 1, 1, 2); place(&g, 1, 2, 1, 2);
    c[0] = COMMAND_RIGHT; c[1] = COMMAND_LEFT;
    game_resolve(&g, c);
    CHECK(g.players[0].pos.x == 2 && g.players[1].pos.x == 1 && g.players[0].hp == 2);
    arena(&g, c);
    place(&g, 0, 1, 1, 2); place(&g, 1, 2, 1, 2); place(&g, 2, 3, 1, 2);
    c[0] = c[1] = COMMAND_RIGHT;
    game_resolve(&g, c);
    CHECK(g.cancelled[0] && g.cancelled[1] && !g.cancelled[2]);
    CHECK(g.players[0].pos.x == 1 && g.players[1].pos.x == 2);
    arena(&g, c);
    place(&g, 0, 1, 1, 2); place(&g, 1, 3, 1, 2); g.key_owner = 0;
    c[0] = COMMAND_DROP_RIGHT; c[1] = COMMAND_LEFT;
    game_resolve(&g, c);
    CHECK(g.cancelled[0] && g.cancelled[1] && g.key_owner == 0);
    arena(&g, c);
    place(&g, 0, 1, 1, 2); place(&g, 1, 2, 1, 2); g.key_owner = 0;
    c[0] = COMMAND_DROP_RIGHT;
    game_resolve(&g, c);
    CHECK(g.cancelled[0] && g.players[0].hp == 1 && g.players[1].hp == 2);
    arena(&g, c); place(&g, 0, 0, 0, 2); c[0] = COMMAND_LEFT;
    game_resolve(&g, c); CHECK(g.cancelled[0] && g.players[0].hp == 1);
    arena(&g, c); place(&g, 0, 1, 1, 2); g.terrain[1][2] = '#'; c[0] = COMMAND_RIGHT;
    game_resolve(&g, c); CHECK(g.cancelled[0] && g.players[0].hp == 1);
}

/* Verify all four three-cell attack fans, movement dodges and the per-turn damage cap. */
static void test_attacks(void)
{
    Game g; Command c[PLAYER_COUNT];
    const int dx[4] = {0, 0, -1, 1}, dy[4] = {-1, 1, 0, 0};
    int d, k;
    for (d = 0; d < 4; ++d) {
        arena(&g, c); place(&g, 0, 3, 3, 2); c[0] = (Command)(COMMAND_ATTACK_UP + d);
        for (k = -1; k <= 1; ++k) place(&g, k + 2, 3 + dx[d] + (d < 2 ? k : 0), 3 + dy[d] + (d >= 2 ? k : 0), 2);
        game_resolve(&g, c);
        CHECK(g.players[1].hp == 1 && g.players[2].hp == 1 && g.players[3].hp == 1);
    }
    arena(&g, c); place(&g, 0, 2, 2, 2); place(&g, 1, 1, 3, 2); place(&g, 2, 3, 3, 2);
    c[0] = (Command)999; c[1] = c[2] = COMMAND_ATTACK_UP;
    game_resolve(&g, c); CHECK(g.players[0].hp == 1 && g.cancelled[0]);
    arena(&g, c); place(&g, 0, 2, 3, 2); place(&g, 1, 2, 2, 2);
    c[0] = COMMAND_ATTACK_UP; c[1] = COMMAND_UP;
    game_resolve(&g, c); CHECK(g.players[1].hp == 2 && g.players[1].pos.y == 1);
    arena(&g, c); place(&g, 0, 2, 3, 1); place(&g, 1, 2, 2, 1);
    c[0] = COMMAND_ATTACK_UP; c[1] = COMMAND_ATTACK_DOWN;
    game_resolve(&g, c); CHECK(g.players[0].hp == 0 && g.players[1].hp == 0);
}

/* Item commands are explicit; death and victory are resolved after all attacks. */
static void test_items(void)
{
    Game g; Command c[PLAYER_COUNT];
    arena(&g, c); place(&g, 0, 4, 5, 2); c[0] = COMMAND_RIGHT;
    game_resolve(&g, c); CHECK(g.key_owner == -1); /* Walking over key does not pick it up. */
    arena(&g, c); place(&g, 0, 4, 5, 2); c[0] = COMMAND_PICK_RIGHT;
    game_resolve(&g, c); CHECK(g.key_owner == 0);
    c[0] = COMMAND_DROP_UP; game_resolve(&g, c);
    CHECK(g.key_owner == -1 && g.key.x == 4 && g.key.y == 4);
    arena(&g, c); place(&g, 0, 4, 5, 2); place(&g, 1, 6, 5, 2);
    c[0] = COMMAND_PICK_RIGHT; c[1] = COMMAND_PICK_LEFT;
    game_resolve(&g, c); CHECK(g.key_owner == -1 && g.players[0].hp == 1 && g.players[1].hp == 1);
    arena(&g, c); place(&g, 0, 5, 6, 2); c[0] = COMMAND_RIGHT;
    game_resolve(&g, c); CHECK(g.winner == -1 && g.players[0].pos.x == 5 && g.players[0].hp == 1);
    arena(&g, c); place(&g, 0, 5, 6, 2); g.key_owner = 0; c[0] = COMMAND_RIGHT;
    game_resolve(&g, c); CHECK(g.winner == 0);
    arena(&g, c); place(&g, 0, 5, 6, 1); place(&g, 1, 6, 5, 2); g.key_owner = 0;
    c[0] = COMMAND_RIGHT; c[1] = COMMAND_ATTACK_DOWN;
    game_resolve(&g, c);
    CHECK(g.winner == -1 && g.key_owner == -1 && g.players[0].hp == 0);
    CHECK(g.key.x == g.key_start.x && g.key.y == g.key_start.y);
    arena(&g, c); place(&g, 0, 2, 2, 1); g.key_owner = 0; c[0] = (Command)-1;
    game_resolve(&g, c); CHECK(g.key_owner == -1 && g.key.x == 2 && g.key.y == 2);
    arena(&g, c); place(&g, 0, 4, 5, 1); place(&g, 1, 4, 6, 2);
    c[0] = COMMAND_PICK_RIGHT; c[1] = COMMAND_ATTACK_UP;
    game_resolve(&g, c); CHECK(g.players[0].hp == 0 && g.key_owner == -1 && g.key.x == 4 && g.key.y == 5);
}

/* Rest is 1,2,4... whole skipped turns, not the death turn or the respawn turn. */
static void test_respawn(void)
{
    Game g; Command c[PLAYER_COUNT];
    int death, t;
    arena(&g, c); place(&g, 0, 2, 2, 1); c[0] = (Command)-1;
    for (death = 1; death <= 3; ++death) {
        g.players[0].hp = 1;
        game_resolve(&g, c);
        CHECK(g.players[0].deaths == (unsigned)death && g.players[0].rest == (UINT64_C(1) << (death - 1)));
        for (t = 0; t < (1 << (death - 1)); ++t) { game_begin_turn(&g); CHECK(g.players[0].hp == 0); }
        game_begin_turn(&g); CHECK(g.players[0].hp == 2 && g.players[0].pos.x == 0 && g.players[0].pos.y == 0);
    }
    g.players[0].hp = 0; g.players[0].rest = 0; place(&g, 1, 0, 0, 2);
    game_begin_turn(&g); CHECK(g.players[0].hp == 0 && g.players[0].deaths == 3);
    g.players[1].pos.x = 1;
    game_begin_turn(&g); CHECK(g.players[0].hp == 2);
}

/* Check map errors and serialize a v2 snapshot through an actual isolated worker. */
static void test_files(void)
{
    Game g; Snapshot s;
    char path[MAX_PATH], directory[MAX_PATH], log_path[MAX_PATH], changed[sizeof(valid_map)];
    FILE *file;
    DWORD length = GetTempPathA(sizeof(directory), directory);
    CHECK(length > 0 && length < sizeof(directory));
    if (!length || length >= sizeof(directory)) return;
    if (!GetTempFileNameA(directory, "fgc", 0, path)) { CHECK(0); return; }
    if (!GetTempFileNameA(directory, "fgl", 0, log_path)) { CHECK(0); DeleteFileA(path); return; }
    CHECK(load_text(&g, valid_map)); CHECK(g.size == 5 && PLAYER_COUNT == 6 && g.terrain[3][3] == ' ');
    /* A UTF-8 editor signature must not count toward the first row's cell width. */
    {
        char with_bom[sizeof(valid_map) + 3];
        memcpy(with_bom, "\xEF\xBB\xBF", 3);
        memcpy(with_bom + 3, valid_map, sizeof(valid_map));
        CHECK(load_text(&g, with_bom));
        CHECK(g.size == 5 && g.players[0].pos.x == 1);
    }
    CHECK(!load_text(&g, "")); CHECK(!load_text(&g, "###\n"));
    CHECK(!load_text(&g, "######\n####\n######\n"));
    strcpy(changed, valid_map); changed[0] = '?'; CHECK(!load_text(&g, changed));
    strcpy(changed, valid_map); changed[14] = '1'; CHECK(!load_text(&g, changed));
    strcpy(changed, valid_map); changed[35] = ' '; changed[36] = ' '; CHECK(!load_text(&g, changed));
    CHECK(!load_text(&g, "##########\n##P0@-()##\n##########\n  P1P2P3P4\nP5        \n"));
    CHECK(load_text(&g, "##########\r\n##P0@-()##\r\n##P1P2P3##\r\n  P4P5P6##\r\n##########"));
    g.turn = 13;
    CHECK(snapshot_write(&g, 0, path));
    file = fopen(path, "r"); CHECK(file != NULL);
    if (file) { CHECK(snapshot_read(file, &s)); fclose(file); CHECK(s.turn == 13 && s.self == 0 && s.key.x == 2 && s.key.y == 1 && s.players[5].hp == 2); }
    CHECK(worker_command(0, path, log_path) == COMMAND_PICK_RIGHT);
    file = fopen(log_path, "r"); CHECK(file != NULL);
    if (file) { CHECK(fgetc(file) == EOF); fclose(file); }
    DeleteFileA(path); DeleteFileA(log_path);
}

/* Exercise actual worker decisions: escape immediately when safe, or drop a key
   under pressure and reclaim it during the following simulated turns. */
static void test_player5_tactics(void)
{
    Game g;
    Command c[PLAYER_COUNT];
    char directory[MAX_PATH], path[MAX_PATH], log_path[MAX_PATH];
    int x, y, i, turn, dropped = 0, reclaimed = 0, combat = 0;
    DWORD length = GetTempPathA(sizeof(directory), directory);
    CHECK(length > 0 && length < sizeof(directory));
    if (!length || length >= sizeof(directory)) return;
    if (!GetTempFileNameA(directory, "p5s", 0, path)) { CHECK(0); return; }
    if (!GetTempFileNameA(directory, "p5l", 0, log_path)) { CHECK(0); DeleteFileA(path); return; }

    arena(&g, c);
    place(&g, 5, 5, 6, 2); g.key_owner = 5;
    CHECK(snapshot_write(&g, 5, path));
    c[5] = worker_command(5, path, log_path);
    CHECK(c[5] == COMMAND_RIGHT);
    game_resolve(&g, c);
    CHECK(g.winner == 5);

    /* A one-HP P4 must dodge a fatal trade rather than attack mechanically. */
    arena(&g, c);
    memset(g.terrain, '#', sizeof(g.terrain));
    for (x = 1; x <= 5; ++x) g.terrain[3][x] = ' ';
    g.exit.x = 5; g.exit.y = 3; g.terrain[3][5] = 'E';
    g.key.x = g.key_start.x = 4; g.key.y = g.key_start.y = 3;
    place(&g, 4, 3, 3, 1); place(&g, 5, 2, 3, 2);
    CHECK(snapshot_write(&g, 4, path));
    c[4] = worker_command(4, path, log_path);
    CHECK(c[4] == COMMAND_RIGHT);
    CHECK(snapshot_write(&g, 5, path));
    c[5] = worker_command(5, path, log_path);
    CHECK(c[5] == COMMAND_RIGHT || c[5] == COMMAND_ATTACK_RIGHT);
    game_resolve(&g, c);
    CHECK(g.players[4].hp == 1 && g.players[5].hp == 2);

    /* With enough HP, P4 finishes the carrier and survives the counterattack. */
    place(&g, 4, 3, 3, 2); place(&g, 5, 2, 3, 1); g.key_owner = 5;
    CHECK(snapshot_write(&g, 4, path));
    c[4] = worker_command(4, path, log_path);
    CHECK(c[4] == COMMAND_ATTACK_LEFT);
    c[5] = COMMAND_ATTACK_RIGHT;
    game_resolve(&g, c);
    CHECK(g.players[4].hp == 1 && g.players[5].hp == 0 && g.key_owner == -1);

    /* A diagonal exit defender can kill a one-HP carrier on the escape tile. */
    for (i = 0; i < 5; ++i) {
        arena(&g, c);
        place(&g, 5, 5, 6, 1); place(&g, i, 6, 5, 2); g.key_owner = 5;
        CHECK(snapshot_write(&g, 5, path));
        c[5] = worker_command(5, path, log_path);
        CHECK(c[5] != COMMAND_RIGHT);
        c[i] = COMMAND_ATTACK_DOWN;
        game_resolve(&g, c);
        CHECK(g.players[5].hp > 0 && g.winner < 0 && !g.cancelled[5]);
    }

    arena(&g, c);
    for (y = 0; y < g.size; ++y)
        for (x = 0; x < g.size; ++x)
            g.terrain[y][x] = x == 0 || y == 0 || x == 6 || y == 6 ? '#' : ' ';
    g.turn = 12; g.exit.x = 5; g.exit.y = 1; g.terrain[1][5] = 'E';
    g.key.x = g.key_start.x = 3; g.key.y = g.key_start.y = 3;
    g.key_owner = 5;
    place(&g, 0, 2, 1, 2); g.players[0].spawn = g.players[0].pos;
    place(&g, 1, 5, 5, 2); g.players[1].spawn = g.players[1].pos;
    place(&g, 2, 5, 3, 2); g.players[2].spawn = g.players[2].pos;
    place(&g, 5, 2, 4, 1); g.players[5].spawn = g.players[5].pos;
    for (turn = 0; turn < 48 && g.winner < 0 && !reclaimed; ++turn) {
        Game before = g;
        /* Publish the same state to each worker before resolving any command. */
        for (i = 0; i < PLAYER_COUNT; ++i) {
            c[i] = COMMAND_ATTACK_UP;
            if (g.players[i].hp <= 0) continue;
            CHECK(snapshot_write(&g, i, path));
            c[i] = worker_command(i, path, log_path);
        }
        CHECK(c[5] >= COMMAND_UP && c[5] < COMMAND_COUNT);
        game_resolve(&g, c);
        if (before.players[5].hp > 0 && g.players[5].hp <= 0) {
            fprintf(stderr, "Tactical death at step %d\n", turn);
            for (i = 0; i < PLAYER_COUNT; ++i)
                fprintf(stderr, "P%d (%d,%d) HP%d command%d -> (%d,%d) HP%d\n", i,
                    before.players[i].pos.x, before.players[i].pos.y, before.players[i].hp, (int)c[i],
                    g.players[i].pos.x, g.players[i].pos.y, g.players[i].hp);
        }
        if (c[5] >= COMMAND_DROP_UP && c[5] <= COMMAND_DROP_RIGHT &&
            !g.cancelled[5] && g.key_owner == -1) dropped = 1;
        if (dropped && g.key_owner == 5 && g.players[5].hp > 0) reclaimed = 1;
        CHECK(g.players[5].deaths == 0);
        if (!reclaimed) game_begin_turn(&g);
    }
    CHECK(!dropped || reclaimed);

    /* In a forced, favorable confrontation the real workers must fight and
       finish the escape, rather than waiting or manipulating the key. */
    arena(&g, c);
    memset(g.terrain, '#', sizeof(g.terrain));
    for (x = 1; x <= 5; ++x) g.terrain[3][x] = ' ';
    g.exit.x = 5; g.exit.y = 3; g.terrain[3][5] = 'E';
    g.key.x = g.key_start.x = 2; g.key.y = g.key_start.y = 3;
    place(&g, 2, 4, 3, 1); g.players[2].spawn.x = 1; g.players[2].spawn.y = 3;
    place(&g, 5, 3, 3, 2); g.players[5].spawn.x = 2; g.players[5].spawn.y = 3;
    g.key_owner = 5;
    for (turn = 0; turn < 40 && g.winner < 0; ++turn) {
        game_begin_turn(&g);
        for (i = 0; i < PLAYER_COUNT; ++i) {
            c[i] = COMMAND_ATTACK_UP;
            if (g.players[i].hp <= 0) continue;
            CHECK(snapshot_write(&g, i, path));
            c[i] = worker_command(i, path, log_path);
        }
        game_resolve(&g, c);
        if ((c[2] >= COMMAND_ATTACK_UP && c[2] <= COMMAND_ATTACK_RIGHT && g.damaged[5]) ||
            (c[5] >= COMMAND_ATTACK_UP && c[5] <= COMMAND_ATTACK_RIGHT && g.damaged[2])) ++combat;
    }
    CHECK(combat > 0 && g.winner == 5);
    DeleteFileA(path); DeleteFileA(log_path);
}

/* P2 fights first, but must also claim keys and complete its own escape. */
static void test_player2_guard(void)
{
    Game g;
    Command c[PLAYER_COUNT];
    char directory[MAX_PATH], path[MAX_PATH], log_path[MAX_PATH];
    int turn;
    DWORD length = GetTempPathA(sizeof(directory), directory);
    CHECK(length > 0 && length < sizeof(directory));
    if (!length || length >= sizeof(directory)) return;
    if (!GetTempFileNameA(directory, "p2s", 0, path)) { CHECK(0); return; }
    if (!GetTempFileNameA(directory, "p2l", 0, log_path)) { CHECK(0); DeleteFileA(path); return; }

    arena(&g, c);
    g.terrain[6][6] = ' '; g.exit.x = 3; g.exit.y = 1; g.terrain[1][3] = 'E';
    g.terrain[2][3] = '#';
    g.key.x = 2; g.key.y = 3;
    place(&g, 2, 3, 3, 2);
    CHECK(snapshot_write(&g, 2, path));
    CHECK(worker_command(2, path, log_path) == COMMAND_LEFT);

    g.key.x = 1; g.key.y = 1;
    CHECK(snapshot_write(&g, 2, path));
    CHECK(worker_command(2, path, log_path) == COMMAND_LEFT);

    g.terrain[2][3] = ' ';
    place(&g, 2, 3, 2, 2);
    g.key.x = 4; g.key.y = 2;
    place(&g, 5, 6, 6, 2); g.key_owner = 5;
    for (turn = 0; turn < 3; ++turn) {
        CHECK(snapshot_write(&g, 2, path));
        c[2] = worker_command(2, path, log_path);
        CHECK(c[2] == COMMAND_ATTACK_UP);
        game_resolve(&g, c);
        CHECK(g.players[2].pos.x == 3 && g.players[2].pos.y == 2);
        CHECK(g.players[2].hp == 2 && g.key_owner == 5);
        game_begin_turn(&g);
    }
    /* A carrier in a diagonal fan takes priority over a different low-HP target. */
    place(&g, 5, 4, 3, 2); g.key_owner = 5;
    place(&g, 0, 2, 1, 1);
    CHECK(snapshot_write(&g, 2, path));
    c[2] = worker_command(2, path, log_path);
    CHECK(c[2] >= COMMAND_ATTACK_UP && c[2] <= COMMAND_ATTACK_RIGHT);

    /* Even with an adjacent loose key, a live target takes attack priority. */
    g.key_owner = -1;
    g.players[0].hp = 0;
    CHECK(snapshot_write(&g, 2, path));
    c[2] = worker_command(2, path, log_path);
    CHECK(c[2] == COMMAND_PICK_RIGHT || (c[2] >= COMMAND_ATTACK_UP && c[2] <= COMMAND_ATTACK_RIGHT));
    g.players[5].hp = 0;
    CHECK(snapshot_write(&g, 2, path));
    c[2] = worker_command(2, path, log_path);
    CHECK(c[2] == COMMAND_PICK_RIGHT);
    game_resolve(&g, c);
    CHECK(g.key_owner == 2);
    CHECK(snapshot_write(&g, 2, path));
    c[2] = worker_command(2, path, log_path);
    CHECK(c[2] == COMMAND_UP);
    game_resolve(&g, c);
    CHECK(g.winner == 2);

    /* A distant loose key must not pull P2 away from an established post. */
    arena(&g, c);
    g.terrain[6][6] = ' '; g.exit.x = 3; g.exit.y = 1; g.terrain[1][3] = 'E';
    place(&g, 2, 3, 2, 2);
    g.key.x = 1; g.key.y = 5;
    CHECK(snapshot_write(&g, 2, path));
    CHECK(worker_command(2, path, log_path) == COMMAND_ATTACK_UP);

    /* A key under the guard's feet still permits a sidestep, pickup and escape. */
    g.key = g.key_start = g.players[2].pos;
    for (turn = 0; turn < 8 && g.winner < 0; ++turn) {
        game_begin_turn(&g);
        CHECK(snapshot_write(&g, 2, path));
        c[2] = worker_command(2, path, log_path);
        game_resolve(&g, c);
        CHECK(!g.cancelled[2]);
    }
    CHECK(g.winner == 2);
    DeleteFileA(path); DeleteFileA(log_path);
}

/* Verify the decisions that used to cause stalled paths and pointless fights. */
static void test_smart_rivals(void)
{
    Game g;
    Command c[PLAYER_COUNT];
    char directory[MAX_PATH], path[MAX_PATH], log_path[MAX_PATH];
    int x;
    DWORD length = GetTempPathA(sizeof(directory), directory);
    CHECK(length > 0 && length < sizeof(directory));
    if (!length || length >= sizeof(directory)) return;
    if (!GetTempFileNameA(directory, "ais", 0, path)) { CHECK(0); return; }
    if (!GetTempFileNameA(directory, "ail", 0, log_path)) { CHECK(0); DeleteFileA(path); return; }

    /* A far occupied guard tile must not make P2 stop three tiles away. */
    arena(&g, c);
    memset(g.terrain, '#', sizeof(g.terrain));
    for (x = 1; x <= 5; ++x) g.terrain[3][x] = ' ';
    g.exit.x = 5; g.exit.y = 3; g.terrain[3][5] = 'E';
    g.key.x = g.key_start.x = 1; g.key.y = g.key_start.y = 3;
    place(&g, 2, 1, 3, 2); place(&g, 0, 4, 3, 2);
    CHECK(snapshot_write(&g, 2, path));
    CHECK(worker_command(2, path, log_path) == COMMAND_RIGHT);

    /* A low-health guard dodges a fatal reply, rather than picking up bait. */
    arena(&g, c);
    g.terrain[6][6] = ' '; g.exit.x = 3; g.exit.y = 1; g.terrain[1][3] = 'E';
    g.key.x = 4; g.key.y = 2;
    place(&g, 2, 3, 2, 1); place(&g, 5, 4, 3, 2);
    CHECK(snapshot_write(&g, 2, path));
    c[2] = worker_command(2, path, log_path);
    CHECK(c[2] < COMMAND_ATTACK_UP);
    c[5] = COMMAND_ATTACK_UP;
    game_resolve(&g, c);
    CHECK(g.players[2].hp == 1 && !g.cancelled[2]);

    /* A carrier escapes a needless fight when movement avoids the attack fan. */
    arena(&g, c);
    g.terrain[6][6] = ' '; g.exit.x = 3; g.exit.y = 1; g.terrain[1][3] = 'E';
    place(&g, 4, 3, 3, 2); place(&g, 0, 4, 4, 2); g.key_owner = 4;
    CHECK(snapshot_write(&g, 4, path));
    c[4] = worker_command(4, path, log_path);
    CHECK(c[4] == COMMAND_UP);
    c[0] = COMMAND_ATTACK_UP;
    game_resolve(&g, c);
    CHECK(g.players[4].hp == 2 && g.players[4].pos.y == 2);
    CHECK(snapshot_write(&g, 4, path));
    c[4] = worker_command(4, path, log_path);
    game_resolve(&g, c);
    CHECK(g.winner == 4);
    DeleteFileA(path); DeleteFileA(log_path);
}

/* Regress the observed item loop and a forced, survivable P2/P5 exchange. */
static void test_combat_and_key_intent(void)
{
    static const char terrain[13][14] = {
        "#############", "#.....#.....#", "#.##..#..##.#", "#.....#.....#",
        "#.##.....##.#", "#...........#", "###.#####.###", "#.....E.....#",
        "#.##.....##.#", "#.....#.....#", "#.##..#..##.#", "#...........#", "#############"
    };
    const int px[6] = {9, 5, 4, 7, 8, 4}, py[6] = {5, 9, 7, 3, 4, 4};
    const int sx[6] = {11, 1, 1, 11, 11, 1}, sy[6] = {9, 11, 9, 1, 11, 1};
    const int hp[6] = {0, 2, 1, 1, 1, 2}, deaths[6] = {3, 1, 0, 2, 1, 0};
    Game g;
    Command c[PLAYER_COUNT];
    char directory[MAX_PATH], path[MAX_PATH], log_path[MAX_PATH];
    int x, y, i, turn;
    DWORD length = GetTempPathA(sizeof(directory), directory);
    CHECK(length > 0 && length < sizeof(directory));
    if (!length || length >= sizeof(directory)) return;
    if (!GetTempFileNameA(directory, "ais", 0, path)) { CHECK(0); return; }
    if (!GetTempFileNameA(directory, "ail", 0, log_path)) { CHECK(0); DeleteFileA(path); return; }

    arena(&g, c); g.size = 13; g.turn = 37;
    for (y = 0; y < 13; ++y)
        for (x = 0; x < 13; ++x) g.terrain[y][x] = terrain[y][x] == '.' ? ' ' : terrain[y][x];
    for (i = 0; i < PLAYER_COUNT; ++i) {
        place(&g, i, px[i], py[i], hp[i]);
        g.players[i].spawn.x = sx[i]; g.players[i].spawn.y = sy[i];
        g.players[i].deaths = deaths[i];
    }
    g.players[0].rest = 2;
    g.key.x = g.key_start.x = 6; g.key.y = g.key_start.y = 5;
    g.exit.x = 6; g.exit.y = 7; g.key_owner = 5;
    CHECK(snapshot_write(&g, 5, path));
    c[5] = worker_command(5, path, log_path);
    CHECK(c[5] >= COMMAND_UP && c[5] < COMMAND_DROP_UP);

    /* Quiet possession must lead to escape, without drop/pickup busywork. */
    arena(&g, c); place(&g, 5, 1, 1, 2); g.key_owner = 5;
    for (turn = 0; turn < 14 && g.winner < 0; ++turn) {
        game_begin_turn(&g);
        CHECK(snapshot_write(&g, 5, path));
        c[5] = worker_command(5, path, log_path);
        CHECK(c[5] < COMMAND_DROP_UP);
        game_resolve(&g, c);
        CHECK(g.key_owner == 5);
    }
    CHECK(g.winner == 5);

    /* A fight elsewhere is not a reason to drop and reclaim our own key. */
    arena(&g, c); place(&g, 5, 1, 1, 2); g.key_owner = 5;
    place(&g, 0, 4, 4, 1); place(&g, 3, 4, 5, 1);
    CHECK(snapshot_write(&g, 5, path));
    c[5] = worker_command(5, path, log_path);
    CHECK(c[5] >= COMMAND_UP && c[5] < COMMAND_DROP_UP);
    c[0] = COMMAND_ATTACK_DOWN; c[3] = COMMAND_ATTACK_UP;
    game_resolve(&g, c);
    CHECK(g.players[0].hp == 0 && g.players[3].hp == 0 && g.key_owner == 5);

    arena(&g, c); memset(g.terrain, '#', sizeof(g.terrain));
    for (x = 1; x <= 5; ++x) g.terrain[3][x] = ' ';
    g.exit.x = 5; g.exit.y = 3; g.terrain[3][5] = 'E';
    place(&g, 2, 4, 3, 1); g.players[2].spawn.x = 1; g.players[2].spawn.y = 3;
    place(&g, 5, 3, 3, 2); g.key_owner = 5;
    CHECK(snapshot_write(&g, 2, path)); c[2] = worker_command(2, path, log_path);
    CHECK(snapshot_write(&g, 5, path)); c[5] = worker_command(5, path, log_path);
    CHECK(c[2] == COMMAND_ATTACK_LEFT && c[5] == COMMAND_ATTACK_RIGHT);
    game_resolve(&g, c);
    CHECK(g.players[2].hp == 0 && g.players[5].hp == 1 && g.key_owner == 5);

    /* A healthy exit defender should land a nonfatal hit instead of retreating. */
    arena(&g, c);
    g.terrain[6][6] = ' '; g.exit.x = 3; g.exit.y = 1; g.terrain[1][3] = 'E';
    g.key.x = 1; g.key.y = 5;
    place(&g, 2, 3, 2, 2); place(&g, 5, 4, 3, 2);
    CHECK(snapshot_write(&g, 2, path));
    c[2] = worker_command(2, path, log_path);
    CHECK(c[2] == COMMAND_ATTACK_DOWN);
    c[5] = COMMAND_ATTACK_UP;
    game_resolve(&g, c);
    CHECK(g.players[2].hp == 1 && g.players[5].hp == 1);

    /* Release forces a key-first rival to spend a turn picking up instead of
       delivering the fatal second hit. P5 can finish it and reclaim the key. */
    arena(&g, c); memset(g.terrain, '#', sizeof(g.terrain));
    g.terrain[3][3] = g.terrain[3][4] = g.terrain[4][4] = ' ';
    for (x = 1; x <= 4; ++x) g.terrain[5][x] = ' ';
    g.exit.x = 5; g.exit.y = 3; g.terrain[3][5] = 'E';
    g.key.x = g.key_start.x = 4; g.key.y = g.key_start.y = 3;
    place(&g, 0, 4, 4, 2); g.players[0].spawn.x = 1; g.players[0].spawn.y = 5;
    place(&g, 5, 3, 3, 2); g.players[5].spawn = g.players[5].pos;
    g.key_owner = 5;
    for (turn = 0; turn < 8 && g.winner < 0; ++turn) {
        game_begin_turn(&g);
        for (i = 0; i < PLAYER_COUNT; ++i) {
            c[i] = COMMAND_ATTACK_UP;
            if (g.players[i].hp <= 0) continue;
            CHECK(snapshot_write(&g, i, path));
            c[i] = worker_command(i, path, log_path);
        }
        if (turn == 0) CHECK(c[5] == COMMAND_DROP_RIGHT);
        game_resolve(&g, c);
        CHECK(g.players[5].deaths == 0);
    }
    CHECK(g.winner == 5 && g.players[0].deaths > 0);
    DeleteFileA(path); DeleteFileA(log_path);
}

/* Every strategy must obtain a loose key and reach the exit under normal rules. */
static void test_all_players_escape(void)
{
    Game g;
    Command c[PLAYER_COUNT];
    char directory[MAX_PATH], path[MAX_PATH], log_path[MAX_PATH];
    int id, turn, x, y;
    DWORD length = GetTempPathA(sizeof(directory), directory);
    CHECK(length > 0 && length < sizeof(directory));
    if (!length || length >= sizeof(directory)) return;
    if (!GetTempFileNameA(directory, "ais", 0, path)) { CHECK(0); return; }
    if (!GetTempFileNameA(directory, "ail", 0, log_path)) { CHECK(0); DeleteFileA(path); return; }
    for (id = 0; id < PLAYER_COUNT; ++id) {
        arena(&g, c);
        place(&g, id, 4, 5, 2);
        for (turn = 0; turn < 12 && g.winner < 0; ++turn) {
            game_begin_turn(&g);
            CHECK(snapshot_write(&g, id, path));
            c[id] = worker_command(id, path, log_path);
            if (turn == 0 && id != 2) CHECK(c[id] == COMMAND_PICK_RIGHT);
            game_resolve(&g, c);
            CHECK(!g.cancelled[id] && g.players[id].deaths == 0);
        }
        if (g.winner != id) fprintf(stderr, "Escape stalled: P%d at (%d,%d), key_owner=%d, command=%d\n",
            id, g.players[id].pos.x, g.players[id].pos.y, g.key_owner, (int)c[id]);
        CHECK(g.winner == id);
    }
    /* The real parent enforces the 1500 ms worker budget, even on a 32x32 map. */
    arena(&g, c);
    g.size = MAP_MAX;
    for (y = 0; y < g.size; ++y)
        for (x = 0; x < g.size; ++x)
            g.terrain[y][x] = x == 0 || y == 0 || x == g.size - 1 || y == g.size - 1 ? '#' : ' ';
    g.key.x = g.key_start.x = 16; g.key.y = g.key_start.y = 16;
    g.exit.x = 30; g.exit.y = 30; g.terrain[30][30] = 'E';
    for (id = 0; id < PLAYER_COUNT; ++id) place(&g, id, id * 4 + 1, 1, 2);
    CHECK(snapshot_write(&g, 5, path));
    c[5] = worker_command(5, path, log_path);
    CHECK(c[5] >= COMMAND_UP && c[5] < COMMAND_COUNT);
    DeleteFileA(path); DeleteFileA(log_path);
}

/* Instructor regression entry point; normal gameplay never executes test fixtures. */
int self_test(void)
{
    test_collisions(); test_attacks(); test_items(); test_respawn(); test_files();
    test_player5_tactics();
    test_player2_guard();
    test_smart_rivals();
    test_combat_and_key_intent();
    test_all_players_escape();
    printf("Self-test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
