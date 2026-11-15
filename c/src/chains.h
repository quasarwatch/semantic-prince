/*
 * chains.h - "spg chains" subcommand: 3-word chain beam generator.
 *
 * Port of src/genchains.py (post bugfixes_v1/v2). See chains.c for the
 * full contract (scoring, beam, tie order, ported bugfixes).
 */

#ifndef CHAINS_H
#define CHAINS_H

int cmd_chains(int argc, char **argv);

#endif
