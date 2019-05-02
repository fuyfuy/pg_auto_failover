/*
 * src/bin/pg_autoctl/keeper_init.h
 *     Keeper configuration data structure and function definitions
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#ifndef KEEPER_INIT_H
#define KEEPER_INIT_H

#include <stdbool.h>

#include "keeper.h"
#include "keeper_config.h"

extern bool keeperInitWarnings;

bool keeper_pg_init(Keeper *keeper, KeeperConfig *config);
bool keeper_pg_init_continue(Keeper *keeper, KeeperConfig *config);

#endif /* KEEPER_INIT_H */
