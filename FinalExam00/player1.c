#include "strategy.h"

/* Compete for the key and escape using the cautious policy. */
Command player1_choose_command(const char *state_path)
{
    Snapshot s;
    FILE *file = fopen(state_path, "r");
    int ok;
    if (!file) return COMMAND_ATTACK_UP;
    ok = snapshot_read(file, &s);
    fclose(file);
    if (!ok) return COMMAND_ATTACK_UP;
    return strategy_choose(&s, STRATEGY_CAUTIOUS);
}
