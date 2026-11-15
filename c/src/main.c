/*
 * main.c - spg: semantic prince generator, single binary with subcommands.
 */

#include <stdio.h>
#include <string.h>

#include "chains.h"
#include "targeted.h"
#include "evaluate.h"

int cmd_dump(int argc, char **argv);
int cmd_struct(int argc, char **argv);
int cmd_pairs(int argc, char **argv);

static int cmd_stub(const char *name)
{
  fprintf(stderr, "spg %s: not implemented yet\n", name);

  return 2;
}

static void usage(FILE *f)
{
  fprintf(f,
    "usage: spg <command> [args]\n"
    "\n"
    "  dump      UNI BI K [minlen maxlen]   sha256 of model arrays (parity gate)\n"
    "  pairs     UNI BI K freq|pwbi LIMIT [alpha] [--no-dedup]\n"
    "  struct    UNI BI TPL DIG SYM K LIMIT [alpha] [budget|global] [ntpl]\n"
    "            [--no-dedup]                template word/digit/symbol runs\n"
    "  chains    UNI BI K LIMIT [alpha] [B] [T] [lmin] [lmax] [lam]\n"
    "  targeted  UNI BI TEST [NP] [K] [--service=N] [--personal=F] [--deleet]\n"
    "            [--emit[=w1,w2,...]]\n"
    "  evaluate  TEST LIMIT [candidates...]   score candidates vs test set\n");
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    usage(stderr);
    return 2;
  }

  const char *cmd = argv[1];

  if (!strcmp(cmd, "dump"))
    return cmd_dump(argc - 2, argv + 2);

  if (!strcmp(cmd, "pairs"))
    return cmd_pairs(argc - 2, argv + 2);

  if (!strcmp(cmd, "chains"))
    return cmd_chains(argc - 2, argv + 2);

  if (!strcmp(cmd, "targeted"))
    return spg_targeted(argc - 2, argv + 2);

  if (!strcmp(cmd, "struct"))
    return cmd_struct(argc - 2, argv + 2);

  if (!strcmp(cmd, "evaluate"))
    return spg_evaluate(argc - 1, argv + 1);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h"))
  {
    usage(stdout);
    return 0;
  }

  fprintf(stderr, "spg: unknown command '%s'\n", cmd);
  usage(stderr);

  return 2;
}
