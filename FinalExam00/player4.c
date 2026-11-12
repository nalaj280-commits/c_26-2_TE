#include "tactics.h"

/* Compete for the key and escape using the interceptor policy. */
Command player4_choose_command(const char *state_path)
{
    Snapshot s;
    FILE *file = fopen(state_path, "r");
    int ok;
    if (!file) return COMMAND_ATTACK_UP;
    ok = snapshot_read(file, &s);
    fclose(file);
    if (!ok) return COMMAND_ATTACK_UP;
    return tactics_choose(&s, strategy_choose(&s, STRATEGY_INTERCEPTOR), 0);
}
